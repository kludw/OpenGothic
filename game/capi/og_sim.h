/* opengothic-rs: og-sim delegation (task 5.11, ADR 0023), lane A's seam file next to
 * og_capi.h.
 *
 * The fork calls Rust modules (crates/og-sim, through adapters in apps/gothic) at the
 * points where a ported C++ module computes something. Per module a mode:
 *   OG_SIM_OFF     C++ only, the table is not called;
 *   OG_SIM_SHADOW  C++ computes and wins; Rust computes the same from its own state and
 *                  logs every difference (gothic --sim-shadow <mods>);
 *   OG_SIM_USE     C++ still computes (cheap, keeps its state comparable), the Rust
 *                  result is used (gothic --sim-use <mods>).
 * Every call hands Rust the C++ result; in USE mode Rust writes its own result back.
 *
 * Plain C ABI. Callbacks must not throw or unwind (the host wraps them in catch_unwind).
 * time: the main thread, and the loading thread for NEW_GAME/LOADED/STARTED and the
 * startup scripts' Wld_SetTime/Wld_IsTime. rng: any thread (keyed draws come from
 * animation workers when the update is parallel), always under OgRng's lock, so draws
 * reach the table in the order C++ made them; callbacks must not call OgRng. Pointer
 * arguments are non-null and valid for the call only. */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_SIM_OFF    0
#define OG_SIM_SHADOW 1
#define OG_SIM_USE    2

#define OG_SIM_MOD_TIME 0  /* gtime, GameSession::tick's clock, Wld_SetTime, Wld_IsTime */
#define OG_SIM_MOD_RNG  1  /* OgRng: the four named streams (OG_RNG_* in og_capi.h) */
#define OG_SIM_MODS     2

/* GameSession's clock fields. */
typedef struct og_sim_clock {
  uint64_t ticks;            /* tickCount(), session ms */
  int64_t  world_time;       /* wrldTime.toInt(), game ms */
  uint64_t world_time_part;  /* wrldTimePart */
  uint64_t time_mul;         /* timeMul, thousandths */
  uint64_t time_mul_fract;   /* timeMulFract */
  } og_sim_clock;

/* What the clock call was, for og_sim_vtable.time. */
#define OG_SIM_TIME_NEW_GAME    0  /* GameSession(file) after setTime(8:00): the defaults */
#define OG_SIM_TIME_LOADED      1  /* GameSession(Serialize&) after reading game/session: adopt */
#define OG_SIM_TIME_STARTED     2  /* end of GameSession(file): ticks = 1: adopt */
#define OG_SIM_TIME_TICK        3  /* GameSession::tick(a): a = dt after World::scaleTime */
#define OG_SIM_TIME_SET         4  /* GameSession::setTime(t): a = t.toInt() */
#define OG_SIM_TIME_MULTIPLIER  5  /* setTimeMultiplyer(f): f */
#define OG_SIM_TIME_DAY_TIME    6  /* World::setDayTime(h, m): a = h, b = m */

typedef struct og_sim_rng_counters {
  uint64_t draws, keyed_draws, digest, keyed_sum;
  } og_sim_rng_counters;

typedef struct og_sim_vtable {
  void* user;
  /* Clock: `clock` holds the C++ state after the operation; in USE mode Rust overwrites
   * it with its own (C++ adopts it). Returns the dt C++ ticks with for TICK (Rust's in
   * USE mode, else C++'s `cpp_dt`), 0 otherwise. */
  uint64_t (*time)(void* user, uint32_t op, int64_t a, int64_t b, float f,
                   uint64_t cpp_dt, og_sim_clock* clock);
  /* Wld_IsTime(h0, m0, h1, m1) against the current world time; returns the result to
   * use (Rust's in USE mode, else `cpp`). */
  int32_t (*is_time)(void* user, int32_t h0, int32_t m0, int32_t h1, int32_t m1,
                     int32_t cpp);
  /* OgRng::seed. */
  void (*rng_seed)(void* user, uint64_t seed);
  /* OgRng::next / keyed: returns the value to use. */
  uint32_t (*rng_next)(void* user, uint32_t stream, uint32_t cpp);
  uint32_t (*rng_keyed)(void* user, uint32_t stream, uint64_t key, uint32_t cpp);
  /* OgRng::info in USE mode: Rust's counters and state hash. */
  void (*rng_info)(void* user, uint32_t stream, og_sim_rng_counters* counters,
                   uint64_t* state_hash);
  /* OgRng::load: the save's state of one stream (engine = libc++'s mt19937 text);
   * OgRng::load calls it for streams 0..3 in order; Rust replaces its streams only when
 * all four were accepted. 0 on success; on failure the load fails. */
  int32_t (*rng_load)(void* user, uint32_t stream, uint64_t seed,
                      const og_sim_rng_counters* counters, const char* engine);
  } og_sim_vtable;

/* Registers the table (copied) and the modes before og_init/og_run and before
 * og_rng_seed, once per process. `modes[OG_SIM_MODS]`. A module whose mode is OFF never
 * calls the table. Every callback must be set. Returns 0, or
 * -1 (logged) for a NULL argument, a missing callback or an unknown mode. */
int32_t og_sim_set(const og_sim_vtable* vtable, const uint8_t* modes);

/* Test seam for gtime (task 5.11 proptests): applies op to (a, b, c) and returns the
 * result as gametime.h computes it.
 *   0 gtime(a, b).toInt()          (hour, min as int32)
 *   1 gtime(a, b, c).toInt()       (day, hour, min as int64)
 *   2 day(), 3 timeInDay(), 4 hour(), 5 minute() of gtime(a ms)
 *   6 addMilis: gtime(a ms) + uint64(b)
 * Unknown op: INT64_MIN. */
int64_t og_sim_test_gtime(uint32_t op, int64_t a, int64_t b, int64_t c);

#ifdef __cplusplus
}
#endif
