#pragma once

#include <zenkit/DaedalusVm.hh>

#include "scriptstats.h"
#include "scripttrace.h"
#include "../../game/capi/og_script.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// opengothic-rs (task 4.12, ADR 0022): the engine's handle on a Daedalus VM. Holders keep an OgVm
// instead of a zenkit::DaedalusVm, so a second interpreter (the Rust VM, 4.14) can take over behind
// the same calls. Today every call forwards to ZenKit unchanged; instances, members and the
// call stack stay in ZenKit. Externals registered here are also kept in a name -> callback table:
// each entry pops its arguments off the ZenKit stack and pushes the result exactly like the
// callback ZenKit itself stores, so a call coming from another VM can run OpenGothic's callback.
// With `--script-trace` (task 4.13) every entry and external also goes to ScriptTrace.
// With og_script_stats_enable (task 4.19) every entry opens a ScriptStats::Top: the outermost is timed.
// With a backend registered through game/capi/og_script.h (task 4.14) that takes this VM, the
// entries run on the backend (the Rust VM) instead: ZenKit still owns the symbols' C++ side,
// binds instances and checks them, and serves the externals and member accesses the backend
// makes through the seam.
class OgVm final {
  public:
    // Runs one external against the VM's ZenKit stack: pops the arguments, calls the engine, pushes the result.
    using External = std::function<void(OgVm&)>;

    // `name` is the DAT file (GOTHIC.DAT, MENU.DAT, ...): it names the VM in the script trace.
    // `dat` = the bytes `script` was parsed from, offered to the og_script backend (copied).
    OgVm(zenkit::DaedalusScript&& script, uint8_t flags, std::string name, std::span<const uint8_t> dat = {});
    OgVm(OgVm&&) noexcept;
    OgVm& operator=(OgVm&&) noexcept;
    ~OgVm();

    // The raw VM, for code that drives ZenKit internals directly (Ikarus' DirectMemory, ADR 0003)
    // and for class-member registration, which lives with the instances in ZenKit.
    // Functions registered on it directly (DirectMemory's override_function) bypass the external table.
    zenkit::DaedalusVm&       zenkit()       { return *zk; }
    const zenkit::DaedalusVm& zenkit() const { return *zk; }

    std::string_view name() const { return vmName; }
    // The og_script backend runs this VM's scripts.
    bool onBackend() const { return rust!=nullptr; }
    // The og_script bridge (og_script_host); defined in ogvm.cpp.
    class Rust;

    // opengothic-rs (task 4.14): `--script-dump <path>`: after every engine init_instance, the
    // instance's members as JSON Lines, read through ZenKit's symbols (the same objects for
    // both backends). Throws when the file can't be written or a dump is already open.
    static void openDump(const std::string& path);
    // A dump is open: lazily loaded definitions (ParticleFx, VisualFx: task 4.15) then init every
    // instance of their classes once, so the dump covers the whole DAT, not only what a run used.
    static bool dumping();
    // `{NAME:[values],…}` for every member of the instance's class, read through ZenKit's
    // symbols in the script trace's value encoding (the --script-dump member format).
    std::string membersJson(const zenkit::DaedalusInstance& inst, const zenkit::DaedalusSymbol* sym) const;

    template<typename R = zenkit::IgnoreReturnValue, typename S, typename... P>
    R call_function(S&& sym, P... args) {
      ScriptStats::Top stats;
      if(!ScriptTrace::active() && rust==nullptr)
        return zk->template call_function<R, P...>(std::forward<S>(sym), args...);
      // ZenKit's by-name overload only looks the symbol up; calling by symbol is the same call.
      auto* s = symbolOf(sym);
      if(!ScriptTrace::active())
        return callOn<R>(s, args...);
      ScriptTrace::Entry entry(*zk, vmName, "call", s, ScriptTrace::values(*zk, args...), nullptr, ScriptTrace::returnKind<R>());
      return traced(entry, [&]() -> R { return callOn<R>(s, args...); });
      }

