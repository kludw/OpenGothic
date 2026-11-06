/* opengothic-rs: seam for the Rust Daedalus VM (task 4.14, ADR 0022), lane B's file next to
 * og_capi.h, with its own version.
 *
 * Two directions:
 * - The backend (Rust) registers an og_script_backend before og_init/og_run. Every OgVm the
 *   engine creates offers itself to the backend with the DAT bytes C++ loaded; a backend that
 *   takes the VM runs its scripts from then on: OgVm's init_instance, allocate_instance and
 *   call_function go to the backend. Instances stay ZenKit's C++ objects (zenkit::INpc,
 *   ICamera, ...); the backend reads and writes their members through the host functions.
 * - The host functions below (C++, called by the backend while it runs): members under an
 *   instance, the instance registry, and og_script_call_external, which runs OpenGothic's
 *   registered callback for an external against the ZenKit stack.
 *
 * Strings cross as the edition's bytes (pointer + length), never as UTF-8; the backend
 * converts. Instances cross as og_script_inst handles of the host's registry: the registry
 * pins the C++ object (a shared_ptr) from the moment a handle is made until it is released,
 * the outermost backend entry (init_instance, bind_instance, call_function) that was running
 * or came next returns, or the VM is destroyed, so a handle never dangles; a released or
 * unknown handle is an error. Version 2 (task 4.16): the backend keeps no handle between
 * entries; ZenKit's symbols hold the globals (og_script_global_get/set), as the engine reads
 * and writes them itself (SELF/OTHER around its calls, saves, NPC routines).
 *
 * Threads: a VM is used by one thread at a time (the engine's own rule for ZenKit VMs);
 * host functions run on the thread of the backend call that made them. Callbacks must not
 * throw or unwind (a Rust backend wraps them in catch_unwind); host functions catch every
 * C++ exception and report it as OG_SCRIPT_ERROR with the message. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_SCRIPT_VERSION 2

/* Status codes of backend callbacks and host functions. */
#define OG_SCRIPT_OK          0
/* Failed; the message is in the call's error buffer (NUL-terminated, cut to its size). */
#define OG_SCRIPT_ERROR      -1
/* og_script_call_external: no callback and no default external registered for the name. */
#define OG_SCRIPT_NO_EXTERNAL -2
/* A backend callback panicked (the backend logs it). */
#define OG_SCRIPT_PANICKED   -3

/* `flags` bits of `create` (ZenKit's DaedalusVmExecutionFlag; ogvm.cpp checks they match). */
#define OG_SCRIPT_FLAG_ALLOW_NULL_INSTANCE_ACCESS 2u
#define OG_SCRIPT_FLAG_IGNORE_CONST_SPECIFIER     4u

/* An instance in the host's registry. generation 0 never names an instance: {0, 0} is null. */
typedef struct og_script_inst {
  uint32_t slot;
  uint32_t generation;
  } og_script_inst;

typedef enum og_script_kind {
  OG_SCRIPT_VOID     = 0,
  OG_SCRIPT_INT      = 1, /* also bool (0/1) */
  OG_SCRIPT_FLOAT    = 2,
  OG_SCRIPT_STRING   = 3, /* edition bytes */
  OG_SCRIPT_INSTANCE = 4,
  OG_SCRIPT_FUNCTION = 5, /* a function symbol's index, in `i` */
  OG_SCRIPT_IGNORE   = 6, /* call_function's ret_kind only: drop whatever the function returns
                           * (ZenKit's IgnoreReturnValue); OG_SCRIPT_VOID requires a void function */
  OG_SCRIPT_REFERENCE = 7, /* og_script_call_external arguments only (version 2): variable `i`
                            * (a symbol index), element `index`, of instance `inst` for a member.
                            * The host pushes a reference to it as ZenKit's PUSHV does, so a
                            * callback's std::string_view points into the variable, not into a
                            * temporary string a nested script call may reuse. */
  } og_script_kind;

