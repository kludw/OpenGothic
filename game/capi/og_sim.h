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
 * thread; never two at once. damage, fight: the main thread; fight_load: the thread that
 * runs Gothic::setupGlobalScripts. quest, table, query: the loading thread (GameScript and
 * World construction, startup scripts, save loading) and the main thread; never two at
 * once. Pointer arguments are non-null (unless documented otherwise)
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
#define OG_SIM_MOD_DAMAGE 4 /* DamageCalculator::damageValue, damageFall (5.15) */
#define OG_SIM_MOD_FIGHTAI 5 /* FightAlgo's move selection, the FAI table (5.16) */
#define OG_SIM_MOD_QUESTLOG 6 /* QuestLog and Log_CreateTopic/SetTopicStatus/AddEntry (5.17) */
#define OG_SIM_MOD_ATTITUDE 7 /* GameScript's guild attitude table, guild/personAttitude (5.17) */
#define OG_SIM_MOD_PERC_RANGES 8 /* GameScript::PerDist: Perc_SetRange, PerDist::at (5.17) */
#define OG_SIM_MOD_ROOMS 9 /* World's portal-room guilds: assignRoomToGuild, guildOfRoom (5.17) */
#define OG_SIM_MODS     10

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

/* What a DamageCalculator call computes (og_sim_damage_in.kind). */
#define OG_SIM_DAMAGE_MELEE  0  /* damageValue without bullet or spell: swordDamage */
#define OG_SIM_DAMAGE_BULLET 1  /* damageValue with a Bullet: rangeDamage(bullet) */
#define OG_SIM_DAMAGE_SPELL  2  /* damageValue(isSpell) without a bullet: rangeDamage(splDmg) */
#define OG_SIM_DAMAGE_FALL   3  /* damageFall */

#define OG_SIM_DAMAGE_TYPES 8   /* zenkit::DamageType::NUM */

/* Everything a DamageCalculator call reads from the NPCs, the bullet, the script and the
 * game version; fields a kind doesn't read are 0. */
typedef struct og_sim_damage_in {
  uint32_t kind;                              /* OG_SIM_DAMAGE_* */
  uint32_t game;                              /* Gothic::version().game: 1 or 2 */
  /* attacker (src) */
  int32_t  strength;                          /* attribute(ATR_STRENGTH) */
  int32_t  src_damage[OG_SIM_DAMAGE_TYPES];   /* handle().damage */
  int32_t  hitchance[3];                      /* handle().hitchance[TALENT_UNKNOWN, 1H, 2H] */
  int32_t  talent[3];                         /* talentValue(TALENT_UNKNOWN, 1H, 2H) */
  int32_t  is_monster;                        /* isMonster() */
  uint32_t weapon;                            /* active weapon: 0 none, 1 one-, 2 two-handed */
  int32_t  damage_type;                       /* damageTypeMask(src) */
  int32_t  crit_mul;                          /* GameScript::criticalDamageMultiplyer */
  /* victim (other) */
  int32_t  protection[OG_SIM_DAMAGE_TYPES];   /* handle().protection */
  /* BULLET: Bullet::damage(); SPELL: splDmg */
  int32_t  damage[OG_SIM_DAMAGE_TYPES];
  float    path_length;                       /* Bullet::pathLength */
  int32_t  bullet_is_spell;                   /* Bullet::isSpell */
  float    hit_chance;                        /* Bullet::hitChance */
  float    crit_chance;                       /* Bullet::critChance */
  uint32_t coll_mask;                         /* CollideMask */
  /* FALL: the falling NPC */
  float    speed;                             /* fallSpeed.length() */
  int32_t  falldown_height;                   /* guildVal().falldown_height[guild] */
  int32_t  falldown_damage;                   /* guildVal().falldown_damage[guild] */
  int32_t  prot_fall;                         /* protection(PROT_FALL) */
  } og_sim_damage_in;

/* DamageCalculator::Val. */
typedef struct og_sim_damage_out {
  int32_t value;
  int32_t has_hit;
  int32_t invincible;
  } og_sim_damage_out;

/* One GameScript::rand(max) call and its answer (OgRng gameplay draw % max). */
typedef struct og_sim_damage_draw {
  uint32_t max;
  uint32_t value;
  } og_sim_damage_draw;

