#include "scripttrace.h"

#include <Tempest/Log>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace Tempest;

namespace {
  // The format of the events; bump on any change a reader would notice. A trace opened in
  // gameplay mode is version 2 (`sync`, `vm_end`, `mark`, `ret`/`subject` on `enter`); without
  // it the trace is exactly version 1's.
  constexpr uint32_t Version = 1;
  constexpr uint32_t GameplayVersion = 2;

  // The last values a sync wrote (gameplay mode): ints, floats and func values as their 32 bits,
  // strings, instances by address (a freed address reused for an equal-looking object is missed).
  struct Shadow {
    bool                                         synced = false;
    std::vector<uint32_t>                        nums;
    std::vector<std::string>                     strs;
    std::vector<const zenkit::DaedalusInstance*> insts;
    };

  struct Known {
    std::weak_ptr<zenkit::DaedalusInstance> ref;
    int64_t                                 ordinal = 0;
    // Seen only as a raw pointer: can't tell when it dies, so it keeps its ordinal.
    bool                                    raw = false;
    zenkit::DaedalusVm*                     vm = nullptr;
    Shadow                                  shadow;
    };

  // One member or global of a sync: a symbol and how many values it holds.
  struct Slot {
    zenkit::DaedalusSymbol*  sym = nullptr;
    zenkit::DaedalusDataType type = zenkit::DaedalusDataType::VOID;
    uint16_t                 count = 0;
    // Members: -1 not tried yet, 0 the engine didn't register it (ZenKit throws on access), 1 readable.
    int8_t                   readable = -1;
    };

  struct VmSync {
    bool                                        built = false;
    std::vector<Slot>                           globals;
    Shadow                                      shadow;
    // By class symbol index.
    std::unordered_map<uint32_t, std::vector<Slot>> classes;
    };

  // Counted per VM, so a VM used from another thread (lazy ParticleFx definitions) can't
  // shift the numbers of the others.
  struct Counters {
    uint64_t seq     = 0;
    uint64_t entries = 0;
    int64_t  nextOrdinal = 0;
    };

  struct State {
    std::mutex   sync;
    std::FILE*   file = nullptr;
    std::string  path;
    uint64_t     failed  = 0;
    uint64_t     seq     = 0;
    uint64_t     entries = 0;
    int64_t      instanceCount = 0;
    std::unordered_map<std::string, Counters> vms;
    // By the VM's DAT name (the address of a freed definition VM gets reused).
    std::unordered_map<const zenkit::DaedalusVm*, std::string> names;
    std::unordered_map<const zenkit::DaedalusInstance*, Known> instances;
    bool         gameplay = false;
    std::unordered_map<const zenkit::DaedalusVm*, VmSync> vmSync;
    // The thread that last ran an entry on each VM: tick() reads only its own thread's VMs
    // (lazy ParticleFx definitions run on a loader thread).
    std::unordered_map<const zenkit::DaedalusVm*, std::thread::id> lastThread;
    };

  State& state() {
    static State s;
    return s;
    }

  std::atomic_bool isActive{false};
  // Entries and externals in progress on this thread: 0 = a top-level engine -> VM call.
  thread_local uint32_t depth = 0;
  // Instances encoded on this thread since the last Entry/Ext took them: the arguments and
  // results a sync covers (gameplay mode).
  thread_local std::vector<const zenkit::DaedalusInstance*> encoded;
  // The targets of the entries running on this thread (innermost last): every instance taken
  // while an entry runs (an external's arguments and result too) is checked at its `return`,
  // since the entry's code may have written it (`n = Hlp_GetNpc(X); n.aivar[5] = 1;`).
  thread_local std::vector<std::vector<const zenkit::DaedalusInstance*>*> openEntries;

  void addTarget(std::vector<const zenkit::DaedalusInstance*>& v, const zenkit::DaedalusInstance* p) {
    if(p!=nullptr && std::find(v.begin(), v.end(), p)==v.end())
      v.push_back(p);
    }