/* One value. `str`/`len` are valid for the call that hands them over: the receiver copies.
 * A string a host function returns stays valid until the next host call on the same VM. */
typedef struct og_script_value {
  uint32_t       kind; /* og_script_kind */
  int32_t        i;
  float          f;
  const char*    str;
  size_t         len;
  og_script_inst inst;
  uint32_t       index; /* OG_SCRIPT_REFERENCE: the element */
  } og_script_value;

/* The host side of one VM (OgVm's bridge); stable for the VM's life. */
typedef struct og_script_host og_script_host;

typedef struct og_script_backend {
  void* user;
  /* Offers a new VM: `name` = its DAT file (GOTHIC.DAT, Camera.dat, ...), `dat` = the bytes
   * the engine parsed, `flags` = OG_SCRIPT_FLAG_* bits. Returns the backend's
   * VM, or NULL to leave this VM on ZenKit. Called before the engine registers externals. */
  void* (*create)(void* user, og_script_host* host, const char* name, const uint8_t* dat, size_t len, uint32_t flags);
  /* The OgVm is gone; handles it held are released by the host. */
  void (*destroy)(void* user, void* vm);
  /* The engine registered a callback for external symbol `symbol` (OgVm::register_external). */
  int32_t (*bind_external)(void* vm, uint32_t symbol, char* err, size_t err_len);
  /* The engine registered a default external (for externals without a callback). */
  void (*bind_default_external)(void* vm);
  /* The engine registered an exception handler: recover from script errors as ZenKit's
   * lenient_vm_exception_handler (the only handler OpenGothic uses). */
  void (*set_lenient)(void* vm);
  /* ZenKit's init_instance(instance, sym): `inst` was just made and bound by the host; bind
   * it to `symbol` in the backend and run the instance's code on it. */
  int32_t (*init_instance)(void* vm, uint32_t symbol, og_script_inst inst, char* err, size_t err_len);
  /* ZenKit's allocate_instance(instance, sym): bind only, no code runs. */
  int32_t (*bind_instance)(void* vm, uint32_t symbol, og_script_inst inst, char* err, size_t err_len);
  /* ZenKit's call_function<R>: `ret_kind` is R (OG_SCRIPT_IGNORE for IgnoreReturnValue).
   * A returned string stays valid until the next call into this VM. */
  int32_t (*call_function)(void* vm, uint32_t symbol, const og_script_value* args, size_t count,
                           uint32_t ret_kind, og_script_value* ret, char* err, size_t err_len);
  /* The instance a global instance variable holds (SELF, OTHER, ...); {0,0} for null. The
   * engine uses ZenKit's symbols instead since version 2 (they hold the globals); both
   * callbacks stay mandatory until the table changes again. */
  int32_t (*get_instance)(void* vm, uint32_t symbol, og_script_inst* out, char* err, size_t err_len);
  int32_t (*set_instance)(void* vm, uint32_t symbol, og_script_inst inst, char* err, size_t err_len);
  } og_script_backend;

/* Registers the backend (copied) before og_init/og_run, once per process; NULL (or never
 * calling this) keeps every VM on ZenKit. `user` must outlive the game. Every callback must
 * be set. */
void og_script_set_backend(const og_script_backend* backend);

/* --- Host functions (backend -> C++) --- */

/* Member `member` (a class member's symbol index), array `index`, of instance `inst`, read
 * as `kind` (OG_SCRIPT_INT, _FLOAT or _STRING) through ZenKit's DaedalusSymbol accessors, so
 * ZenKit's checks (class, type, index) apply and their exceptions become OG_SCRIPT_ERROR. */
int32_t og_script_member_get(og_script_host* host, og_script_inst inst, uint32_t member, uint16_t index,
                             uint32_t kind, og_script_value* out, char* err, size_t err_len);
int32_t og_script_member_set(og_script_host* host, og_script_inst inst, uint32_t member, uint16_t index,
                             const og_script_value* value, char* err, size_t err_len);