    // The init/allocate overloads that create the instance do what ZenKit's do (make_shared,
    // then the overload taking it), so the trace numbers the instance before its script runs.
    template<typename T, typename S>
    std::shared_ptr<T> init_instance(S&& sym) {
      ScriptStats::Top stats;
      if(!ScriptTrace::active() && rust==nullptr && !dumping())
        return zk->template init_instance<T>(std::forward<S>(sym));
      auto inst = std::make_shared<T>();
      init_instance<T>(inst, std::forward<S>(sym));
      return inst;
      }
    template<typename T, typename S>
    void init_instance(std::shared_ptr<T> const& instance, S&& sym) {
      ScriptStats::Top stats;
      if(!ScriptTrace::active() && rust==nullptr && !dumping())
        return zk->template init_instance<T>(instance, std::forward<S>(sym));
      auto* s = symbolOf(sym);
      if(!ScriptTrace::active()) {
        initOn<T>(instance, s);
        } else {
        ScriptTrace::created(*zk, instance);
        // Gameplay trace (task 4.18): ZenKit's init_instance starts with this same allocate_instance
        // (it only binds and types the instance), done here first so the entry's sync can read the
        // C++ defaults of the instance's members before its code runs.
        if(ScriptTrace::gameplay())
          zk->template allocate_instance<T>(instance, mutableSymbol(s));
        ScriptTrace::Entry entry(*zk, vmName, "init", s, "[]", instance.get());
        traced(entry, [&]() { initOn<T>(instance, s); return instance; });
        }
      dump(*instance, s);
      }
    std::shared_ptr<zenkit::DaedalusInstance> init_opaque_instance(zenkit::DaedalusSymbol* sym) {
      ScriptStats::Top stats;
      if(!ScriptTrace::active() && rust==nullptr && !dumping())
        return zk->init_opaque_instance(sym);
      std::shared_ptr<zenkit::DaedalusInstance> inst;
      if(!ScriptTrace::active()) {
        inst = opaqueOn(sym);
        } else {
        // ZenKit makes the opaque instance inside: it is numbered when first seen.
        ScriptTrace::Entry entry(*zk, vmName, "opaque", sym, "[]");
        inst = traced(entry, [&]() { return opaqueOn(sym); });
        }
      dump(*inst, sym);
      return inst;
      }

    template<typename T, typename S>
    std::shared_ptr<T> allocate_instance(S&& sym) {
      ScriptStats::Top stats;
      if(!ScriptTrace::active() && rust==nullptr)
        return zk->template allocate_instance<T>(std::forward<S>(sym));
      auto inst = std::make_shared<T>();
      allocate_instance<T>(inst, std::forward<S>(sym));
      return inst;
      }
    template<typename T, typename S>
    void allocate_instance(std::shared_ptr<T> const& instance, S&& sym) {
      ScriptStats::Top stats;
      if(!ScriptTrace::active() && rust==nullptr)
        return zk->template allocate_instance<T>(instance, std::forward<S>(sym));
      auto* s = symbolOf(sym);
      auto bind = [&]() {
        zk->template allocate_instance<T>(instance, mutableSymbol(s));
        if(rust!=nullptr)
          backendBind(instance, s);
        return instance;
        };
      if(!ScriptTrace::active()) {
        bind();
        return;
        }
      // Runs no script code, but it binds the instance to its symbol: a replay must do it too.
      ScriptTrace::created(*zk, instance);
      ScriptTrace::Entry entry(*zk, vmName, "allocate", s, "[]", instance.get());
      traced(entry, bind);
      }

    const std::vector<zenkit::DaedalusSymbol>& symbols() const noexcept { return zk->symbols(); }