  // Bytes >= 0x80 as \u00XX (Latin-1), exactly as og-zkexec writes them.
  std::string jsonString(std::string_view s) {
    std::string out = "\"";
    for(unsigned char c:s) {
      if(c=='"' || c=='\\') {
        out += '\\';
        out += char(c);
        }
      else if(c<0x20 || c>=0x7F) {
        char buf[8];
        std::snprintf(buf,sizeof(buf),"\\u%04x",c);
        out += buf;
        }
      else {
        out += char(c);
        }
      }
    out += '"';
    return out;
    }

  std::string symbolName(const zenkit::DaedalusSymbol* sym) {
    return sym==nullptr ? std::string("null") : jsonString(sym->name());
    }

  // Caller holds s.sync.
  void writeLine(State& s, const std::string& line) {
    if(s.file==nullptr)
      return;
    if(std::fwrite(line.data(),1,line.size(),s.file)!=line.size() || std::fputc('\n',s.file)==EOF)
      s.failed++;
    }

  // Caller holds s.sync.
  int64_t ordinalOf(State& s, const zenkit::DaedalusVm& vm, const zenkit::DaedalusInstance* p,
                    const std::shared_ptr<zenkit::DaedalusInstance>* shared) {
    auto it = s.instances.find(p);
    if(it!=s.instances.end() && (it->second.raw || !it->second.ref.expired()))
      return it->second.ordinal;
    Known k;
    k.ordinal = s.vms[s.names.at(&vm)].nextOrdinal++;
    k.vm      = const_cast<zenkit::DaedalusVm*>(&vm);
    s.instanceCount++;
    if(shared!=nullptr)
      k.ref = *shared; else
      k.raw = true;
    s.instances[p] = k;
    return k.ordinal;
    }

  std::string encodeInstance(zenkit::DaedalusVm& vm, const zenkit::DaedalusInstance* p, const std::shared_ptr<zenkit::DaedalusInstance>* shared) {
    if(p==nullptr)
      return "{\"inst\":-1}";
    int64_t ordinal = 0;
    {
      auto& s = state();
      std::lock_guard<std::mutex> guard(s.sync);
      ordinal = ordinalOf(s,vm,p,shared);
      if(s.gameplay)
        encoded.push_back(p);
    }
    return "{\"inst\":" + std::to_string(ordinal) + ",\"sym\":" + symbolName(vm.find_symbol_by_index(p->symbol_index())) + "}";
    }

  std::string globalValue(zenkit::DaedalusVm& vm, zenkit::DaedalusSymbol* sym) {
    if(sym==nullptr)
      return "null";
    return ScriptTrace::instanceValue(vm, sym->get_instance());
    }

  // Caller holds s.sync.
  std::string instanceRef(State& s, zenkit::DaedalusVm& vm, const std::shared_ptr<zenkit::DaedalusInstance>& p) {
    if(p==nullptr)
      return "{\"inst\":-1}";
    return "{\"inst\":" + std::to_string(ordinalOf(s,vm,p.get(),&p)) + ",\"sym\":" +
           symbolName(vm.find_symbol_by_index(p->symbol_index())) + "}";
    }

