#pragma once

/* opengothic-rs: plain C seam between OpenGothic and a host program (Rust).
 * No C++ types cross this boundary. Bump OG_CAPI_VERSION on any ABI change. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_CAPI_VERSION 1

/* Per-frame context; only valid inside the callback that received it. */
typedef struct og_ctx og_ctx;

/* Levels passed to og_callbacks.on_log. */
#define OG_LOG_ERROR 1
#define OG_LOG_WARN  2
#define OG_LOG_INFO  3
#define OG_LOG_DEBUG 4
#define OG_LOG_TRACE 5

typedef struct og_callbacks {
  void* user;
  /* Called once per rendered frame on the main thread, also while loading or paused.
   * Counts render() calls, not simulation ticks: some calls advance no game time.
   * Must not throw or unwind: on macOS it runs inside a dispatch block, so nothing
   * would reach og_run's catch (a Rust host wraps it in catch_unwind). May be NULL. */
  void (*on_frame)(void* user, og_ctx* ctx);
  /* Receives OpenGothic's log lines instead of log.txt (Tempest still echoes its own
   * lines to stdout/stderr; zenkit and dmusic lines only come here).
   * level is an OG_LOG_* value; source names the logger ("tempest", "zenkit", "dmusic");
   * msg is one line (Tempest splits lines longer than ~255 bytes into several calls).
   * Both strings are NUL-terminated and only valid during the call.
   * Called from any thread, also concurrently, while og_run runs; a call that started
   * just before og_run returns may still finish after it. Must not throw or unwind.
   * May be NULL (then OpenGothic writes log.txt as usual). */
  void (*on_log)(void* user, int level, const char* source, const char* msg);
} og_callbacks;

/* Runs the game until it exits; blocks. Call once per process, on the main thread.
 * argv is passed to OpenGothic's command line parser (argv[0] = program name).
 * cb may be NULL; it is copied, but cb->user must outlive the call.
 * Returns the game's exit code, OG_RUN_EXCEPTION if a C++ exception escaped
 * game setup or the event loop (it is logged), or OG_RUN_ALREADY_RUNNING on a nested/second call. */
int og_run(int argc, const char** argv, const og_callbacks* cb);

#define OG_RUN_EXCEPTION       (-1001)
#define OG_RUN_ALREADY_RUNNING (-1002)

/* 0-based index of the current frame. */
uint64_t og_ctx_frame(const og_ctx* ctx);

/* Asks the game to exit; og_run returns soon after. A few more frames (and
 * on_frame calls) may still arrive before the loop stops. */
void og_request_exit(og_ctx* ctx);

#ifdef __cplusplus
}
#endif
