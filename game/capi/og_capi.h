#pragma once

/* opengothic-rs: plain C seam between OpenGothic and a host program (Rust).
 * No C++ types cross this boundary. Bump OG_CAPI_VERSION on any ABI change. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_CAPI_VERSION 0

/* Per-frame context; only valid inside the callback that received it. */
typedef struct og_ctx og_ctx;

typedef struct og_callbacks {
  void* user;
  /* Called once per rendered frame on the main thread, also while loading or paused.
   * Counts render() calls, not simulation ticks: some calls advance no game time.
   * Must not throw or unwind: on macOS it runs inside a dispatch block, so nothing
   * would reach og_run's catch (a Rust host wraps it in catch_unwind). May be NULL. */
  void (*on_frame)(void* user, og_ctx* ctx);
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
