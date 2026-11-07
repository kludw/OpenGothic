#pragma once

#include <zenkit/DaedalusVm.hh>

#include <concepts>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// opengothic-rs (task 4.13): the `--script-trace <path>` recorder. JSON Lines in the value
// encoding of og-parity's daedalus-exec harness (task 4.6, crates/og-parity/src/daedalus_exec.rs);
// the event list is in docs/notes/daedalus-vm.md § In-game script trace. Inactive (nothing
// opened, every hook a no-op) unless open() was called; the game calls it once at start-up,
// before any VM exists, and stop() at the first simulation tick.
// Gameplay mode (task 4.18, `--script-trace-gameplay`): the trace goes on until shutdown and
// adds `sync` events, the script-visible state the engine changed (members, globals, instance
// symbols), so og-parity daedalus-replay can re-run every call offline.
namespace ScriptTrace {
  // Opens (truncates) the trace and writes its header. Throws std::runtime_error when the
  // file can't be written, or when a trace is already open.
  void open(const std::string& path, bool gameplay = false);
  // Writes the end event with `reason`, flushes and closes; later events are dropped.
  // No-op when inactive.
  void stop(std::string_view reason);
  bool active();
  // Opened in gameplay mode: no stop at the first tick, `sync` events.
  bool gameplay();
  // A simulation tick starts (gameplay mode): syncs every live instance and every VM's globals.
  void tick();
  // `{"ev":"mark","action":N,"tool":…}` (gameplay mode): a recorded action starts (og_script_trace_mark).
  void mark(uint64_t action, std::string_view tool);
  // `{"ev":"patch","vm":…,"id":…,"symbol":"NAME","kind":…}` (task 4.21): the Rust backend
  // entered `symbol` through a script patch (og_script_trace_patch).
  void patch(std::string_view vmName, std::string_view id, std::string_view symbol, std::string_view kind);

  // A VM was created (OgVm's constructor) / is being destroyed (its instances aren't synced any more).
  void vm(const zenkit::DaedalusVm& vm, std::string_view vmName, size_t symbols);
  void vmGone(const zenkit::DaedalusVm& vm);

  // `instance` is being created by the engine: it gets the VM's next creation ordinal, even
  // when its address was seen before. Ordinals, entry numbers and external seq count per VM.
  void created(const zenkit::DaedalusVm& vm, const std::shared_ptr<zenkit::DaedalusInstance>& instance);
  // A VM exception the registered handler recovered from (lenient GOTHIC VM); not called for FAIL.
  void recover(std::string_view vmName, const zenkit::DaedalusScriptError& err, const zenkit::DaedalusInstruction& instr);

  // Encoded values: {"i":n}, {"f":bits}, {"s":"…"} (edition bytes, Latin-1 escaped),
  // {"inst":ordinal,"sym":"NAME"} ({"inst":-1} for null), {"fn":"NAME"}, {"ref":"NAME"}
  // (a symbol popped by reference); null for no value.
  std::string intValue(int64_t v);
  std::string floatValue(float v);
  std::string stringValue(std::string_view bytes);
  std::string functionValue(const zenkit::DaedalusSymbol* fn);
  std::string referenceValue(const zenkit::DaedalusSymbol* sym);
  std::string instanceValue(zenkit::DaedalusVm& vm, const std::shared_ptr<zenkit::DaedalusInstance>& instance);
  std::string instanceValue(zenkit::DaedalusVm& vm, const zenkit::DaedalusInstance* instance);

  template<typename T>
  inline constexpr bool unsupported = false;

  template<typename T>
  std::string value(zenkit::DaedalusVm& vm, const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::same_as<U, zenkit::IgnoreReturnValue>) {
      return "null";
      }
    else if constexpr (std::same_as<U, bool>) {
      return intValue(v ? 1 : 0);
      }
    else if constexpr (std::floating_point<U>) {
      return floatValue(float(v));
      }
    else if constexpr (std::integral<U>) {
      return intValue(int64_t(v));
      }
    else if constexpr (std::convertible_to<const U&, std::string_view>) {
      return stringValue(std::string_view(v));
      }
    else if constexpr (std::same_as<U, zenkit::DaedalusFunction>) {
      return functionValue(v.value);
      }
    else if constexpr (std::same_as<U, zenkit::DaedalusSymbol*> || std::same_as<U, const zenkit::DaedalusSymbol*>) {
      return referenceValue(v);
      }
    else if constexpr (zenkit::DaedalusInstancePointer<U>) {
      return instanceValue(vm, std::static_pointer_cast<zenkit::DaedalusInstance>(v));
      }
    else if constexpr (zenkit::DaedalusInstancePointerRaw<U>) {
      return instanceValue(vm, static_cast<const zenkit::DaedalusInstance*>(v));
      }
    else {
      static_assert(unsupported<U>, "script trace: no encoding for this script value type");
      }
    }

  // The result type a call_function<R> asks for, as `enter` names it in gameplay mode.
  template<typename R>
  constexpr const char* returnKind() {
    using U = std::remove_cvref_t<R>;
    if constexpr (std::same_as<U, zenkit::IgnoreReturnValue>)
      return "ignore";
    else if constexpr (std::is_void_v<U>)
      return "void";
    else if constexpr (std::same_as<U, bool> || std::integral<U>)
      return "int";
    else if constexpr (std::floating_point<U>)
      return "float";
    else if constexpr (std::convertible_to<const U&, std::string_view>)
      return "string";
    else
      return "instance";
    }

  template<typename... P>
  std::string values(zenkit::DaedalusVm& vm, const P&... v) {
    std::string out = "[";
    bool first = true;
    ((out += (first ? "" : ","), out += value(vm, v), first = false), ...);
    out += "]";
    return out;
    }

  // One engine -> VM entry (call_function, init_instance, allocate_instance,
  // init_opaque_instance). The constructor writes `enter`, ret()/fail() write `return` /
  // `error`. Construct only while active(). In gameplay mode `enter` and `return` are preceded
  // by a sync of the instances the entry sees (arguments, globals, `subject` = the instance
  // being made) and of the VM's globals.
  class Entry final {
    public:
      Entry(zenkit::DaedalusVm& vm, std::string_view vmName, const char* kind,
            const zenkit::DaedalusSymbol* sym, const std::string& args,
            const zenkit::DaedalusInstance* subject = nullptr, const char* ret = nullptr);
      ~Entry();
      Entry(const Entry&)=delete;
      Entry& operator=(const Entry&)=delete;

      void ret(const std::string& value);
      void fail(std::string_view what);

    private:
      zenkit::DaedalusVm*                          machine = nullptr;
      uint64_t                                     index = 0;
      std::string                                  vmName;
      std::vector<const zenkit::DaedalusInstance*> targets;
    };

  // One external call (also the default external). The `ext` event is written when the
  // callback returns (calls it makes into the VM come first), or with "threw":true when
  // it throws. Construct only while active(). In gameplay mode the call is bracketed by syncs
  // of its argument and result instances, the globals' instances and the VM's globals.
  class Ext final {
    public:
      Ext(zenkit::DaedalusVm& vm, std::string_view vmName, std::string_view symbol, std::string args, bool isDefault = false);
      ~Ext();
      Ext(const Ext&)=delete;
      Ext& operator=(const Ext&)=delete;

      void ret(std::string value);

    private:
      zenkit::DaedalusVm*                          machine = nullptr;
      uint64_t    seq = 0;
      uint32_t    depth = 0;
      std::string vmName, symbol, args, result;
      bool        isDefault = false, done = false;
      std::vector<const zenkit::DaedalusInstance*> targets;
    };
  }