/* zenkit::IFightAi::move_count, FightAi::FA's situation lists (declaration order:
 * enemy_prehit, enemy_stormprehit, my_w_combo, my_w_runto, my_w_strafe, my_w_focus,
 * my_w_nofocus, my_g_combo, my_g_runto, my_g_strafe, my_g_focus, my_fk_focus,
 * my_g_fk_nofocus, my_fk_focus_far, my_fk_nofocus_far, my_fk_focus_mag,
 * my_fk_nofocus_mag) and FightAlgo::MV_MAX. */
#define OG_SIM_FAI_MOVES      6
#define OG_SIM_FAI_SITUATIONS 17
#define OG_SIM_FIGHT_QUEUE    6

/* What FightAlgo reads from one NPC. */
typedef struct og_sim_fight_side {
  int32_t  range_base;    /* guildVal().fight_range_base[guild] */
  int32_t  range_g;       /* fight_range_g */
  int32_t  range_fist;    /* fight_range_fist */
  int32_t  range_1ha;     /* fight_range_1ha */
  int32_t  range_2ha;     /* fight_range_2ha */
  uint32_t weapon_state;  /* WeaponState: 0 none, 1 fist, 2 1H, 3 2H, 4 bow, 5 crossbow, 6 mage */
  int32_t  sword_length;  /* inventory().activeWeapon()->swordLength(), 0 without one */
  int32_t  run;           /* bodyStateMasked()==BS_RUN */
  int32_t  prehit;        /* isPrehit() */
  float    center[3];     /* collosionCenter() */
  float    rotation;      /* rotationRad() */
  float    dist2;         /* fightDistanceTo(the other).quadLength() */
  } og_sim_fight_side;

/* What FightAlgo::fillQueue reads: the NPC, its target, Gothic::version().game and the
 * tactic (handle().fight_tactic, or the seam's hero tactic). */
typedef struct og_sim_fight_in {
  uint32_t          game;
  int32_t           tactic;
  og_sim_fight_side npc;
  og_sim_fight_side tg;
  } og_sim_fight_in;

/* FightAlgo's state (what FightAlgo::save writes). */
typedef struct og_sim_fight_state {
  int32_t queue_id;                  /* zenkit::FightAiMove */
  uint8_t tr[OG_SIM_FIGHT_QUEUE];    /* FightAlgo::Action, the head first */
  uint8_t hit_flg;
  uint8_t pad;                       /* 0 */
  } og_sim_fight_state;

/* og_sim_vtable.fight's op. */
#define OG_SIM_FIGHT_FETCH    0  /* fetchInstructions: result = its bool; `in` set */
#define OG_SIM_FIGHT_NEXT     1  /* nextFromQueue: result = the Action */
#define OG_SIM_FIGHT_CONSUME  2  /* consumeAction: result 0 */
#define OG_SIM_FIGHT_CLEAR    3  /* onClearTarget: result 0 */
#define OG_SIM_FIGHT_TAKE_HIT 4  /* onTakeHit: result 0 */

typedef struct og_sim_fight_out {
  og_sim_fight_state state;  /* after the operation */
  int32_t            result;
  } og_sim_fight_out;

/* Bytes, not NUL-terminated; `data` may be NULL when `len` is 0. */
typedef struct og_sim_str {
  const uint8_t* data;
  uint32_t       len;
  } og_sim_str;

/* One QuestLog::Quest. */
typedef struct og_sim_quest {
  og_sim_str        name;
  uint32_t          section;      /* QuestLog::Section */
  uint32_t          status;       /* QuestLog::Status */
  const og_sim_str* entries;      /* may be NULL when entry_count is 0 */
  uint32_t          entry_count;
  } og_sim_quest;

/* USE mode: replaces the quest log the quest call was about with `quests` (copied). Only
 * valid during that call. */
typedef void (*og_sim_quest_adopt)(void* ctx, const og_sim_quest* quests, uint32_t count);

/* An operation on a small session table (quest, table, query); fields an op doesn't
 * name are 0/empty. */