    zenkit::DaedalusSymbol*       find_symbol_by_name(std::string_view name)       { return zk->find_symbol_by_name(name); }
    const zenkit::DaedalusSymbol* find_symbol_by_name(std::string_view name) const { return zk->find_symbol_by_name(name); }
    zenkit::DaedalusSymbol*       find_symbol_by_index(uint32_t index)             { return zk->find_symbol_by_index(index); }
    const zenkit::DaedalusSymbol* find_symbol_by_index(uint32_t index) const       { return zk->find_symbol_by_index(index); }
    template<typename T>
    zenkit::DaedalusSymbol*       find_symbol_by_instance(T const& inst)           { return zk->find_symbol_by_instance(inst); }
    template<typename T>
    const zenkit::DaedalusSymbol* find_symbol_by_instance(T const& inst) const     { return zk->find_symbol_by_instance(inst); }
    std::span<zenkit::DaedalusSymbol> find_parameters_for_function(const zenkit::DaedalusSymbol* fn) {
      return zk->find_parameters_for_function(fn);
      }
    void enumerate_instances_by_class_name(std::string_view name, std::function<void(zenkit::DaedalusSymbol&)> const& cb) {
      zk->enumerate_instances_by_class_name(name, cb);
      }

    zenkit::DaedalusSymbol* global_self()   { return zk->global_self();   }
    zenkit::DaedalusSymbol* global_other()  { return zk->global_other();  }
    zenkit::DaedalusSymbol* global_victim() { return zk->global_victim(); }
    zenkit::DaedalusSymbol* global_hero()   { return zk->global_hero();   }
    zenkit::DaedalusSymbol* global_item()   { return zk->global_item();   }

    void register_as_opaque(zenkit::DaedalusSymbol* sym) { zk->register_as_opaque(sym); }

    template<typename F>
    void register_default_external(F const& cb) {
      static_assert(std::is_invocable_v<F const&, std::string_view>, "the engine registers a by-name default external");
      defaultExternal = cb;
      if(rust!=nullptr)
        backendBindDefault();
      if(!ScriptTrace::active())
        return zk->register_default_external(std::function<void(std::string_view)>(cb));
      zk->register_default_external(std::function<void(std::string_view)>([machine = zk.get(), name = vmName, cb](std::string_view ext) {
        if(!ScriptTrace::active())
          return cb(ext);
        ScriptTrace::Ext trace(*machine, name, ext, "[]", true);
        cb(ext);
        trace.ret("null");
        }));
      }
    template<typename F>
    void register_exception_handler(F const& cb) {
      if(rust!=nullptr)
        backendLenient();
      if(!ScriptTrace::active())
        return zk->register_exception_handler(cb);
      zk->register_exception_handler([name = vmName, cb](zenkit::DaedalusVm& vm, zenkit::DaedalusScriptError const& err,
                                                         zenkit::DaedalusInstruction const& instr) {
        auto strategy = cb(vm, err, instr);
        if(strategy!=zenkit::DaedalusVmExceptionStrategy::FAIL)
          ScriptTrace::recover(name, err, instr);
        return strategy;
        });
      }

    // Same contract as zenkit::DaedalusVm::register_external (same checks, same exceptions, a name the
    // script doesn't declare is ignored); the callback also lands in the external table.
    template<typename R, typename... P>
    void register_external(std::string_view name, std::function<R(P...)> const& callback) {
      const std::function<R(P...)> cb = ScriptTrace::active() ? tracedExternal(name, callback) : callback;
      zk->register_external(name, cb);
      if(auto* sym = zk->find_symbol_by_name(name)) {
        auto& ext = externals[sym->name()];
        ext = Zk::template stackCall<R, P...>(cb);
        bindIndex(sym->index(), &ext);
        if(rust!=nullptr)
          backendBindExternal(sym);
        }
      }
    template<typename T>
    void register_external(std::string_view name, T const& callback) {
      register_external(name, std::function {callback});
      }

    // The callback registered for the external with symbol index `symbol`, or nullptr.
    const External* external(uint32_t symbol) const;