  uint32_t bitsOf(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits,&f,sizeof(bits));
    return bits;
    }

  // The variables a VM's sync covers: what --script-state-dump calls globals (GameScript::dumpState),
  // plus every instance symbol (which instance PC_HERO, DIA_… currently is).
  void buildGlobals(zenkit::DaedalusVm& vm, VmSync& v) {
    v.built = true;
    for(auto& sym : const_cast<std::vector<zenkit::DaedalusSymbol>&>(vm.symbols())) {
      auto t = sym.type();
      if(sym.is_member())
        continue;
      if(sym.is_generated() && sym.name()=="$PHOENIX_FAKE_STRINGS")
        continue;
      uint16_t count = uint16_t(sym.count());
      if(t==zenkit::DaedalusDataType::INSTANCE)
        count = 1;
      else if(sym.is_const())
        continue;
      else if(t==zenkit::DaedalusDataType::FUNCTION)
        count = std::min<uint16_t>(count, 1);
      else if(t!=zenkit::DaedalusDataType::INT && t!=zenkit::DaedalusDataType::FLOAT && t!=zenkit::DaedalusDataType::STRING)
        continue;
      if(count==0)
        continue;
      v.globals.push_back(Slot{&sym, t, count, 1});
      }
    }

  std::vector<Slot>& classOf(zenkit::DaedalusVm& vm, VmSync& v, const zenkit::DaedalusInstance& inst) {
    static std::vector<Slot> none;
    auto* cls = vm.find_symbol_by_index(inst.symbol_index());
    while(cls!=nullptr && cls->type()!=zenkit::DaedalusDataType::CLASS)
      cls = vm.find_symbol_by_index(cls->parent());
    if(cls==nullptr)
      return none;
    auto it = v.classes.find(cls->index());
    if(it!=v.classes.end())
      return it->second;
    std::vector<Slot> slots;
    for(auto* m : vm.find_class_members(*cls))
      slots.push_back(Slot{m, m->type(), uint16_t(m->count()), -1});
    return v.classes.emplace(cls->index(), std::move(slots)).first->second;
    }

  // Compares `slots` (read with `context`, null for globals) against `sh`, appends each change
  // as `head,index,value]` to `out` (comma-separated) and updates the shadow.
  void diffSlots(State& s, zenkit::DaedalusVm& vm, std::vector<Slot>& slots, const zenkit::DaedalusInstance* context,
                 Shadow& sh, const std::string& prefix, std::string& out) {
    // `synced` is set only once a whole first pass worked: a pass that threw part way leaves
    // short vectors, which the next pass would index past their end.
    const bool first = !sh.synced;
    if(first) {
      sh.nums.clear();
      sh.strs.clear();
      sh.insts.clear();
      }
    size_t ni = 0, si = 0, ii = 0;
    auto emit = [&](const Slot& sl, uint16_t i, const std::string& value) {
      out += out.empty() ? "" : ",";
      out += prefix + symbolName(sl.sym) + "," + std::to_string(i) + "," + value + "]";
      };
    for(auto& sl : slots) {
      if(sl.readable==-1) {
        try {
          switch(sl.type) {
            case zenkit::DaedalusDataType::FLOAT:  (void)sl.sym->get_float(0, context);  break;
            case zenkit::DaedalusDataType::STRING: (void)sl.sym->get_string(0, context); break;
            case zenkit::DaedalusDataType::INT:
            case zenkit::DaedalusDataType::FUNCTION: (void)sl.sym->get_int(0, context); break;
            default: break;
            }
          sl.readable = 1;
          }
        catch(const zenkit::DaedalusScriptError&) {
          sl.readable = 0;
          }
        }
      if(sl.readable==0)
        continue;
      for(uint16_t i=0; i<sl.count; ++i) {
        switch(sl.type) {
          case zenkit::DaedalusDataType::FLOAT:
          case zenkit::DaedalusDataType::INT:
          case zenkit::DaedalusDataType::FUNCTION: {
            const bool isFloat = sl.type==zenkit::DaedalusDataType::FLOAT;
            uint32_t v = isFloat ? bitsOf(sl.sym->get_float(i, context)) : uint32_t(sl.sym->get_int(i, context));
            if(first)
              sh.nums.push_back(v);
            if(first || sh.nums[ni]!=v) {
              sh.nums[ni] = v;
              float f = 0;
              std::memcpy(&f,&v,sizeof(f));
              emit(sl, i, isFloat ? ScriptTrace::floatValue(f) : ScriptTrace::intValue(int32_t(v)));
              }
            ni++;
            break;
            }
          case zenkit::DaedalusDataType::STRING: {
            const std::string& v = sl.sym->get_string(i, context);
            if(first)
              sh.strs.push_back(v);
            if(first || sh.strs[si]!=v) {
              sh.strs[si] = v;
              emit(sl, i, ScriptTrace::stringValue(v));
              }
            si++;
            break;
            }
          case zenkit::DaedalusDataType::INSTANCE: {
            auto& v = sl.sym->get_instance();
            if(first)
              sh.insts.push_back(v.get());
            if(first || sh.insts[ii]!=v.get()) {
              sh.insts[ii] = v.get();
              emit(sl, i, instanceRef(s, vm, v));
              }
            ii++;
            break;
            }
          default:
            break;
          }
        }
      }
    sh.synced = true;
    }

  // `{"ev":"sync","vm":…,"at":…,"new":[{"inst":…,"sym":…},…],"members":[[inst,"MEMBER",index,value],…],"globals":[["SYM",index,value],…]}`
  // for the changes since the last sync of each instance in `targets` (all live instances of
  // `vm` when `all`) and of `vm`'s globals; nothing when nothing changed. Caller holds s.sync.
  void syncUnguarded(State& s, zenkit::DaedalusVm& vm, const char* at,
                     const std::vector<const zenkit::DaedalusInstance*>& targets, bool all) {
    auto name = s.names.find(&vm);
    if(!s.gameplay || name==s.names.end())
      return;
    auto& v = s.vmSync[&vm];
    if(!v.built)
      buildGlobals(vm, v);
    std::string members, fresh;
    auto syncOne = [&](const zenkit::DaedalusInstance* p, Known& k) {
      // First sync of this instance: a replay that hasn't seen it makes it from its symbol.
      if(!k.shadow.synced) {
        fresh += fresh.empty() ? "" : ",";
        fresh += "{\"inst\":" + std::to_string(k.ordinal) + ",\"sym\":" + symbolName(vm.find_symbol_by_index(p->symbol_index())) + "}";
        }
      auto& slots = classOf(vm, v, *p);
      diffSlots(s, vm, slots, p, k.shadow, "[" + std::to_string(k.ordinal) + ",", members);
      };
    if(all) {
      // By ordinal, so two runs write the same line.
      std::vector<std::pair<int64_t, const zenkit::DaedalusInstance*>> live;
      for(auto& [p,k] : s.instances)
        if(!k.raw && k.vm==&vm && !k.ref.expired())
          live.emplace_back(k.ordinal, p);
      std::sort(live.begin(), live.end());
      for(auto& [ordinal,p] : live) {
        auto& k = s.instances.at(p);
        auto alive = k.ref.lock();
        if(alive!=nullptr)
          syncOne(p, k);
        }
      } else {
      std::vector<const zenkit::DaedalusInstance*> done;
      for(auto* p : targets) {
        if(p==nullptr || std::find(done.begin(),done.end(),p)!=done.end())
          continue;
        done.push_back(p);
        auto it = s.instances.find(p);
        // Raw instances are synced only here, while they are an argument or result (alive).
        if(it==s.instances.end() || it->second.vm!=&vm || (!it->second.raw && it->second.ref.expired()))
          continue;
        syncOne(p, it->second);
        }
      }
    std::string globals;
    diffSlots(s, vm, v.globals, nullptr, v.shadow, "[", globals);
    if(members.empty() && globals.empty() && fresh.empty())
      return;
    writeLine(s, "{\"ev\":\"sync\",\"vm\":" + jsonString(name->second) + ",\"at\":\"" + at +
                 "\",\"new\":[" + fresh + "],\"members\":[" + members + "],\"globals\":[" + globals + "]}");
    }

  // syncUnguarded, but a failure (a ZenKit access error on an instance, bad_alloc) is logged and
  // marks the trace incomplete instead of escaping from a destructor or into the game. Caller holds s.sync.
  void syncLocked(State& s, zenkit::DaedalusVm& vm, const char* at,
                  const std::vector<const zenkit::DaedalusInstance*>& targets, bool all) {
    try {
      syncUnguarded(s, vm, at, targets, all);
      }
    catch(const std::exception& e) {
      s.failed++;
      Log::e("script trace: sync at ", at, " failed: ", e.what());
      }
    }

  // The instances encoded on this thread since the last take, plus the five script globals'
  // instances; also added to every running entry's targets.
  std::vector<const zenkit::DaedalusInstance*> takeTargets(zenkit::DaedalusVm& vm) {
    std::vector<const zenkit::DaedalusInstance*> t;
    t.swap(encoded);
    for(auto* g : {vm.global_self(), vm.global_other(), vm.global_victim(), vm.global_hero(), vm.global_item()})
      if(g!=nullptr)
        t.push_back(g->get_instance().get());
    for(auto* open : openEntries)
      for(auto* p : t)
        addTarget(*open, p);
    return t;
    }
  }

