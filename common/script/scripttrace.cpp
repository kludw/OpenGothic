#include "scripttrace.h"

#include <Tempest/Log>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using namespace Tempest;

namespace {
  // The format of the events; bump on any change a reader would notice.
  constexpr uint32_t Version = 1;

  struct Known {
    std::weak_ptr<zenkit::DaedalusInstance> ref;
    int64_t                                 ordinal = 0;
    // Seen only as a raw pointer: can't tell when it dies, so it keeps its ordinal.
    bool                                    raw = false;
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
    };

  State& state() {
    static State s;
    return s;
    }

  std::atomic_bool isActive{false};
  // Entries and externals in progress on this thread: 0 = a top-level engine -> VM call.
  thread_local uint32_t depth = 0;

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
    }
    return "{\"inst\":" + std::to_string(ordinal) + ",\"sym\":" + symbolName(vm.find_symbol_by_index(p->symbol_index())) + "}";
    }

  std::string globalValue(zenkit::DaedalusVm& vm, zenkit::DaedalusSymbol* sym) {
    if(sym==nullptr)
      return "null";
    return ScriptTrace::instanceValue(vm, sym->get_instance());
    }
  }

void ScriptTrace::open(const std::string& path) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file!=nullptr)
    throw std::runtime_error("script trace: already open (" + s.path + ")");
  std::FILE* f = std::fopen(path.c_str(),"wb");
  if(f==nullptr)
    throw std::runtime_error("script trace: can't open \"" + path + "\": " + std::strerror(errno));
  s.file = f;
  s.path = path;
  writeLine(s, "{\"ev\":\"trace\",\"format\":\"og-script-trace\",\"version\":" + std::to_string(Version) + "}");
  if(s.failed>0) {
    std::fclose(f);
    s.file = nullptr;
    throw std::runtime_error("script trace: can't write header to \"" + path + "\"");
    }
  isActive.store(true);
  Log::i("script trace: opened \"", path, "\"");
  }

void ScriptTrace::stop(std::string_view reason) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file==nullptr)
    return;
  isActive.store(false);
  writeLine(s, "{\"ev\":\"end\",\"reason\":" + jsonString(reason) + ",\"entries\":" + std::to_string(s.entries) +
               ",\"externals\":" + std::to_string(s.seq) + ",\"instances\":" + std::to_string(s.instanceCount) + "}");
  if(std::fclose(s.file)!=0)
    s.failed++;
  s.file = nullptr;
  s.instances.clear();
  s.vms.clear();
  s.names.clear();
  if(s.failed>0)
    Log::e("script trace: ", s.failed, " writes to \"", s.path, "\" failed; the trace is incomplete");
  else
    Log::i("script trace: closed \"", s.path, "\" (", reason, "): ", s.entries, " entries, ", s.seq, " external calls");
  }

bool ScriptTrace::active() {
  return isActive.load(std::memory_order_relaxed);
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
                          const zenkit::DaedalusSymbol* sym, const std::string& args) {
  // The script globals the engine set for this call (SELF, OTHER, ...): a replay sets them first.
  const std::string globals = "{\"self\":"   + globalValue(vm, vm.global_self())   +
                              ",\"other\":"  + globalValue(vm, vm.global_other())  +
                              ",\"victim\":" + globalValue(vm, vm.global_victim()) +
                              ",\"hero\":"   + globalValue(vm, vm.global_hero())   +
                              ",\"item\":"   + globalValue(vm, vm.global_item())   + "}";
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  index = s.vms[std::string(vmName)].entries++;
  s.entries++;
  this->vmName = vmName;
  writeLine(s, "{\"ev\":\"enter\",\"entry\":" + std::to_string(index) + ",\"vm\":" + jsonString(vmName) +
               ",\"kind\":\"" + kind + "\",\"symbol\":" + symbolName(sym) + ",\"args\":" + args +
               ",\"depth\":" + std::to_string(depth) + ",\"globals\":" + globals + "}");
  depth++;
  }

ScriptTrace::Entry::~Entry() {
  depth--;
  }

void ScriptTrace::Entry::ret(const std::string& value) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  writeLine(s, "{\"ev\":\"return\",\"entry\":" + std::to_string(index) + ",\"vm\":" + jsonString(vmName) + ",\"value\":" + value + "}");
  }

void ScriptTrace::Entry::fail(std::string_view what) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  writeLine(s, "{\"ev\":\"error\",\"entry\":" + std::to_string(index) + ",\"vm\":" + jsonString(vmName) + ",\"message\":" + jsonString(what) + "}");
  }

ScriptTrace::Ext::Ext(std::string_view vmName, std::string_view symbol, std::string args, bool isDefault)
  : vmName(vmName), symbol(symbol), args(std::move(args)), isDefault(isDefault) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  seq   = s.vms[this->vmName].seq++;
  s.seq++;
  depth = ::depth;
  ::depth++;
  }

ScriptTrace::Ext::~Ext() {
  ::depth--;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
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
