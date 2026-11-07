#include "ogvm.h"

#include <Tempest/Log>

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>


static_assert(OG_SCRIPT_FLAG_ALLOW_NULL_INSTANCE_ACCESS==zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
static_assert(OG_SCRIPT_FLAG_IGNORE_CONST_SPECIFIER==zenkit::DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER);

namespace {
  std::mutex        backendSync;
  og_script_backend backend   = {};
  bool              hasBackend = false;

  std::mutex        dumpSync;
  std::ofstream     dumpFile;
  // Set once at start-up, before any VM exists; read on every init_instance.
  std::atomic<bool> dumpOpen = false;

  void setError(char* err, size_t errLen, std::string_view msg) {
    if(err==nullptr || errLen==0)
      return;
    const size_t n = std::min(msg.size(), errLen-1);
    std::memcpy(err, msg.data(), n);
    err[n] = '\0';
    }
  }

// The host side of one VM on the og_script backend (og_script_host*). Heap-allocated so its
// address stays put when the OgVm moves; `owner` follows the moves.
class OgVm::Rust final {
  public:
    struct Pin {
      std::shared_ptr<zenkit::DaedalusInstance> inst;
      uint32_t                                  generation = 0;
      };

    OgVm*                     owner = nullptr;
    void*                     vm    = nullptr;
    std::vector<Pin>          pins;
    std::vector<uint32_t>     freeSlots;
    // Slots pinned since the last releaseAll (a slot released and pinned again is listed
    // twice): releaseAll visits only these, not every slot ever made (task 4.19: the world
    // load pins thousands in one entry, then each later entry pins a handful).
    std::vector<uint32_t>     pinned;
    std::unordered_map<const zenkit::DaedalusInstance*, uint32_t> slotOf;
    // Strings handed to the backend by member_get / call_external, valid until the next call.
    std::string               scratch;
    // Backend entries running (nested ones come from externals). The backend keeps no handle
    // between entries (globals live in ZenKit's symbols), so when the outermost one returns
    // every pin goes: the instances are ZenKit's and the engine's again, as on ZenKit.
    uint32_t                  entries = 0;

    // One backend entry; the outermost releases every pin when it ends.
    struct Entry final {
      explicit Entry(Rust& r) : r(r) { ++r.entries; }
      Entry(const Entry&) = delete;
      ~Entry() {
        if(--r.entries==0)
          r.releaseAll();
        }
      Rust& r;
      };

    og_script_inst pin(std::shared_ptr<zenkit::DaedalusInstance> const& inst) {
      if(inst==nullptr)
        return {0, 0};
      if(auto it = slotOf.find(inst.get()); it!=slotOf.end())
        return {it->second, pins[it->second].generation};
      uint32_t slot;
      if(!freeSlots.empty()) {
        slot = freeSlots.back();
        freeSlots.pop_back();
        } else {
        slot = uint32_t(pins.size());
        pins.push_back({});
        }
      auto& p = pins[slot];
      p.inst = inst;
      // Generation 0 means null; a slot whose generation would wrap is retired.
      p.generation += 1;
      slotOf[inst.get()] = slot;
      pinned.push_back(slot);
      return {slot, p.generation};
      }

    // The pinned instance, or nullptr for the null handle; throws for a stale or unknown one.
    std::shared_ptr<zenkit::DaedalusInstance> const& get(og_script_inst h) {
      static const std::shared_ptr<zenkit::DaedalusInstance> null;
      if(h.slot==0 && h.generation==0)
        return null;
      if(h.slot>=pins.size() || pins[h.slot].generation!=h.generation || pins[h.slot].inst==nullptr)
        throw std::runtime_error("stale instance handle (slot " + std::to_string(h.slot) +
                                 ", generation " + std::to_string(h.generation) + ")");
      return pins[h.slot].inst;
      }

    void release(og_script_inst h) {
      auto& inst = get(h);
      if(inst==nullptr)
        throw std::runtime_error("release of the null instance");
      slotOf.erase(inst.get());
      pins[h.slot].inst.reset();
      if(pins[h.slot].generation!=UINT32_MAX)
        freeSlots.push_back(h.slot);
      }

    void releaseAll() {
      // Taken out first: an instance's destructor may run below and must not touch the list.
      std::vector<uint32_t> slots;
      slots.swap(pinned);
      for(uint32_t slot:slots) {
        auto& p = pins[slot];
        if(p.inst==nullptr)
          continue;
        slotOf.erase(p.inst.get());
        p.inst.reset();
        if(p.generation!=UINT32_MAX)
          freeSlots.push_back(slot);
        }
      // Its capacity back (no allocation per entry), unless something was pinned meanwhile.
      slots.clear();
      if(pinned.empty())
        pinned.swap(slots);
      }

    zenkit::DaedalusSymbol& member(uint32_t index) {
      auto* sym = owner->zk->find_symbol_by_index(index);
      if(sym==nullptr)
        throw std::runtime_error("no symbol " + std::to_string(index));
      if(!sym->is_member())
        throw std::runtime_error(sym->name() + " is not a class member");
      return *sym;
      }

    zenkit::DaedalusSymbol& global(uint32_t index) {
      auto* sym = owner->zk->find_symbol_by_index(index);
      if(sym==nullptr)
        throw std::runtime_error("no symbol " + std::to_string(index));
      if(sym->is_member())
        throw std::runtime_error(sym->name() + " is a class member, not a global");
      return *sym;
      }

    int32_t globalGet(uint32_t index, uint16_t i, uint32_t kind, og_script_value& out) {
      auto& sym = global(index);
      out = {};
      out.kind = kind;
      switch(kind) {
        case OG_SCRIPT_INT:
          out.i = sym.get_int(i);
          return OG_SCRIPT_OK;
        case OG_SCRIPT_FLOAT:
          out.f = sym.get_float(i);
          return OG_SCRIPT_OK;
        case OG_SCRIPT_STRING:
          scratch = sym.get_string(i);
          out.str = scratch.data();
          out.len = scratch.size();
          return OG_SCRIPT_OK;
        case OG_SCRIPT_INSTANCE:
          out.inst = pin(sym.get_instance());
          return OG_SCRIPT_OK;
        }
      throw std::runtime_error("og_script_global_get: no global of kind " + std::to_string(kind));
      }

    int32_t globalSet(uint32_t index, uint16_t i, const og_script_value& v) {
      auto& sym = global(index);
      switch(v.kind) {
        case OG_SCRIPT_INT:
          sym.set_int(v.i, i);
          return OG_SCRIPT_OK;
        case OG_SCRIPT_FLOAT:
          sym.set_float(v.f, i);
          return OG_SCRIPT_OK;
        case OG_SCRIPT_STRING:
          sym.set_string(std::string_view(v.str, v.len), i);
          return OG_SCRIPT_OK;
        case OG_SCRIPT_INSTANCE:
          sym.set_instance(get(v.inst));
          return OG_SCRIPT_OK;
        }
      throw std::runtime_error("og_script_global_set: no global of kind " + std::to_string(v.kind));
      }

    // The trace's encoding of a value the backend hands over (ScriptTrace::value's cases).
    std::string traceValue(const og_script_value& v) {
      switch(v.kind) {
        case OG_SCRIPT_VOID:
          return "null";
        case OG_SCRIPT_INT:
          return ScriptTrace::intValue(v.i);
        case OG_SCRIPT_FLOAT:
          return ScriptTrace::floatValue(v.f);
        case OG_SCRIPT_STRING:
          return ScriptTrace::stringValue(std::string_view(v.str, v.len));
        case OG_SCRIPT_FUNCTION:
          return ScriptTrace::functionValue(owner->zk->find_symbol_by_index(uint32_t(v.i)));
        case OG_SCRIPT_INSTANCE:
          return ScriptTrace::instanceValue(*owner->zk, get(v.inst));
        }
      throw std::runtime_error("og_script_trace_external: no value of kind " + std::to_string(v.kind));
      }

    void traceExternal(std::string_view ext, const og_script_value* args, size_t count, const og_script_value* ret, bool threw) {
      if(!ScriptTrace::active())
        return;
      auto* sym = owner->zk->find_symbol_by_name(ext);
      if(sym==nullptr)
        throw std::runtime_error("og_script_trace_external: no symbol " + std::string(ext));
      std::string values = "[";
      for(size_t i=0; i<count; ++i)
        values += (i==0 ? "" : ",") + traceValue(args[i]);
      values += "]";
      // Same event as OgVm::tracedExternal writes for an engine callback.
      ScriptTrace::Ext trace(*owner->zk, owner->vmName, sym->name(), std::move(values));
      if(!threw)
        trace.ret(ret!=nullptr ? traceValue(*ret) : "null");
      }

    void push(const og_script_value& v) {
      auto& zk = *owner->zk;
      switch(v.kind) {
        case OG_SCRIPT_INT:
        case OG_SCRIPT_FUNCTION:
          zk.push_int(v.i);
          return;
        case OG_SCRIPT_FLOAT:
          zk.push_float(v.f);
          return;
        case OG_SCRIPT_STRING:
          zk.push_string(std::string_view(v.str, v.len));
          return;
        case OG_SCRIPT_INSTANCE:
          zk.push_instance(get(v.inst));
          return;
        case OG_SCRIPT_REFERENCE: {
          auto* sym = zk.find_symbol_by_index(uint32_t(v.i));
          if(sym==nullptr)
            throw std::runtime_error("og_script: reference to no symbol " + std::to_string(v.i));
          if(v.index>UINT8_MAX)
            throw std::runtime_error("og_script: reference to " + sym->name() + " index " + std::to_string(v.index));
          // ZenKit's PUSHV takes the instance register as the reference's context; restored
          // also when the push throws (stack overflow).
          struct Restore final {
            zenkit::DaedalusVm&                       zk;
            std::shared_ptr<zenkit::DaedalusInstance> prev;
            ~Restore() { zk.unsafe_set_gi(std::move(prev)); }
            } restore {zk, zk.unsafe_get_gi()};
          zk.unsafe_set_gi(get(v.inst));
          zk.push_reference(sym, uint8_t(v.index));
          return;
          }
        }
      throw std::runtime_error("og_script: no argument of kind " + std::to_string(v.kind));
      }

    void pop(uint32_t kind, og_script_value& out) {
      auto& zk = *owner->zk;
      out = {};
      out.kind = kind;
      switch(kind) {
        case OG_SCRIPT_VOID:
          return;
        case OG_SCRIPT_INT:
        case OG_SCRIPT_FUNCTION:
          out.i = zk.pop_int();
          return;
        case OG_SCRIPT_FLOAT:
          out.f = zk.pop_float();
          return;
        case OG_SCRIPT_STRING:
          scratch  = zk.pop_string();
          out.str  = scratch.data();
          out.len  = scratch.size();
          return;
        case OG_SCRIPT_INSTANCE:
          out.inst = pin(zk.pop_instance());
          return;
        }
      throw std::runtime_error("og_script: no result of kind " + std::to_string(kind));
      }

    int32_t memberGet(og_script_inst inst, uint32_t index, uint16_t i, uint32_t kind, og_script_value& out) {
      auto& ctx = get(inst);
      auto& sym = member(index);
      out = {};
      out.kind = kind;
      switch(kind) {
        case OG_SCRIPT_INT:
          out.i = sym.get_int(i, ctx.get());
          return OG_SCRIPT_OK;
        case OG_SCRIPT_FLOAT:
          out.f = sym.get_float(i, ctx.get());
          return OG_SCRIPT_OK;
        case OG_SCRIPT_STRING:
          scratch = sym.get_string(i, ctx.get());
          out.str = scratch.data();
          out.len = scratch.size();
          return OG_SCRIPT_OK;
        }
      throw std::runtime_error("og_script_member_get: no member of kind " + std::to_string(kind));
      }

    int32_t memberSet(og_script_inst inst, uint32_t index, uint16_t i, const og_script_value& v) {
      auto& ctx = get(inst);
      auto& sym = member(index);
      switch(v.kind) {
        case OG_SCRIPT_INT:
          sym.set_int(v.i, i, ctx.get());
          return OG_SCRIPT_OK;
        case OG_SCRIPT_FLOAT:
          sym.set_float(v.f, i, ctx.get());
          return OG_SCRIPT_OK;
        case OG_SCRIPT_STRING:
          sym.set_string(std::string_view(v.str, v.len), i, ctx.get());
          return OG_SCRIPT_OK;
        }
      throw std::runtime_error("og_script_member_set: no member of kind " + std::to_string(v.kind));
      }

    int32_t callExternal(std::string_view ext, const og_script_value* args, size_t count, uint32_t retKind, og_script_value& ret) {
      OgVm& vm = *owner;
      auto* cb = vm.external(ext);
      if(cb==nullptr) {
        if(!vm.defaultExternal)
          return OG_SCRIPT_NO_EXTERNAL;
        auto* sym = vm.zk->find_symbol_by_name(ext);
        vm.defaultExternal(sym!=nullptr ? std::string_view(sym->name()) : ext);
        ret = {};
        ret.kind = retKind;
        return OG_SCRIPT_OK;
        }
      for(size_t i=0; i<count; ++i)
        push(args[i]);
      (*cb)(vm);
      pop(retKind, ret);
      return OG_SCRIPT_OK;
      }

    // A standalone VM for seam tests; the backend must take it.
    static Rust* open(std::span<const uint8_t> dat, const char* name, uint32_t flags) {
      zenkit::DaedalusScript script;
      auto read = zenkit::Read::from(reinterpret_cast<const std::byte*>(dat.data()), dat.size());
      script.load(read.get());
      for(uint32_t i=0; i<script.symbols().size(); ++i)
        if(auto* sym = script.find_symbol_by_index(i); sym->type()==zenkit::DaedalusDataType::CLASS)
          script.register_as_opaque(sym);
      auto vm = std::make_unique<OgVm>(std::move(script), uint8_t(flags), name, dat);
      if(vm->rust==nullptr)
        throw std::runtime_error("og_script_test_open: no backend took the VM");
      // The bodies of Gothic::concatstrings / inttostring (private there).
      vm->register_external("concatstrings", [](std::string_view a, std::string_view b) { return std::string(a) + std::string(b); });
      vm->register_external("inttostring",   [](int i) { return std::to_string(i); });
      return vm.release()->rust.get();
      }

    og_script_inst testInit(const char* symbol) {
      auto* sym = owner->find_symbol_by_name(symbol);
      if(sym==nullptr)
        throw std::runtime_error(std::string("no symbol ") + symbol);
      return pin(owner->init_opaque_instance(sym));
      }

    // Turns a backend status into the engine's exception.
    // `what` + the symbol's name only on a panic: entries run every frame (task 4.19).
    void check(int32_t status, const char* err, std::string_view what, const zenkit::DaedalusSymbol& sym) {
      if(status==OG_SCRIPT_OK)
        return;
      if(status==OG_SCRIPT_PANICKED)
        throw zenkit::DaedalusVmException {"og_script backend panicked in " + std::string(what) + " " + sym.name() + ": " + err};
      throw zenkit::DaedalusVmException {std::string(err)};
      }
  };

namespace {
  // Runs a host function: C++ exceptions become OG_SCRIPT_ERROR with their message.
  template<typename F>
  int32_t guarded(og_script_host* host, char* err, size_t errLen, F&& fn) {
    try {
      if(host==nullptr)
        throw std::runtime_error("null og_script_host");
      return fn(*reinterpret_cast<OgVm::Rust*>(host));
      }
    catch(const std::exception& e) {
      setError(err, errLen, e.what());
      return OG_SCRIPT_ERROR;
      }
    catch(...) {
      setError(err, errLen, "non-standard C++ exception");
      return OG_SCRIPT_ERROR;
      }
    }
  }

OgVm::OgVm(zenkit::DaedalusScript&& script, uint8_t flags, std::string name, std::span<const uint8_t> dat)
  : zk(std::make_unique<Zk>(std::move(script), flags)), vmName(std::move(name)) {
  ScriptTrace::vm(*zk, vmName, zk->symbols().size());

  std::lock_guard<std::mutex> guard(backendSync);
  if(!hasBackend)
    return;
  if(dat.empty())
    throw std::runtime_error("og_script: VM " + vmName + " made without its DAT bytes while a backend is registered");
  auto r = std::make_unique<Rust>();
  r->owner = this;
  r->vm = backend.create(backend.user, reinterpret_cast<og_script_host*>(r.get()), vmName.c_str(), dat.data(), dat.size(), flags);
  if(r->vm!=nullptr) {
    Tempest::Log::i("og_script: ", vmName, " runs on the backend");
    rust = std::move(r);
    }
  }

OgVm::OgVm(OgVm&& other) noexcept
  : zk(std::move(other.zk)), vmName(std::move(other.vmName)), externals(std::move(other.externals)),
    defaultExternal(std::move(other.defaultExternal)), rust(std::move(other.rust)) {
  if(rust!=nullptr)
    rust->owner = this;
  }

OgVm& OgVm::operator=(OgVm&& other) noexcept {
  if(this==&other)
    return *this;
  if(rust!=nullptr)
    backend.destroy(backend.user, rust->vm);
  if(zk!=nullptr)
    ScriptTrace::vmGone(*zk);
  zk              = std::move(other.zk);
  vmName          = std::move(other.vmName);
  externals       = std::move(other.externals);
  defaultExternal = std::move(other.defaultExternal);
  rust            = std::move(other.rust);
  if(rust!=nullptr)
    rust->owner = this;
  return *this;
  }

OgVm::~OgVm() {
  // The backend goes first: it may still hold handles the bridge pins.
  if(rust!=nullptr)
    backend.destroy(backend.user, rust->vm);
  if(zk!=nullptr)
    ScriptTrace::vmGone(*zk);
  }

const OgVm::External* OgVm::external(std::string_view name) const {
  auto* sym = zk->find_symbol_by_name(name);
  if(sym==nullptr)
    return nullptr;
  auto it = externals.find(sym->name());
  if(it==externals.end())
    return nullptr;
  return &it->second;
  }

std::shared_ptr<zenkit::DaedalusInstance> OgVm::opaqueOn(zenkit::DaedalusSymbol* sym) {
  if(rust==nullptr)
    return zk->init_opaque_instance(sym);
  // zenkit::DaedalusVm::init_opaque_instance (DaedalusVm.cc:89), with the code on the backend.
  auto cls = sym;
  while(cls!=nullptr && cls->type()!=zenkit::DaedalusDataType::CLASS)
    cls = zk->find_symbol_by_index(cls->parent());
  if(cls==nullptr)
    throw zenkit::DaedalusVmException {"Cannot init " + sym->name() +
                                       ": parent class not found (did you try to initialize $INSTANCE_HELP?)"};
  auto inst = std::make_shared<zenkit::DaedalusOpaqueInstance>(*cls, zk->find_class_members(*cls));
  initOn<zenkit::DaedalusOpaqueInstance>(inst, sym);
  return inst;
  }

og_script_inst OgVm::backendPin(std::shared_ptr<zenkit::DaedalusInstance> const& inst) {
  return rust->pin(inst);
  }

void OgVm::backendCall(const zenkit::DaedalusSymbol* s, std::vector<og_script_value>& args, uint32_t retKind, og_script_value& ret) {
  // callOn pinned the arguments before: they go with this entry.
  Rust::Entry entry(*rust);
  char err[1024];
  err[0] = '\0'; // the backend NUL-terminates a message it writes (task 4.19: no 1 KiB memset per entry)
  auto st = backend.call_function(rust->vm, s->index(), args.data(), args.size(), retKind, &ret, err, sizeof(err));
  rust->check(st, err, "call_function", *s);
  }

void OgVm::backendInit(std::shared_ptr<zenkit::DaedalusInstance> const& inst, const zenkit::DaedalusSymbol* s) {
  Rust::Entry entry(*rust);
  char err[1024];
  err[0] = '\0'; // the backend NUL-terminates a message it writes (task 4.19: no 1 KiB memset per entry)
  auto h  = rust->pin(inst);
  auto st = backend.init_instance(rust->vm, s->index(), h, err, sizeof(err));
  rust->check(st, err, "init_instance", *s);
  }

void OgVm::backendBind(std::shared_ptr<zenkit::DaedalusInstance> const& inst, const zenkit::DaedalusSymbol* s) {
  Rust::Entry entry(*rust);
  char err[1024];
  err[0] = '\0'; // the backend NUL-terminates a message it writes (task 4.19: no 1 KiB memset per entry)
  auto h  = rust->pin(inst);
  auto st = backend.bind_instance(rust->vm, s->index(), h, err, sizeof(err));
  rust->check(st, err, "allocate_instance", *s);
  }

void OgVm::backendBindExternal(const zenkit::DaedalusSymbol* sym) {
  char err[1024] = {};
  auto st = backend.bind_external(rust->vm, sym->index(), err, sizeof(err));
  rust->check(st, err, "bind_external", *sym);
  }

void OgVm::backendBindDefault() {
  backend.bind_default_external(rust->vm);
  }

void OgVm::backendLenient() {
  backend.set_lenient(rust->vm);
  }

void OgVm::openDump(const std::string& path) {
  std::lock_guard<std::mutex> guard(dumpSync);
  if(dumpOpen)
    throw std::runtime_error("--script-dump: a dump is already open");
  dumpFile.open(path, std::ios::binary | std::ios::trunc);
  if(!dumpFile)
    throw std::runtime_error("--script-dump: can't write " + path);
  dumpOpen.store(true, std::memory_order_release);
  }

bool OgVm::dumping() {
  return dumpOpen.load(std::memory_order_acquire);
  }

void OgVm::dump(const zenkit::DaedalusInstance& inst, const zenkit::DaedalusSymbol* sym) {
  if(!dumping() || sym==nullptr)
    return;
  // {"vm":…,"backend":…,"sym":…,"members":{NAME:[values]}} in the script trace's value encoding.
  std::string line = "{\"vm\":\"" + vmName + "\",\"backend\":\"";
  line += rust!=nullptr ? "rust" : "zenkit";
  line += "\",\"sym\":\"" + sym->name() + "\",\"members\":" + membersJson(inst, sym) + "}\n";
  std::lock_guard<std::mutex> guard(dumpSync);
  dumpFile << line;
  dumpFile.flush();
  }

std::string OgVm::membersJson(const zenkit::DaedalusInstance& inst, const zenkit::DaedalusSymbol* sym) const {
  auto* cls = zk->find_symbol_by_index(sym->parent());
  while(cls!=nullptr && cls->type()!=zenkit::DaedalusDataType::CLASS)
    cls = zk->find_symbol_by_index(cls->parent());
  std::string line = "{";
  bool first = true;
  if(cls!=nullptr) {
    for(auto* m : zk->find_class_members(*cls)) {
      line += first ? "" : ",";
      first = false;
      line += "\"" + m->name() + "\":";
      // Members the engine didn't register (ZenKit's DaedalusUnboundMemberAccess) show the error.
      std::string values = "[";
      try {
        for(uint16_t i=0; i<m->count(); ++i) {
          values += i==0 ? "" : ",";
          switch(m->type()) {
            case zenkit::DaedalusDataType::FLOAT:
              values += ScriptTrace::floatValue(m->get_float(i, &inst));
              break;
            case zenkit::DaedalusDataType::STRING:
              values += ScriptTrace::stringValue(m->get_string(i, &inst));
              break;
            case zenkit::DaedalusDataType::INT:
            case zenkit::DaedalusDataType::FUNCTION:
              values += ScriptTrace::intValue(m->get_int(i, &inst));
              break;
            default:
              values += "null";
            }
          }
        values += "]";
        }
      catch(const zenkit::DaedalusScriptError& e) {
        values = "{\"error\":" + ScriptTrace::stringValue(e.what()).substr(5);
        }
      line += values;
      }
    }
  return line + "}";
  }

extern "C" void og_script_set_backend(const og_script_backend* b) {
  std::lock_guard<std::mutex> guard(backendSync);
  if(b==nullptr) {
    backend    = {};
    hasBackend = false;
    return;
    }
  backend    = *b;
  hasBackend = backend.create && backend.destroy && backend.bind_external && backend.bind_default_external &&
               backend.set_lenient && backend.init_instance && backend.bind_instance && backend.call_function &&
               backend.get_instance && backend.set_instance;
  if(!hasBackend)
    Tempest::Log::e("og_script: backend with a missing callback ignored, every VM stays on ZenKit");
  }

extern "C" int32_t og_script_member_get(og_script_host* host, og_script_inst inst, uint32_t member, uint16_t index,
                                        uint32_t kind, og_script_value* out, char* err, size_t errLen) {
  ScriptStats::accessor();
  return guarded(host, err, errLen, [&](OgVm::Rust& r) { return r.memberGet(inst, member, index, kind, *out); });
  }

extern "C" int32_t og_script_member_set(og_script_host* host, og_script_inst inst, uint32_t member, uint16_t index,
                                        const og_script_value* value, char* err, size_t errLen) {
  ScriptStats::accessor();
  return guarded(host, err, errLen, [&](OgVm::Rust& r) { return r.memberSet(inst, member, index, *value); });
  }

extern "C" int32_t og_script_global_get(og_script_host* host, uint32_t symbol, uint16_t index, uint32_t kind,
                                        og_script_value* out, char* err, size_t errLen) {
  ScriptStats::accessor();
  return guarded(host, err, errLen, [&](OgVm::Rust& r) { return r.globalGet(symbol, index, kind, *out); });
  }

extern "C" int32_t og_script_global_set(og_script_host* host, uint32_t symbol, uint16_t index,
                                        const og_script_value* value, char* err, size_t errLen) {
  ScriptStats::accessor();
  return guarded(host, err, errLen, [&](OgVm::Rust& r) { return r.globalSet(symbol, index, *value); });
  }

extern "C" int32_t og_script_tracing(void) {
  return ScriptTrace::active() ? 1 : 0;
  }

extern "C" void og_script_trace_mark(uint64_t action, const char* tool, size_t toolLen) {
  // Called from Rust: nothing may unwind out.
  try {
    ScriptTrace::mark(action, std::string_view(tool, toolLen));
    }
  catch(const std::exception& e) {
    Tempest::Log::e("script trace: mark of action ", action, " failed: ", e.what());
    }
  catch(...) {
    Tempest::Log::e("script trace: mark of action ", action, " failed");
    }
  }

extern "C" int32_t og_script_trace_external(og_script_host* host, const char* name, size_t nameLen,
                                            const og_script_value* args, size_t count, const og_script_value* ret,
                                            int32_t threw, char* err, size_t errLen) {
  return guarded(host, err, errLen, [&](OgVm::Rust& r) {
    r.traceExternal(std::string_view(name, nameLen), args, count, ret, threw!=0);
    return OG_SCRIPT_OK;
    });
  }

extern "C" int32_t og_script_instance_symbol(og_script_host* host, og_script_inst inst, uint32_t* symbol, char* err, size_t errLen) {
  return guarded(host, err, errLen, [&](OgVm::Rust& r) {
    auto& p = r.get(inst);
    if(p==nullptr)
      throw std::runtime_error("og_script_instance_symbol: null instance");
    *symbol = p->symbol_index();
    return OG_SCRIPT_OK;
    });
  }

extern "C" int32_t og_script_instance_release(og_script_host* host, og_script_inst inst, char* err, size_t errLen) {
  return guarded(host, err, errLen, [&](OgVm::Rust& r) {
    r.release(inst);
    return OG_SCRIPT_OK;
    });
  }

extern "C" int32_t og_script_call_external(og_script_host* host, const char* name, size_t nameLen,
                                           const og_script_value* args, size_t count, uint32_t retKind,
                                           og_script_value* ret, char* err, size_t errLen) {
  ScriptStats::Bridge stats;
  return guarded(host, err, errLen, [&](OgVm::Rust& r) {
    return r.callExternal(std::string_view(name, nameLen), args, count, retKind, *ret);
    });
  }

extern "C" og_script_host* og_script_test_open(const uint8_t* dat, size_t len, const char* name, uint32_t flags,
                                               char* err, size_t errLen) {
  try {
    return reinterpret_cast<og_script_host*>(OgVm::Rust::open(std::span<const uint8_t>(dat, len), name, flags));
    }
  catch(const std::exception& e) {
    setError(err, errLen, e.what());
    return nullptr;
    }
  catch(...) {
    setError(err, errLen, "non-standard C++ exception");
    return nullptr;
    }
  }

extern "C" int32_t og_script_test_init(og_script_host* host, const char* symbol, og_script_inst* out, char* err, size_t errLen) {
  return guarded(host, err, errLen, [&](OgVm::Rust& r) {
    *out = r.testInit(symbol);
    return OG_SCRIPT_OK;
    });
  }

extern "C" void* og_script_test_backend_vm(og_script_host* host) {
  if(host==nullptr)
    return nullptr;
  return reinterpret_cast<OgVm::Rust*>(host)->vm;
  }

extern "C" void og_script_test_close(og_script_host* host) {
  if(host!=nullptr)
    delete reinterpret_cast<OgVm::Rust*>(host)->owner;
  }