void ScriptTrace::open(const std::string& path, bool gameplay) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file!=nullptr)
    throw std::runtime_error("script trace: already open (" + s.path + ")");
  std::FILE* f = std::fopen(path.c_str(),"wb");
  if(f==nullptr)
    throw std::runtime_error("script trace: can't open \"" + path + "\": " + std::strerror(errno));
  s.file = f;
  s.path = path;
  s.gameplay = gameplay;
  writeLine(s, "{\"ev\":\"trace\",\"format\":\"og-script-trace\",\"version\":" +
               std::to_string(gameplay ? GameplayVersion : Version) + "}");
  if(s.failed>0) {
    std::fclose(f);
    s.file = nullptr;
    throw std::runtime_error("script trace: can't write header to \"" + path + "\"");
    }
  isActive.store(true);
  Log::i("script trace: opened \"", path, "\"", gameplay ? " (gameplay mode)" : "");
  }

void ScriptTrace::stop(std::string_view reason) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file==nullptr)
    return;
  isActive.store(false);
  writeLine(s, "{\"ev\":\"end\",\"reason\":" + jsonString(reason) + ",\"entries\":" + std::to_string(s.entries) +
               ",\"externals\":" + std::to_string(s.seq) + ",\"instances\":" + std::to_string(s.instanceCount) +
               // Gameplay mode only (version 1 stays byte-identical): a replay refuses a trace
               // that lost lines, since a dropped check sync would replay as a match.
               (s.gameplay ? ",\"failed\":" + std::to_string(s.failed) : std::string()) + "}");
  if(std::fclose(s.file)!=0)
    s.failed++;
  s.file = nullptr;
  s.gameplay = false;
  s.instances.clear();
  s.vms.clear();
  s.names.clear();
  s.vmSync.clear();
  s.lastThread.clear();
  if(s.failed>0)
    Log::e("script trace: ", s.failed, " writes to \"", s.path, "\" failed; the trace is incomplete");
  else
    Log::i("script trace: closed \"", s.path, "\" (", reason, "): ", s.entries, " entries, ", s.seq, " external calls");
  }

