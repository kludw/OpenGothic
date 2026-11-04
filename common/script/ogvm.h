#pragma once

#include <zenkit/DaedalusVm.hh>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// opengothic-rs (task 4.12, ADR 0022): the engine's handle on a Daedalus VM. Holders keep an OgVm
// instead of a zenkit::DaedalusVm, so a second interpreter (the Rust VM, 4.14) can take over behind
// the same calls. Today every call forwards to ZenKit unchanged; instances, members and the
// call stack stay in ZenKit. Externals registered here are also kept in a name -> callback table:
// each entry pops its arguments off the ZenKit stack and pushes the result exactly like the
// callback ZenKit itself stores, so a call coming from another VM can run OpenGothic's callback.
class OgVm final {
  public:
    // Runs one external against the VM's ZenKit stack: pops the arguments, calls the engine, pushes the result.
    using External = std::function<void(OgVm&)>;

    OgVm(zenkit::DaedalusScript&& script, uint8_t flags);
    OgVm(OgVm&&) noexcept;
    OgVm& operator=(OgVm&&) noexcept;
    ~OgVm();

    // The raw VM, for code that drives ZenKit internals directly (Ikarus' DirectMemory, ADR 0003)
    // and for class-member registration, which lives with the instances in ZenKit.
    // Functions registered on it directly (DirectMemory's override_function) bypass the external table.
    zenkit::DaedalusVm&       zenkit()       { return *zk; }
    const zenkit::DaedalusVm& zenkit() const { return *zk; }

    template<typename R = zenkit::IgnoreReturnValue, typename S, typename... P>
    R call_function(S&& sym, P... args) {
      return zk->template call_function<R, P...>(std::forward<S>(sym), args...);
      }

    template<typename T, typename S>
    std::shared_ptr<T> init_instance(S&& sym) {
      return zk->template init_instance<T>(std::forward<S>(sym));
      }
    template<typename T, typename S>
    void init_instance(std::shared_ptr<T> const& instance, S&& sym) {
      zk->template init_instance<T>(instance, std::forward<S>(sym));
      }
    std::shared_ptr<zenkit::DaedalusInstance> init_opaque_instance(zenkit::DaedalusSymbol* sym) {
      return zk->init_opaque_instance(sym);
      }

    template<typename T, typename S>
    std::shared_ptr<T> allocate_instance(S&& sym) {
      return zk->template allocate_instance<T>(std::forward<S>(sym));
      }
    template<typename T, typename S>
    void allocate_instance(std::shared_ptr<T> const& instance, S&& sym) {
      zk->template allocate_instance<T>(instance, std::forward<S>(sym));
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
    void register_default_external(F const& cb) { zk->register_default_external(cb); }
    template<typename F>
    void register_exception_handler(F const& cb) { zk->register_exception_handler(cb); }

    // Same contract as zenkit::DaedalusVm::register_external (same checks, same exceptions, a name the
    // script doesn't declare is ignored); the callback also lands in the external table.
    template<typename R, typename... P>
    void register_external(std::string_view name, std::function<R(P...)> const& callback) {
      zk->register_external(name, callback);
      if(auto* sym = zk->find_symbol_by_name(name))
        externals[sym->name()] = Zk::template stackCall<R, P...>(callback);
      }
    template<typename T>
    void register_external(std::string_view name, T const& callback) {
      register_external(name, std::function {callback});
      }

    // The callback registered for an external, or nullptr. Names match like ZenKit's symbol lookup.
    const External* external(std::string_view name) const;

  private:
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
    std::unordered_map<std::string, External> externals;
  };
