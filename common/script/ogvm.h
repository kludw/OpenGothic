#pragma once

#include <zenkit/DaedalusVm.hh>

#include "scripttrace.h"

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
class OgVm final {
  public:
    // Runs one external against the VM's ZenKit stack: pops the arguments, calls the engine, pushes the result.
    using External = std::function<void(OgVm&)>;

    // `name` is the DAT file (GOTHIC.DAT, MENU.DAT, ...): it names the VM in the script trace.
    OgVm(zenkit::DaedalusScript&& script, uint8_t flags, std::string name);
    OgVm(OgVm&&) noexcept;
    OgVm& operator=(OgVm&&) noexcept;
    ~OgVm();

    // The raw VM, for code that drives ZenKit internals directly (Ikarus' DirectMemory, ADR 0003)
    // and for class-member registration, which lives with the instances in ZenKit.
    // Functions registered on it directly (DirectMemory's override_function) bypass the external table.
    zenkit::DaedalusVm&       zenkit()       { return *zk; }
    const zenkit::DaedalusVm& zenkit() const { return *zk; }

    std::string_view name() const { return vmName; }

    template<typename R = zenkit::IgnoreReturnValue, typename S, typename... P>
    R call_function(S&& sym, P... args) {
      if(!ScriptTrace::active())
        return zk->template call_function<R, P...>(std::forward<S>(sym), args...);
      // ZenKit's by-name overload only looks the symbol up; calling by symbol is the same call.
      auto* s = symbolOf(sym);
      ScriptTrace::Entry entry(*zk, vmName, "call", s, ScriptTrace::values(*zk, args...));
      return traced(entry, [&]() -> R { return zk->template call_function<R, P...>(s, args...); });
      }

    // The init/allocate overloads that create the instance do what ZenKit's do (make_shared,
    // then the overload taking it), so the trace numbers the instance before its script runs.
    template<typename T, typename S>
    std::shared_ptr<T> init_instance(S&& sym) {
      if(!ScriptTrace::active())
        return zk->template init_instance<T>(std::forward<S>(sym));
      auto inst = std::make_shared<T>();
      init_instance<T>(inst, std::forward<S>(sym));
      return inst;
      }
    template<typename T, typename S>
    void init_instance(std::shared_ptr<T> const& instance, S&& sym) {
      if(!ScriptTrace::active())
        return zk->template init_instance<T>(instance, std::forward<S>(sym));
      auto* s = symbolOf(sym);
      ScriptTrace::created(*zk, instance);
      ScriptTrace::Entry entry(*zk, vmName, "init", s, "[]");
      traced(entry, [&]() { zk->template init_instance<T>(instance, mutableSymbol(s)); return instance; });
      }
    std::shared_ptr<zenkit::DaedalusInstance> init_opaque_instance(zenkit::DaedalusSymbol* sym) {
      if(!ScriptTrace::active())
        return zk->init_opaque_instance(sym);
      // ZenKit makes the opaque instance inside: it is numbered when first seen.
      ScriptTrace::Entry entry(*zk, vmName, "opaque", sym, "[]");
      return traced(entry, [&]() { return zk->init_opaque_instance(sym); });
      }

    template<typename T, typename S>
    std::shared_ptr<T> allocate_instance(S&& sym) {
      if(!ScriptTrace::active())
        return zk->template allocate_instance<T>(std::forward<S>(sym));
      auto inst = std::make_shared<T>();
      allocate_instance<T>(inst, std::forward<S>(sym));
      return inst;
      }
    template<typename T, typename S>
    void allocate_instance(std::shared_ptr<T> const& instance, S&& sym) {
      if(!ScriptTrace::active())
        return zk->template allocate_instance<T>(instance, std::forward<S>(sym));
      // Runs no script code, but it binds the instance to its symbol: a replay must do it too.
      auto* s = symbolOf(sym);
      ScriptTrace::created(*zk, instance);
      ScriptTrace::Entry entry(*zk, vmName, "allocate", s, "[]");
      traced(entry, [&]() { zk->template allocate_instance<T>(instance, mutableSymbol(s)); return instance; });
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
      if(!ScriptTrace::active())
        return zk->register_default_external(std::function<void(std::string_view)>(cb));
      zk->register_default_external(std::function<void(std::string_view)>([name = vmName, cb](std::string_view ext) {
        if(!ScriptTrace::active())
          return cb(ext);
        ScriptTrace::Ext trace(name, ext, "[]", true);
        cb(ext);
        trace.ret("null");
        }));
      }
    template<typename F>
    void register_exception_handler(F const& cb) {
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
      if(auto* sym = zk->find_symbol_by_name(name))
        externals[sym->name()] = Zk::template stackCall<R, P...>(cb);
      }
    template<typename T>
    void register_external(std::string_view name, T const& callback) {
      register_external(name, std::function {callback});
      }

    // The callback registered for an external, or nullptr. Names match like ZenKit's symbol lookup.
    const External* external(std::string_view name) const;

  private:
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
        ScriptTrace::Ext trace(name, symbol, ScriptTrace::values(*machine, args...));
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
  };