bool ScriptTrace::active() {
  return isActive.load(std::memory_order_relaxed);
  }

bool ScriptTrace::gameplay() {
  if(!active())
    return false;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  return s.gameplay;
  }

void ScriptTrace::tick() {
  if(!active())
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(!s.gameplay)
    return;
  std::vector<zenkit::DaedalusVm*> vms;
  for(auto& [vm,name] : s.names) {
    auto t = s.lastThread.find(vm);
    if(t!=s.lastThread.end() && t->second!=std::this_thread::get_id())
      continue;
    vms.push_back(const_cast<zenkit::DaedalusVm*>(vm));
    }
  // Same order in every run: by name.
  std::sort(vms.begin(), vms.end(), [&](auto* a, auto* b) { return s.names.at(a) < s.names.at(b); });
  for(auto* vm : vms)
    syncLocked(s, *vm, "tick", {}, true);
  }

void ScriptTrace::mark(uint64_t action, std::string_view tool) {
  if(!active())
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.gameplay)
    writeLine(s, "{\"ev\":\"mark\",\"action\":" + std::to_string(action) + ",\"tool\":" + jsonString(tool) + "}");
  }

void ScriptTrace::patch(std::string_view vmName, std::string_view id, std::string_view symbol, std::string_view kind) {
  if(!active())
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  writeLine(s, "{\"ev\":\"patch\",\"vm\":" + jsonString(vmName) + ",\"id\":" + jsonString(id) +
               ",\"symbol\":" + jsonString(symbol) + ",\"kind\":" + jsonString(kind) + "}");
  }