  private:

    void dump(const zenkit::DaedalusInstance& inst, const zenkit::DaedalusSymbol* sym);

    // The backend half of the entries; the ZenKit half when no backend took the VM.
    template<typename R, typename... P>
    R callOn(const zenkit::DaedalusSymbol* s, P... args) {
      if(rust==nullptr)
        return zk->template call_function<R, P...>(s, args...);
      if(s==nullptr)
        throw zenkit::DaedalusVmException {"Cannot call function: not found"};
      // No heap per entry (task 4.19): entries run every frame.
      std::array<og_script_value, sizeof...(P)> values = {toValue(args)...};
      og_script_value ret = {};
      backendCall(s, values.data(), values.size(), retKind<R>(), ret);
      if constexpr (std::same_as<R, zenkit::IgnoreReturnValue>) {
        return R {};
        }
      else if constexpr (std::same_as<R, void>) {
        return;
        }
      else if constexpr (std::same_as<R, int32_t>) {
        return ret.i;
        }
      else if constexpr (std::same_as<R, float>) {
        return ret.f;
        }
      else if constexpr (std::same_as<R, std::string>) {
        return std::string(ret.str, ret.len);
        }
      else {
        static_assert(ScriptTrace::unsupported<R>, "og_script: no backend return for this type");
        }
      }
    template<typename T>
    void initOn(std::shared_ptr<T> const& instance, const zenkit::DaedalusSymbol* s) {
      if(rust==nullptr)
        return zk->template init_instance<T>(instance, mutableSymbol(s));
      // ZenKit's own bookkeeping and checks (symbol, class registered to T), no code.
      zk->template allocate_instance<T>(instance, mutableSymbol(s));
      backendInit(instance, s);
      }
    std::shared_ptr<zenkit::DaedalusInstance> opaqueOn(zenkit::DaedalusSymbol* sym);

    template<typename R>
    static constexpr uint32_t retKind() {
      if constexpr (std::same_as<R, zenkit::IgnoreReturnValue>) return OG_SCRIPT_IGNORE;
      else if constexpr (std::same_as<R, void>) return OG_SCRIPT_VOID;
      else if constexpr (std::same_as<R, int32_t>) return OG_SCRIPT_INT;
      else if constexpr (std::same_as<R, float>) return OG_SCRIPT_FLOAT;
      else if constexpr (std::same_as<R, std::string>) return OG_SCRIPT_STRING;
      else return OG_SCRIPT_VOID;
      }
    // Strings point into the argument (alive for the call); instances are pinned.
    template<typename T>
    og_script_value toValue(const T& v) {
      using U = std::remove_cvref_t<T>;
      og_script_value out = {};
      if constexpr (std::same_as<U, bool> || std::integral<U>) {
        out.kind = OG_SCRIPT_INT;
        out.i    = int32_t(v);
        }
      else if constexpr (std::floating_point<U>) {
        out.kind = OG_SCRIPT_FLOAT;
        out.f    = float(v);
        }
      else if constexpr (std::convertible_to<const U&, std::string_view>) {
        std::string_view str(v);
        out.kind = OG_SCRIPT_STRING;
        out.str  = str.data();
        out.len  = str.size();
        }
      else if constexpr (zenkit::DaedalusInstancePointer<U>) {
        out.kind = OG_SCRIPT_INSTANCE;
        out.inst = backendPin(std::static_pointer_cast<zenkit::DaedalusInstance>(v));
        }
      else {
        static_assert(ScriptTrace::unsupported<U>, "og_script: no backend value for this argument type");
        }
      return out;
      }