typedef struct og_sim_table_op {
  uint32_t          op;
  int32_t           a, b, c;
  const int32_t*    values;       /* may be NULL when value_count is 0 */
  uint32_t          value_count;
  og_sim_str        str, str2;
  const og_sim_str* names;        /* may be NULL when name_count is 0 */
  uint32_t          name_count;
  } og_sim_table_op;

/* quest ops (key: the QuestLog's address). */
#define OG_SIM_QUEST_CREATE 0  /* Log_CreateTopic(str, a = section) */
#define OG_SIM_QUEST_STATUS 1  /* Log_SetTopicStatus(str, a = status) */
#define OG_SIM_QUEST_ENTRY  2  /* Log_AddEntry(str, str2) */
#define OG_SIM_QUEST_LOAD   3  /* GameScript::loadQuests read the log: `cpp` is the save's */
#define OG_SIM_QUEST_DROP   4  /* ~GameScript */

/* table/query ops of OG_SIM_MOD_ATTITUDE (key: the GameScript's address; the table is
 * gilAttitudes). */
#define OG_SIM_ATT_INIT     0  /* initCommon: a = GIL_MAX, b = sqrt(TAB_ANZAHL), after the
                                  resize to a*a (ATT_HOSTILE) */
#define OG_SIM_ATT_EXCHANGE 1  /* Wld_ExchangeGuildAttitudes: a = 1 and `values` the b*b ints
                                  read from the symbol, a = 0 when there is no symbol */
#define OG_SIM_ATT_SET      2  /* Wld_SetGuildAttitude(a, b, c) */
#define OG_SIM_ATT_LOAD     3  /* loadQuests: `values` the save's table */
#define OG_SIM_ATT_DROP     4  /* ~GameScript: table NULL */
#define OG_SIM_ATT_GET      0  /* query: Wld_GetGuildAttitude(a, b) */
#define OG_SIM_ATT_GUILD    1  /* query: guildAttitude, a/b = the NPCs' guild() (uint32 bits) */
#define OG_SIM_ATT_PERSON   2  /* query: personAttitude, a/b as GUILD, c = bit 0 first NPC
                                  isPlayer(), bit 1 second; values[0..2] = their attitude() */

/* table/query ops of OG_SIM_MOD_PERC_RANGES (key: the PerDist's address; the table is
 * PerDist::range, PERC_Count entries). */