void ScriptTrace::vmGone(const zenkit::DaedalusVm& vm) {
  if(!active())
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  // Version-1 traces don't know VM ends (and keep their instances numbered as before).
  if(!s.gameplay)
    return;
  auto it = s.names.find(&vm);
  if(it==s.names.end())
    return;
  writeLine(s, "{\"ev\":\"vm_end\",\"vm\":" + jsonString(it->second) + "}");
  for(auto i = s.instances.begin(); i!=s.instances.end();) {
    if(i->second.vm==&vm)
      i = s.instances.erase(i); else
      ++i;
    }
  s.vmSync.erase(&vm);
  s.lastThread.erase(&vm);
  s.names.erase(it);
  }

void ScriptTrace::vm(const zenkit::DaedalusVm& vm, std::string_view vmName, size_t symbols) {
  if(!active())
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  s.names[&vm] = std::string(vmName);
  writeLine(s, "{\"ev\":\"vm\",\"vm\":" + jsonString(vmName) + ",\"symbols\":" + std::to_string(symbols) + "}");
  }

void ScriptTrace::created(const zenkit::DaedalusVm& vm, const std::shared_ptr<zenkit::DaedalusInstance>& instance) {
  if(!active() || instance==nullptr)
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  s.instances.erase(instance.get());
  ordinalOf(s, vm, instance.get(), &instance);
  }

void ScriptTrace::recover(std::string_view vmName, const zenkit::DaedalusScriptError& err, const zenkit::DaedalusInstruction& instr) {
  if(!active())
    return;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  writeLine(s, "{\"ev\":\"recover\",\"vm\":" + jsonString(vmName) + ",\"op\":" + std::to_string(int(instr.op)) +
               ",\"message\":" + jsonString(err.what()) + "}");
  }

std::string ScriptTrace::intValue(int64_t v) {
  return "{\"i\":" + std::to_string(v) + "}";
  }

std::string ScriptTrace::floatValue(float v) {
  uint32_t bits = 0;
  std::memcpy(&bits,&v,sizeof(bits));
  return "{\"f\":" + std::to_string(bits) + "}";
  }

std::string ScriptTrace::stringValue(std::string_view bytes) {
  return "{\"s\":" + jsonString(bytes) + "}";
  }

std::string ScriptTrace::functionValue(const zenkit::DaedalusSymbol* fn) {
  return "{\"fn\":" + symbolName(fn) + "}";
  }

std::string ScriptTrace::referenceValue(const zenkit::DaedalusSymbol* sym) {
  return "{\"ref\":" + symbolName(sym) + "}";
  }

std::string ScriptTrace::instanceValue(zenkit::DaedalusVm& vm, const std::shared_ptr<zenkit::DaedalusInstance>& instance) {
  return encodeInstance(vm, instance.get(), &instance);
  }

std::string ScriptTrace::instanceValue(zenkit::DaedalusVm& vm, const zenkit::DaedalusInstance* instance) {
  return encodeInstance(vm, instance, nullptr);
  }