/* Global (non-member) variable `symbol`, element `index`, read as `kind` (OG_SCRIPT_INT,
 * _FLOAT, _STRING, or _INSTANCE: the instance an instance symbol holds, pinned) through
 * ZenKit's DaedalusSymbol accessors, with their checks; set likewise (no const check: the
 * backend checks, as ZenKit's VM does before it writes). Version 2. */
int32_t og_script_global_get(og_script_host* host, uint32_t symbol, uint16_t index, uint32_t kind,
                             og_script_value* out, char* err, size_t err_len);
int32_t og_script_global_set(og_script_host* host, uint32_t symbol, uint16_t index,
                             const og_script_value* value, char* err, size_t err_len);

/* The instance symbol `inst` was made for. */
int32_t og_script_instance_symbol(og_script_host* host, og_script_inst inst, uint32_t* symbol,
                                  char* err, size_t err_len);
/* Unpins `inst`; its handle is stale afterwards. */
int32_t og_script_instance_release(og_script_host* host, og_script_inst inst, char* err, size_t err_len);

/* Runs the engine's callback for external `name` (raw bytes, ASCII-folded like ZenKit's
 * symbol lookup): pushes `args` onto the ZenKit stack in order (instances by handle,
 * functions as their index), runs the callback, pops the result as `ret_kind`
 * (OG_SCRIPT_VOID pops nothing). Instances in the result are pinned. Without a callback the
 * engine's default external runs (it logs the name) and `ret` is the kind's default; without
 * either, OG_SCRIPT_NO_EXTERNAL. */
int32_t og_script_call_external(og_script_host* host, const char* name, size_t name_len,
                                const og_script_value* args, size_t count, uint32_t ret_kind,
                                og_script_value* ret, char* err, size_t err_len);

/* `--script-trace` (task 4.13) is recording: externals the backend runs itself must then be
 * reported with og_script_trace_external. 1 or 0. Version 2. */
int32_t og_script_tracing(void);
/* Records a call of external `name` (raw bytes, folded like og_script_call_external) that
 * the backend ran itself, as the trace records the engine's callbacks: `args` in order,
 * `ret` (NULL for a void external), `threw` = it failed. A no-op when not tracing.
 * Version 2 (task 4.16). */
int32_t og_script_trace_external(og_script_host* host, const char* name, size_t name_len,
                                 const og_script_value* args, size_t count, const og_script_value* ret,
                                 int32_t threw, char* err, size_t err_len);
/* Task 4.18: marks the start of recorded action `action` (the MCP call number) running tool
 * `tool` (raw bytes) in a gameplay script trace (`--script-trace-gameplay`), so a replay names
 * the action a divergence happened in. A no-op otherwise. Any thread; no backend needed. */
void og_script_trace_mark(uint64_t action, const char* tool, size_t tool_len);

/* --- Test support (seam tests without a running game) --- */

/* A ZenKit VM for `dat` (flags as `create`) offered to the registered backend like an
 * engine VM, named `name`. Every class is registered as opaque (as og-parity's harness does);
 * externals "concatstrings" and "inttostring" are bound to C++ callbacks with the bodies of
 * Gothic::concatstrings / inttostring when the DAT declares them. NULL with the message in `err` on failure. */
og_script_host* og_script_test_open(const uint8_t* dat, size_t len, const char* name, uint32_t flags,
                                    char* err, size_t err_len);
/* Makes an opaque instance of instance symbol `symbol` and runs init_instance on it through
 * OgVm (the backend when it took the VM). The handle is pinned. */
int32_t og_script_test_init(og_script_host* host, const char* symbol, og_script_inst* out, char* err, size_t err_len);
/* The backend's VM for this host (what `create` returned), or NULL. */
void* og_script_test_backend_vm(og_script_host* host);
void og_script_test_close(og_script_host* host);

#ifdef __cplusplus
}
#endif