#define OG_SIM_PERC_SET  0  /* Perc_SetRange(a, b) */
#define OG_SIM_PERC_LOAD 1  /* loadPerc: a = the save's count, `values` its ranges when a is
                               PERC_Count (else none: initPerceptions' sets follow) */
#define OG_SIM_PERC_DROP 2  /* ~GameScript: table NULL */
#define OG_SIM_PERC_AT   0  /* query: PerDist::at(a, b) */

/* table/query ops of OG_SIM_MOD_ROOMS (key: the World's address; the table is
 * sectorsData's guilds, one per BSP sector). */
#define OG_SIM_ROOMS_WORLD     0  /* World's constructor: `names` the sectors' names */
#define OG_SIM_ROOMS_ASSIGN    1  /* Wld_AssignRoomToGuild(str, a) */
#define OG_SIM_ROOMS_LOAD      2  /* World::load: one saved pair (str, a) */
#define OG_SIM_ROOMS_DROP      3  /* ~World: table NULL */
#define OG_SIM_ROOMS_OF_ROOM   0  /* query: guildOfRoom(pos), str = roomAt(pos) */
#define OG_SIM_ROOMS_OF_PORTAL 1  /* query: guildOfRoom(str) */

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
  /* A DamageCalculator call: `in` what it read, `cpp` C++'s result, `draws` the
   * GameScript::rand calls C++'s computation made, in order (`draw_count` of them, may be
   * 0 and then `draws` may be NULL). Rust computes the same, answering its own rand calls
   * with those values in order, and compares result and draws. `out` holds `cpp` on
   * entry; in USE mode Rust overwrites it with its result. */
  void (*damage)(void* user, const og_sim_damage_in* in, const og_sim_damage_draw* draws,
                 uint32_t draw_count, const og_sim_damage_out* cpp, og_sim_damage_out* out);
  /* FightAi's constructor built C++'s table from FIGHT.DAT: `dat` is that file's bytes,
   * `table` C++'s lists, `tactics` x OG_SIM_FAI_SITUATIONS x OG_SIM_FAI_MOVES
   * zenkit::FightAiMove values (tactic 0 included, empty). Rust builds its own table
   * from `dat`; differences count as mismatches. Returns 0, or -1 (logged) when Rust
   * couldn't build one: its fight calls then answer with C++'s results and count as
   * mismatches. */
  int32_t (*fight_load)(void* user, const uint8_t* dat, uint64_t dat_len, const int32_t* table,
                        uint32_t tactics);
  /* A FightAlgo operation on `npc` (the NPC's FightAlgo address, a key for the log): `before` is
   * the state before it, `in` what fillQueue read (FETCH only, else NULL), `draws` the
   * GameScript::rand calls C++ made, in order (`draw_count`, may be 0 and `draws` NULL),
   * `cpp` C++'s state and result after it. Rust runs the operation on `before`, its rand
   * calls answered with those values in order, and compares. `out` holds `cpp` on entry;
   * in USE mode Rust overwrites it (C++ adopts the state and returns the result). */
  void (*fight)(void* user, uint64_t npc, uint32_t op, const og_sim_fight_in* in,
                const og_sim_damage_draw* draws, uint32_t draw_count,
                const og_sim_fight_state* before, const og_sim_fight_out* cpp,
                og_sim_fight_out* out);
  /* The quest log `log` changed (OG_SIM_QUEST_*): `cpp` is C++'s log after it, in order.
   * Rust applies the same op to its own log and compares; in USE mode it calls
   * adopt(ctx, its log) and C++ takes that (DROP: `cpp` NULL, no adopt). */
  void (*quest)(void* user, uint64_t log, const og_sim_table_op* op, const og_sim_quest* cpp,
                uint32_t cpp_count, og_sim_quest_adopt adopt, void* ctx);
  /* A table of `module` (ATTITUDE, PERC_RANGES, ROOMS) changed: `table` holds C++'s `len`
   * entries after the op (DROP: NULL, 0). Rust applies the op to its own table of `key`
   * and compares; in USE mode it overwrites `table` with its own (when the lengths agree;
   * else a mismatch, C++'s kept). */
  void (*table)(void* user, uint32_t module, uint64_t key, const og_sim_table_op* op,
                int32_t* table, uint32_t len);
  /* A read of a table of `module`; `cpp` is C++'s answer. Returns the answer to use
   * (Rust's in USE mode). */
  int32_t (*query)(void* user, uint32_t module, uint64_t key, const og_sim_table_op* op,
                   int32_t cpp);
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

/* Test seam for og_sim::damage (5.15): DamageCalculator's computation of `in` (the code
 * damageValue/damageFall run on what they read), its k-th GameScript::rand(max) call
 * answered with raw[k] % max as GameScript::rand reduces an OgRng draw. Writes the result
 * to `out` and the first `cap` draws to `draws`; returns the number of rand calls, or
 * UINT32_MAX when it made more than `raw_count` (`out` is then unspecified). */
uint32_t og_sim_test_damage(const og_sim_damage_in* in, const uint32_t* raw, uint32_t raw_count,
                            og_sim_damage_out* out, og_sim_damage_draw* draws, uint32_t cap);

/* Test seam for og_sim::fightai (5.16): FightAlgo operation `op` (OG_SIM_FIGHT_*) on
 * state `state` (updated in place), with `tactic` (OG_SIM_FAI_SITUATIONS x
 * OG_SIM_FAI_MOVES values) as the NPC's FAI lists and `in` what fillQueue reads (FETCH
 * only; may be NULL otherwise). The k-th GameScript::rand(max) is answered with raw[k] %
 * max. Writes the result to `result` and the first `cap` draws to `draws`; returns the
 * number of rand calls, UINT32_MAX when it made more than `raw_count` (`state` and
 * `result` are then unspecified), or UINT32_MAX - 1 for an unknown op or a FETCH
 * without `in`. */
uint32_t og_sim_test_fightai(uint32_t op, const og_sim_fight_in* in, const int32_t* tactic,
                             og_sim_fight_state* state, int32_t* result, const uint32_t* raw,
                             uint32_t raw_count, og_sim_damage_draw* draws, uint32_t cap);

#ifdef __cplusplus
}
#endif