ScriptTrace::Entry::Entry(zenkit::DaedalusVm& vm, std::string_view vmName, const char* kind,
                          const zenkit::DaedalusSymbol* sym, const std::string& args,
                          const zenkit::DaedalusInstance* subject, const char* ret) : machine(&vm) {
  // The script globals the engine set for this call (SELF, OTHER, ...): a replay sets them first.
  const std::string globals = "{\"self\":"   + globalValue(vm, vm.global_self())   +
                              ",\"other\":"  + globalValue(vm, vm.global_other())  +
                              ",\"victim\":" + globalValue(vm, vm.global_victim()) +
                              ",\"hero\":"   + globalValue(vm, vm.global_hero())   +
                              ",\"item\":"   + globalValue(vm, vm.global_item())   + "}";
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.gameplay) {
    s.lastThread[&vm] = std::this_thread::get_id();
    targets = takeTargets(vm);
    // An init's instance is already allocated (OgVm::init_instance does that first in gameplay
    // mode): its C++ member defaults go out before the code runs. An allocate's instance is typed
    // only inside the entry (ZenKit's member access needs the type): synced from `return` on.
    if(subject!=nullptr && std::string_view(kind)=="init")
      targets.push_back(subject);
    syncLocked(s, vm, "enter", targets, false);
    addTarget(targets, subject);
    }
  index = s.vms[std::string(vmName)].entries++;
  s.entries++;
  this->vmName = vmName;
  // Gameplay mode: the result type the engine asked for (a replay calls the same way) and the
  // instance an init/allocate makes (its ordinal: the replay makes it before the code runs).
  std::string extra;
  if(s.gameplay && ret!=nullptr)
    extra += ",\"ret\":\"" + std::string(ret) + "\"";
  if(s.gameplay && subject!=nullptr) {
    auto it = s.instances.find(subject);
    if(it!=s.instances.end())
      extra += ",\"subject\":" + std::to_string(it->second.ordinal);
    }
  writeLine(s, "{\"ev\":\"enter\",\"entry\":" + std::to_string(index) + ",\"vm\":" + jsonString(vmName) +
               ",\"kind\":\"" + kind + "\",\"symbol\":" + symbolName(sym) + ",\"args\":" + args +
               ",\"depth\":" + std::to_string(depth) + ",\"globals\":" + globals + extra + "}");
  // Last, so a throw above (bad_alloc) can't leave a pointer to this dead object behind.
  if(s.gameplay)
    openEntries.push_back(&targets);
  depth++;
  }

ScriptTrace::Entry::~Entry() {
  depth--;
  if(!openEntries.empty() && openEntries.back()==&targets)
    openEntries.pop_back();
  }

void ScriptTrace::Entry::ret(const std::string& value) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.gameplay) {
    // Adds what was taken since the last take to `targets` (this entry is open).
    takeTargets(*machine);
    syncLocked(s, *machine, "return", targets, false);
    }
  writeLine(s, "{\"ev\":\"return\",\"entry\":" + std::to_string(index) + ",\"vm\":" + jsonString(vmName) + ",\"value\":" + value + "}");
  }

void ScriptTrace::Entry::fail(std::string_view what) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  writeLine(s, "{\"ev\":\"error\",\"entry\":" + std::to_string(index) + ",\"vm\":" + jsonString(vmName) + ",\"message\":" + jsonString(what) + "}");
  }

ScriptTrace::Ext::Ext(zenkit::DaedalusVm& vm, std::string_view vmName, std::string_view symbol, std::string args, bool isDefault)
  : machine(&vm), vmName(vmName), symbol(symbol), args(std::move(args)), isDefault(isDefault) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.gameplay) {
    targets = takeTargets(vm);
    // Every instance an open entry took, not only this call's arguments: the script may have
    // written one of them (`n = Hlp_GetNpc(X); n.aivar[5] = 1;`) and this external may call back
    // into a VM with it, whose `enter` sync a replay applies unchecked.
    for(auto* open : openEntries)
      for(auto* p : *open)
        addTarget(targets, p);
    syncLocked(s, vm, "ext", targets, false);
    }
  seq   = s.vms[this->vmName].seq++;
  s.seq++;
  depth = ::depth;
  ::depth++;
  }

ScriptTrace::Ext::~Ext() {
  ::depth--;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.gameplay) {
    auto t = takeTargets(*machine);
    t.insert(t.end(), targets.begin(), targets.end());
    syncLocked(s, *machine, "extret", t, false);
    }
  std::string line = "{\"ev\":\"ext\",\"seq\":" + std::to_string(seq) + ",\"vm\":" + jsonString(vmName) +
                     ",\"symbol\":" + jsonString(symbol) + ",\"args\":" + args +
                     ",\"ret\":" + (done ? result : std::string("null")) + ",\"depth\":" + std::to_string(depth);
  if(isDefault)
    line += ",\"default\":true";
  if(!done)
    line += ",\"threw\":true";
  writeLine(s, line + "}");
  }

void ScriptTrace::Ext::ret(std::string value) {
  result = std::move(value);
  done   = true;
  }
