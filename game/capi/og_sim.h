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
 * reach the table in the order C++ made them; callbacks must not call OgRng. waynet: the
 * loading thread (waynet_load) and the main thread; never two at once. routine: the
 * loading thread (startup scripts' TA_ and Wld_SetMobRoutine calls, save loading) and the main
 * thread; never two at once. Pointer arguments are non-null (unless documented otherwise)
 * and valid for the call only. */

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
#define OG_SIM_MOD_WAYNET 2 /* WayMatrix::wayTo, WayMatrix::findFreePoint, FpLock (5.13) */
#define OG_SIM_MOD_ROUTINE 3 /* Npc routine tables, WorldObjects mob routines (5.14) */
#define OG_SIM_MODS     4

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

/* A point of the current world's WayMatrix: which list and the index in it, or one of
 * the function-local statics startPoint()/deadPoint() fall back to. */
#define OG_SIM_POINT_NONE          0  /* nullptr */
#define OG_SIM_POINT_WAY           1  /* wayPoints[index] */
#define OG_SIM_POINT_FREE          2  /* freePoints[index] */
#define OG_SIM_POINT_START         3  /* startPoints[index] */
#define OG_SIM_POINT_DEFAULT_START 4  /* startPoint()'s WayPoint(Vec3(), "START") */
#define OG_SIM_POINT_DEFAULT_DEAD  5  /* deadPoint()'s WayPoint(Vec3(-1000,-1000,-1000), "TOT") */
#define OG_SIM_POINT_UNMAPPED      6  /* routines only: a point the NPC's world's matrix
                                         doesn't hold (the fork logs and counts it) */

typedef struct og_sim_point {
  uint32_t kind;   /* OG_SIM_POINT_* */
  uint32_t index;  /* 0 unless WAY/FREE/START */
  } og_sim_point;

/* DynamicWorld::landRay from a point's position, as buildIndex asked it. */
typedef struct og_sim_land_ray {
  float   v[3];     /* the hit, when has_col */
  int32_t has_col;
  } og_sim_land_ray;

/* A ladder mob as calculateLadderPoints sees it. */
typedef struct og_sim_ladder {
  uint32_t mobsi_id;  /* World::mobsiId */
  float    bbox[6];   /* bBox()[0].xyz, bBox()[1].xyz */
  } og_sim_ladder;

/* findFreePoint's filter, run in C++ (the npc ray tests and isLocked): nonzero accepts
 * the point. Only valid during the waynet_find_free_point call it was passed to. */
typedef int32_t (*og_sim_point_filter)(void* ctx, og_sim_point p);

/* One Npc::Routine. */
typedef struct og_sim_routine {
  int64_t        start;     /* gtime ms */
  int64_t        end;
  uint64_t       callback;  /* ScriptFn::ptr; UINT64_MAX = invalid */
  og_sim_point   point;     /* in the NPC's world's matrix; OG_SIM_POINT_NONE = unresolved */
  const uint8_t* name;      /* ADD: TA_Min's way point name; LOAD: fallbackName. Not */
  uint32_t       name_len;  /* NUL-terminated, may be empty (name then may be NULL). */
  } og_sim_routine;

/* routine_npc's op. */
#define OG_SIM_ROUTINE_ADD   0  /* Npc::addRoutine: rows[0], count 1 */
#define OG_SIM_ROUTINE_CLEAR 1  /* Npc::excRoutine's routines.clear(): count 0 */
#define OG_SIM_ROUTINE_LOAD  2  /* Npc::loadAiState: the table as read, in order */
#define OG_SIM_ROUTINE_DROP  3  /* ~Npc: count 0 */

/* routine_current's op and its special indices. */
#define OG_SIM_ROUTINE_CURRENT    0  /* currentRoutine(false) */
#define OG_SIM_ROUTINE_CURRENT_WP 1  /* currentRoutine(true) */
#define OG_SIM_ROUTINE_TA_POINT   2  /* currentTaPoint: the routine whose point it is */
#define OG_SIM_ROUTINE_EMPTY     (-1) /* currentRoutine's static empty routine */
#define OG_SIM_ROUTINE_NPC_WP    (-2) /* TA_POINT with no routines: findPoint(npc wp) */

typedef struct og_sim_mob_routine {
  int64_t time;   /* gtime ms */
  int32_t state;
  } og_sim_mob_routine;

/* One WorldObjects::MobStates as a save holds it. */
typedef struct og_sim_mob_states {
  const uint8_t*            scheme;
  uint32_t                  scheme_len;
  int32_t                   cur_state;
  const og_sim_mob_routine* routines;
  uint32_t                  count;
  } og_sim_mob_states;

/* A scheme (index in WorldObjects::routines) whose state changed. */
typedef struct og_sim_mob_change {
  uint32_t scheme;
  int32_t  state;
  } og_sim_mob_change;

/* waynet_way_to's return value: keep the C++ path. */
#define OG_SIM_KEEP_PATH 0xFFFFFFFFu

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
  /* World's constructor right after WayMatrix::buildIndex: the world's file name
   * (NUL-terminated), the ZEN's bytes as World read them, C++'s point counts, the land
   * ray of every point (way, then free, then start points, in list order: the order
   * buildIndex asked them) and the ladders (mobsiById order). The world becomes the
   * current one; any earlier one is forgotten. Returns 0, or -1 (logged) when Rust
   * couldn't build the same way net: its calls then answer with C++'s results and count
   * as mismatches. */
  int32_t (*waynet_load)(void* user, const char* world, const uint8_t* zen, uint64_t zen_len,
                         uint32_t way_points, uint32_t free_points, uint32_t start_points,
                         const og_sim_land_ray* rays, const og_sim_ladder* ladders,
                         uint32_t ladder_count);
  /* Start of the current world's destructor: no more calls for it. */
  void (*waynet_unload)(void* user);
  /* WayMatrix::wayTo(begin, begin_count, exact_begin, end): `cpp` is C++'s path in pop
   * order (next point first), `out` has room for `out_cap` points. Returns
   * OG_SIM_KEEP_PATH, or (USE mode) the number of points written to `out` in pop order. */
  uint32_t (*waynet_way_to)(void* user, const og_sim_point* begin, uint32_t begin_count,
                            const float* exact_begin, og_sim_point end,
                            const og_sim_point* cpp, uint32_t cpp_count,
                            og_sim_point* out, uint32_t out_cap);
  /* WayMatrix::findFreePoint(at, name, filter): `name` is `name_len` bytes (not
   * NUL-terminated, may be empty), `cpp` C++'s answer. Rust calls `filter(ctx, p)` for
   * its own candidates, in its order. Returns the point to use (Rust's in USE mode). */
  og_sim_point (*waynet_find_free_point)(void* user, const float* at, const uint8_t* name,
                                         uint32_t name_len, og_sim_point_filter filter,
                                         void* ctx, og_sim_point cpp);
  /* FpLock changed a point's use counter: delta +1 (constructor, load) or -1
   * (destructor, move assignment); `cpp_count` is C++'s counter after the change. Returns
   * the counter to use (Rust's in USE mode; C++ adopts it). */
  uint32_t (*waynet_lock)(void* user, og_sim_point p, int32_t delta, uint32_t cpp_count);
  /* A mismatch the fork found itself (a query on a point outside the current way net, a
   * Rust answer it can't map, a filter that threw, a failed load) and logged; `what`
   * (NUL-terminated) names the call. Rust counts it. Never called during another
   * waynet callback. */
  void (*waynet_mismatch)(void* user, const char* what);
  /* An NPC's routine table changed (OG_SIM_ROUTINE_ADD/CLEAR/LOAD/DROP). `npc` is a key
   * (the Npc's address) valid until DROP. */
  void (*routine_npc)(void* user, uint64_t npc, uint32_t op, const og_sim_routine* rows,
                      uint32_t count);
  /* currentRoutine/currentTaPoint at world time `now` (ms): `cpp` is C++'s answer as an
   * index into the NPC's table (or OG_SIM_ROUTINE_EMPTY/NPC_WP), `cpp_routine` that row
   * (NULL for the special indices). Returns the index to use (Rust's in USE mode). */
  int64_t (*routine_current)(void* user, uint64_t npc, uint32_t op, int64_t now, int64_t cpp,
                             const og_sim_routine* cpp_routine);
  /* Npc::endTime of a routine running from `start` to `end` at `now`; returns the end
   * time to use (ms). */
  int64_t (*routine_end_time)(void* user, int64_t start, int64_t end, int64_t now, int64_t cpp);
  /* WorldObjects::setMobRoutine of the world `world` (a key valid until routine_mob_drop):
   * `scheme` is `scheme_len` bytes, not NUL-terminated. */
  void (*routine_mob_set)(void* user, uint64_t world, int64_t time, const uint8_t* scheme,
                          uint32_t scheme_len, int32_t state);
  /* WorldObjects::load read the mob tables: they replace the world's. */
  void (*routine_mob_load)(void* user, uint64_t world, const og_sim_mob_states* states,
                           uint32_t count);
  /* ~WorldObjects. */
  void (*routine_mob_drop)(void* user, uint64_t world);
  /* WorldObjects::tick's mob check at `now`: `cpp` the schemes whose state C++ changed (in
   * table order; C++ updated its curState). Rust writes its changes to `out` (room for
   * `out_cap`, the number of schemes) and returns their count; C++ applies them with
   * setMobState in USE mode, its own otherwise. A count above `out_cap` (Rust returns
   * UINT32_MAX when its changes don't fit) or a scheme index outside the table makes C++
   * apply its own changes and report the mismatch through routine_mismatch. */
  uint32_t (*routine_mob_tick)(void* user, uint64_t world, int64_t now,
                               const og_sim_mob_change* cpp, uint32_t cpp_count,
                               og_sim_mob_change* out, uint32_t out_cap);
  /* resetPositionToTA set every scheme's curState to its state at `now`: `cpp_states` are
   * C++'s, one per scheme in table order. */
  void (*routine_mob_reset)(void* user, uint64_t world, int64_t now, const int32_t* cpp_states,
                            uint32_t count);
  /* The state resetPositionToTA hands an interactive with scheme `scheme`; returns the
   * state to use. */
  int32_t (*routine_mob_scheme_state)(void* user, uint64_t world, const uint8_t* scheme,
                                      uint32_t scheme_len, int32_t cpp);
  /* A mismatch the fork found itself (an unmapped point, an index Rust returned that the
   * table doesn't have) and logged; `what` names the call. Rust counts it. Never called
   * during another routine callback. */
  void (*routine_mismatch)(void* user, const char* what);
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

/* Test seam for og_sim::cxx::sort (task 5.12): std::sort of n `const float*` pointers into
 * `keys` with `*a < *b`, the instantiation WayMatrix's sorts use (pointer elements, a
 * lambda comparator). Writes the sorted order as indices into `keys` to `order`. */
void og_sim_test_sort(const float* keys, uint32_t n, uint32_t* order);

#ifdef __cplusplus
}
#endif
