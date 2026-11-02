#pragma once

/* opengothic-rs: plain C seam between OpenGothic and a host program (Rust).
 * No C++ types cross this boundary. Bump OG_CAPI_VERSION on any ABI change. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_CAPI_VERSION 2

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

/* ---- Reading game state (v2) ------------------------------------------------ */

/* Borrowed text, NOT NUL-terminated, valid only during the visitor call that
 * received it. Game texts are the game's own bytes (Windows-1252 for G2 Gold
 * English), not UTF-8. ptr may be NULL only when len is 0. */
typedef struct og_str {
  const char* ptr;
  size_t      len;
} og_str;

/* World position in centimetres; y is up. */
typedef struct og_vec3 {
  float x, y, z;
} og_vec3;

typedef struct og_state_query {
  float    npc_range;      /* cm; NPCs farther from the player are not visited */
  float    item_range;     /* cm; same for items lying in the world */
  uint32_t max_waypoints;  /* nearest waynet points to visit */
} og_state_query;

typedef struct og_state_header {
  int64_t day;     /* 0-based */
  int32_t hour;
  int32_t minute;
  int32_t paused;  /* 1 if the simulation is paused, else 0 */
} og_state_header;

typedef struct og_player_state {
  og_str  name;
  og_str  guild;    /* script constant name, e.g. "GIL_NONE"; empty if unknown */
  og_vec3 position;
  float   heading_deg;  /* yaw about y; forward = (cos a, 0, sin a); not normalised */
  int32_t hp, hp_max, mana, mana_max;
  int32_t level, experience, experience_next, learning_points;
  int32_t strength, dexterity;
} og_player_state;

/* Attitude values (Gothic's ATT_*). */
#define OG_ATT_HOSTILE  0
#define OG_ATT_ANGRY    1
#define OG_ATT_NEUTRAL  2
#define OG_ATT_FRIENDLY 3

typedef struct og_npc_state {
  uint32_t id;        /* stable until the next world load starts (saves keep it); never reused in a process */
  og_str   name;
  og_str   guild;
  og_vec3  position;
  float    distance;  /* cm from the player */
  int32_t  attitude;  /* OG_ATT_*, towards the player (temporary attitude first) */
  int32_t  hp, hp_max;
  int32_t  in_dialog; /* 1 if talking with the player */
} og_npc_state;

typedef struct og_item_state {
  uint32_t id;        /* same lifetime rules as og_npc_state.id */
  og_str   instance;  /* script instance name, e.g. "ITMI_GOLD" */
  og_str   name;
  uint64_t amount;
  og_vec3  position;
  float    distance;
} og_item_state;

typedef struct og_waypoint_state {
  og_str  name;
  og_vec3 position;
  float   distance;
} og_waypoint_state;

typedef struct og_dialog_state {
  uint32_t partner;       /* og_npc_state.id of the dialog partner */
  og_str   partner_name;
  int32_t  has_line;      /* 1 if a line is being spoken; then the two fields below are set */
  og_str   line_speaker;
  og_str   line_text;
} og_dialog_state;

typedef struct og_dialog_option_state {
  uint32_t index;  /* position in the dialog's choice list; what choosing will take (2.3) */
  og_str   text;
} og_dialog_option_state;

/* One row per stack; a non-stackable item can appear twice (equipped copies, rest). */
typedef struct og_inventory_item_state {
  og_str   instance;
  og_str   name;
  uint64_t amount;
  int32_t  equipped;
} og_inventory_item_state;

#define OG_QUEST_MISSION 0
#define OG_QUEST_NOTE    1
#define OG_QUEST_RUNNING  1
#define OG_QUEST_SUCCESS  2
#define OG_QUEST_FAILED   3
#define OG_QUEST_OBSOLETE 4

typedef struct og_quest_state {
  og_str  topic;
  int32_t section;  /* OG_QUEST_MISSION / OG_QUEST_NOTE */
  int32_t status;   /* OG_QUEST_RUNNING .. OG_QUEST_OBSOLETE */
} og_quest_state;

/* Receives the state, in this order: header, player, npcs (nearest first), items
 * (nearest first), waypoints (nearest first), dialog (at most once) then its
 * options, inventory items, quests (each followed by its entries, oldest first).
 * All callbacks must be non-NULL and must not throw or unwind. */
typedef struct og_state_visitor {
  void* user;
  void (*header)        (void* user, const og_state_header* h);
  void (*player)        (void* user, const og_player_state* p);
  void (*npc)           (void* user, const og_npc_state* n);
  void (*item)          (void* user, const og_item_state* i);
  void (*waypoint)      (void* user, const og_waypoint_state* w);
  void (*dialog)        (void* user, const og_dialog_state* d);
  void (*dialog_option) (void* user, const og_dialog_option_state* o);
  void (*inventory_item)(void* user, const og_inventory_item_state* i);
  void (*quest)         (void* user, const og_quest_state* q);
  void (*quest_entry)   (void* user, og_str text);  /* belongs to the last quest */
} og_state_visitor;

#define OG_STATE_OK          0
#define OG_STATE_NOT_IN_GAME 1      /* no world (menu, loading, saving): nothing visited */
#define OG_STATE_BAD_ARGS    (-1)   /* outside on_frame, or bad query/visitor: nothing visited */
#define OG_STATE_EXCEPTION   (-2)   /* C++ exception while reading (logged); visit may be partial */

/* Visits the current game state. Only inside on_frame with its ctx (main thread).
 * Reading the inventory sorts it, as opening the inventory UI does. */
int og_read_state(og_ctx* ctx, const og_state_query* q, const og_state_visitor* v);

#ifdef __cplusplus
}
#endif