    og_script_inst backendPin(std::shared_ptr<zenkit::DaedalusInstance> const& inst);
    void backendCall(const zenkit::DaedalusSymbol* s, const og_script_value* args, size_t count, uint32_t retKind, og_script_value& ret);
    void backendInit(std::shared_ptr<zenkit::DaedalusInstance> const& inst, const zenkit::DaedalusSymbol* s);
    void backendBind(std::shared_ptr<zenkit::DaedalusInstance> const& inst, const zenkit::DaedalusSymbol* s);
    void backendBindExternal(const zenkit::DaedalusSymbol* sym);
    void bindIndex(uint32_t symbol, const External* ext);
    void backendBindDefault();
    void backendLenient();

    template<typename S>
    const zenkit::DaedalusSymbol* symbolOf(S const& sym) {
      if constexpr (std::is_convertible_v<S const&, std::string_view>)
        return zk->find_symbol_by_name(std::string_view(sym)); else
        return sym;
      }
    // ZenKit's init/allocate take a mutable symbol; a null one (name not found) fails there as by name.
    static zenkit::DaedalusSymbol* mutableSymbol(const zenkit::DaedalusSymbol* s) {
      return const_cast<zenkit::DaedalusSymbol*>(s);
      }

    // Runs `fn` for `entry`; writes its result, or the exception's message before rethrowing it.
    template<typename F>
    auto traced(ScriptTrace::Entry& entry, F&& fn) -> decltype(fn()) {
      using R = decltype(fn());
      try {
        if constexpr (std::is_void_v<R>) {
          fn();
          entry.ret("null");
          } else {
          R r = fn();
          entry.ret(ScriptTrace::value(*zk, r));
          return r;
          }
        }
      catch(const std::exception& e) {
        entry.fail(e.what());
        throw;
        }
      catch(...) {
        entry.fail("non-standard exception");
        throw;
        }
      }

    // The callback with a trace of each call. Captures the heap-allocated ZenKit VM, never `this`
    // (an OgVm moves: GameScript::createVm returns one by value).
    template<typename R, typename... P>
    std::function<R(P...)> tracedExternal(std::string_view ext, std::function<R(P...)> const& callback) {
      zenkit::DaedalusVm* machine = zk.get();
      auto* sym = zk->find_symbol_by_name(ext);
      return [machine, name = vmName, symbol = sym!=nullptr ? sym->name() : std::string(ext), callback](P... args) -> R {
        if(!ScriptTrace::active())
          return callback(args...);
        ScriptTrace::Ext trace(*machine, name, symbol, ScriptTrace::values(*machine, args...));
        if constexpr (std::is_void_v<R>) {
          callback(args...);
          trace.ret("null");
          } else {
          R r = callback(args...);
          trace.ret(ScriptTrace::value(*machine, r));
          return r;
          }
        };
      }

    // Subclass only to reach ZenKit's protected stack helpers that its own external wrapper uses.
    class Zk final : public zenkit::DaedalusVm {
      public:
        using zenkit::DaedalusVm::DaedalusVm;

        // The body of zenkit::DaedalusVm::register_external's stored lambda.
        template<typename R, typename... P>
        static External stackCall(std::function<R(P...)> const& callback) {
          return [callback](OgVm& vm) {
            Zk& machine = *vm.zk;
            if constexpr (std::same_as<void, R>) {
              if constexpr (sizeof...(P) > 0) {
                auto v = machine.template pop_values_for_external<P...>();
                std::apply(callback, v);
                } else {
                callback();
                }
              } else {
              if constexpr (sizeof...(P) > 0) {
                machine.push_value_from_external(std::apply(callback, machine.template pop_values_for_external<P...>()));
                } else {
                machine.push_value_from_external(callback());
                }
              }
            };
          }
      };

    std::unique_ptr<Zk>                     zk;
    std::string                             vmName;
    std::unordered_map<std::string, External> externals;
    // Symbol index -> entry of `externals` (the map's nodes don't move, also not when it moves).
    std::vector<const External*>            externalsByIndex;
    std::function<void(std::string_view)>   defaultExternal;
    // The og_script bridge (heap: its address is the backend's og_script_host); null on ZenKit.
    std::unique_ptr<Rust>                   rust;
  };
