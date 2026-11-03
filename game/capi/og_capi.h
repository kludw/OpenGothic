#pragma once

/* opengothic-rs: plain C seam between OpenGothic and a host program (Rust).
 * No C++ types cross this boundary. Bump OG_CAPI_VERSION on any ABI change. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OG_CAPI_VERSION 14

/* Per-frame context; only valid inside the callback that received it. Exception (v14):
 * og_init's context is also the handle for the loop functions until og_shutdown. */
typedef struct og_ctx og_ctx;

/* Levels passed to og_callbacks.on_log. */
#define OG_LOG_ERROR 1
#define OG_LOG_WARN  2
#define OG_LOG_INFO  3
#define OG_LOG_DEBUG 4
#define OG_LOG_TRACE 5

typedef struct og_callbacks {
  void* user;
  /* Called once per rendered frame on the main thread, also while loading or paused
   * (with og_init: once per og_step instead, v14).
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

/* ---- Host-owned loop (v14) ---------------------------------------------------
 * Instead of og_run, a host may own the loop (task 5.2, ADR 0021):
 *
 *   og_ctx* ctx; if(og_init(argc, argv, &cb, &ctx) != 0) fail;
 *   while(og_wait_display(ctx, 50) >= 0 && og_pump_events(ctx) == 1) {
 *     og_step(ctx, dt_ms); og_render(ctx);
 *   }
 *   code = og_shutdown(ctx);
 *
 * All on the main thread, never from inside a callback. og_init and og_run share the
 * one-run-per-process rule. on_frame is called from og_step (once per step, after the
 * simulation), not from og_render. After any OG_RUN_EXCEPTION the host calls
 * og_shutdown and stops. */

/* Builds the game (what og_run does before its event loop) and returns the context
 * for the loop functions in *out_ctx. cb as for og_run (copied; cb->user must outlive
 * og_shutdown). Returns 0, OG_RUN_ALREADY_RUNNING, or OG_RUN_EXCEPTION (logged; the
 * partly built game is destroyed, *out_ctx stays NULL). */
int og_init(int argc, const char** argv, const og_callbacks* cb, og_ctx** out_ctx);

/* Delivers pending window/input events (at most a bounded batch per call) and runs
 * due Tempest timers, as OpenGothic's own loop does after each event.
 * Returns 1 while the game runs, 0 once it should exit (og_request_exit, window
 * closed), OG_RUN_EXCEPTION, or OG_LOOP_BAD_STATE. */
int og_pump_events(og_ctx* ctx);

/* Paces the loop like stock frames: delivers events until the display's next refresh
 * (stock OpenGothic draws one frame per refresh, from a CVDisplayLink), at most
 * timeout_ms. Returns 1 on a refresh, 0 on timeout (no refresh: the display may be
 * asleep, UB-21) or once the game should exit, OG_RUN_EXCEPTION, or OG_LOOP_BAD_STATE. */
int og_wait_display(og_ctx* ctx, uint32_t timeout_ms);

/* One simulation step: OpenGothic's tick with dt_ms of wall time (stock rules: game
 * time advances by min(dt_ms, 50); nothing while loading, paused or in a video), then
 * the animation and camera update, then on_frame. Returns the game ms advanced (>= 0;
 * the host measures its next dt from now), OG_STEP_TOO_SHORT if dt_ms < 5 (nothing
 * advanced, the time is not consumed: the host passes it again next step; on_frame
 * still runs; except in benchmark-CI mode while loading, where stock skips the whole
 * frame and so does this), OG_RUN_EXCEPTION, or OG_LOOP_BAD_STATE. */
int og_step(og_ctx* ctx, uint32_t dt_ms);

/* Draws and presents one frame (UI, world, video capture) and sleeps for the game's
 * frame cap. Returns 0, OG_RUN_EXCEPTION, or OG_LOOP_BAD_STATE. */
int og_render(og_ctx* ctx);

/* Destroys the game (as og_run does when its loop ends) and detaches the callbacks;
 * ctx is invalid afterwards. Returns the game's exit code (0) or OG_LOOP_BAD_STATE
 * (a destructor that throws terminates the process, as under og_run). */
int og_shutdown(og_ctx* ctx);

/* No og_init engine (never initialised, shut down, or started by og_run), a ctx that
 * is not og_init's, or a call from inside a callback. */
#define OG_LOOP_BAD_STATE (-1003)
#define OG_STEP_TOO_SHORT (-1004)

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

/* og_state_query.sections (v4): which parts after header and player are visited. */
#define OG_SECTION_NPCS      0x01u
#define OG_SECTION_ITEMS     0x02u
#define OG_SECTION_WAYPOINTS 0x04u
#define OG_SECTION_DIALOG    0x08u
#define OG_SECTION_INVENTORY 0x10u
#define OG_SECTION_QUESTS    0x20u
#define OG_SECTION_TRADE     0x40u  /* v11 */
#define OG_SECTION_ALL       0x7Fu

typedef struct og_state_query {
  float    npc_range;      /* cm; NPCs farther from the player are not visited */
  float    item_range;     /* cm; same for items lying in the world */
  uint32_t max_waypoints;  /* nearest waynet points to visit */
  uint32_t sections;       /* OG_SECTION_* mask; unknown bits are BAD_ARGS */
} og_state_query;

typedef struct og_state_header {
  int64_t day;     /* 0-based */
  int32_t hour;
  int32_t minute;
  int32_t paused;  /* 1 if the simulation is paused (by anyone), else 0 */
  int32_t video_playing;  /* 1 while a video plays or is queued: the simulation does not advance */
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
  int32_t  in_sight;  /* v5: 1 if no wall/landscape blocks the ray from the player's eyes to the
                       * NPC's centre or head (OpenGothic's free-LOS check, without senses_range) */
} og_npc_state;

typedef struct og_item_state {
  uint32_t id;        /* same lifetime rules as og_npc_state.id */
  og_str   instance;  /* script instance name, e.g. "ITMI_GOLD" */
  og_str   name;
  uint64_t amount;
  og_vec3  position;
  float    distance;
  int32_t  in_sight;  /* v5: 1 if no wall/landscape blocks the ray from the player's eyes to the item */
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
  int32_t  trade;  /* v11: 1 if choosing it opens the trade screen (the script's trade flag) */
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

/* v11: the open trade screen. */
typedef struct og_trade_state {
  uint32_t partner;       /* og_npc_state.id of the trader */
  og_str   partner_name;
} og_trade_state;

#define OG_TRADE_SIDE_TRADER 0  /* the trader's goods; price = what the player pays per piece */
#define OG_TRADE_SIDE_PLAYER 1  /* the player's goods; price = what the trader pays per piece */

/* One row of the trade screen. Gold and the trader's equipped items are not traded and
 * not listed; the player's equipped items are (selling one takes it off). */
typedef struct og_trade_item_state {
  int32_t  side;      /* OG_TRADE_SIDE_* */
  og_str   instance;
  og_str   name;
  uint64_t amount;
  int64_t  price;
} og_trade_item_state;

/* Receives the state, in this order: header, player, npcs (nearest first), items
 * (nearest first), waypoints (nearest first), dialog (at most once) then its
 * options, inventory items, quests (each followed by its entries, oldest first),
 * trade (at most once, only while the trade screen is open) then its items (v11).
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
  void (*trade)         (void* user, const og_trade_state* t);       /* v11 */
  void (*trade_item)    (void* user, const og_trade_item_state* i);  /* v11 */
} og_state_visitor;

#define OG_STATE_OK          0
#define OG_STATE_NOT_IN_GAME 1      /* no world (menu, loading, saving): nothing visited */
#define OG_STATE_BAD_ARGS    (-1)   /* outside on_frame, or bad query/visitor: nothing visited */
#define OG_STATE_EXCEPTION   (-2)   /* C++ exception while reading (logged); visit may be partial */

/* Visits the current game state. Only inside on_frame with its ctx (main thread).
 * Reading the inventory sorts it, as opening the inventory UI does. */
int og_read_state(og_ctx* ctx, const og_state_query* q, const og_state_visitor* v);

/* ---- Watching for events (v4) ----------------------------------------------- */

/* Cheap per-frame values the host diffs to detect events. Counters and hashes are
 * only meaningful compared with an earlier read of the same world_generation. */
typedef struct og_watch {
  uint64_t world_generation;  /* changes when a world load starts; ids restart then */
  int32_t  hp;                /* player hit points */
  int32_t  level;             /* player experience level */
  int32_t  dead;              /* 1 if the player is dead (not just knocked down) */
  uint32_t hits;              /* times the player was hit, blocked hits included (wraps) */
  uint32_t last_attacker;     /* NPC id of the last hitter in this world; 0 = none */
  uint32_t dialog_partner;    /* NPC id of the dialog partner; 0 = no dialog */
  uint64_t dialog_starts;     /* dialogs opened since the process started */
  int32_t  dialog_important;  /* 1 if the last opened dialog began with an important info
                                 (the NPC addressed the player) */
  uint64_t dialog_choices;    /* hash of the shown choice titles; 0 = no choice menu */
  uint64_t quests;            /* hash of quest topics, statuses and entry counts */
  int32_t  paused;            /* 1 if the simulation is paused (by anyone), else 0 (v8) */
} og_watch;

/* Fills *out (OG_STATE_* codes as og_read_state; on NOT_IN_GAME only
 * world_generation and paused are set). Only inside on_frame with its ctx (main thread). */
int og_read_watch(og_ctx* ctx, og_watch* out);

/* ---- Control (v3) ----------------------------------------------------------- */

/* Result codes of the control functions below. */
#define OG_CTL_OK          0
#define OG_CTL_REJECTED    1      /* og_submit: command not possible now; reason written */
#define OG_CTL_NOT_IN_GAME 2      /* no world (menu, loading, saving); reason written */
#define OG_CTL_BAD_ARGS    (-1)   /* outside on_frame, NULL or malformed arguments */
#define OG_CTL_EXCEPTION   (-2)   /* C++ exception (logged) */

/* Freezes (1) or resumes (0) the simulation; rendering continues. The host holds at
 * most one pause: repeated calls with the same value do nothing, and pauses taken by
 * the game itself (menus) are kept. Only inside on_frame. */
int og_set_paused(og_ctx* ctx, int paused);

/* Drops queued videos and stops the playing one, as Escape does. Returns 1 if a video
 * was playing or queued, 0 if not, OG_CTL_BAD_ARGS outside on_frame. */
int og_skip_videos(og_ctx* ctx);

/* v11, tests and debugging only: runs one Marvin console command (OpenGothic's cheat
 * console, e.g. "insert ITMI_GOLD", "goto waypoint NW_CITY_01", "set time 8 0").
 * Returns OG_CTL_OK if the console accepted and ran it, OG_CTL_REJECTED if not (unknown,
 * incomplete or failed command), OG_CTL_NOT_IN_GAME without a world. Only inside on_frame. */
int og_dev_console(og_ctx* ctx, og_str command);

#define OG_CMD_GO_TO         1
#define OG_CMD_TURN_TO       2
#define OG_CMD_TALK_TO       3
#define OG_CMD_CHOOSE_DIALOG 4
#define OG_CMD_WAIT          5
#define OG_CMD_PICK_UP       6
#define OG_CMD_STEP          7
#define OG_CMD_TURN          8
#define OG_CMD_JUMP          9
#define OG_CMD_USE_ITEM     10  /* v9 */
#define OG_CMD_EQUIP        11  /* v9 */
#define OG_CMD_UNEQUIP      12  /* v9 */
#define OG_CMD_ATTACK       13  /* v10 */
#define OG_CMD_FLEE         14  /* v10 */
#define OG_CMD_TRADE        15  /* v11 */
#define OG_CMD_SLEEP        16  /* v12 */
#define OG_CMD_READ         17  /* v13 */
#define OG_CMD_CLOSE_DOCUMENT 18  /* v13 */

/* og_command.trade_op (v11). */
#define OG_TRADE_OPEN 0
#define OG_TRADE_BUY  1
#define OG_TRADE_SELL 2

#define OG_TARGET_WAYPOINT 1  /* name */
#define OG_TARGET_NPC      2  /* id */
#define OG_TARGET_ITEM     3  /* id */
#define OG_TARGET_POSITION 4  /* position */

typedef struct og_target {
  int32_t  kind;      /* OG_TARGET_* */
  uint32_t id;        /* og_npc_state.id / og_item_state.id */
  og_str   name;      /* waypoint name (game bytes); only read during og_submit */
  og_vec3  position;
} og_target;

#define OG_STEP_FORWARD  1
#define OG_STEP_BACKWARD 2
#define OG_STEP_LEFT     3
#define OG_STEP_RIGHT    4

/* One command; only the fields of its kind are read:
 * GO_TO, TURN_TO: target.  TALK_TO: target.id (an NPC).  PICK_UP: target.id (an item).
 * CHOOSE_DIALOG: index (og_dialog_option_state.index).  WAIT: seconds of simulation time
 * (0..86400).
 * STEP: direction + millis of simulation time.  TURN: degrees, positive turns right
 * (clockwise seen from above, heading_deg decreases).  JUMP: nothing.
 * USE_ITEM, EQUIP, UNEQUIP: instance, the script instance name of an item in the player's
 * inventory (og_inventory_item_state.instance, any case). USE_ITEM is for items that are
 * not equipment (potions, food, plants, torches; documents are refused: use READ); it ends when the
 * use animation does. EQUIP (weapons, armour, shields, rings, amulets, belts, runes and scrolls) and
 * UNEQUIP finish at once.
 * ATTACK: target.id (an NPC); the NPC fight AI drives the player (melee weapon, else
 * fists) until the NPC is down; the weapon is put away when the action ends.
 * FLEE: nothing; runs from the nearest NPC that is fighting the player until 15 m away.
 * TRADE (v11): target.id (an NPC), trade_op, and for BUY/SELL instance + amount (> 0).
 * If the trade screen with that NPC is not open, the NPC must be the dialog partner and
 * offer a trade option, which is chosen; the action then waits for the screen. The deal
 * is all or nothing (refused with the reason, e.g. too little gold). The screen stays
 * open afterwards (OG_SECTION_TRADE); a valid CHOOSE_DIALOG closes it first.
 * SLEEP (v12): hour (0..23). The player lies down in the nearest free bed within 10 m,
 * waits for the bed's sleep dialog, runs the scripts' PC_SLEEP(hour) (what that dialog's
 * options call: sets the clock to the next such hour, restores health and mana, ends the
 * dialog) and gets up; finishes once the player stands again.
 * READ (v13): instance, a document item in the player's inventory (map, letter, book), or
 * empty for the player's map of the current world (a carried ITWR_MAP_* item:
 * ITWR_MAP_<WORLD> first, then one whose name contains the world's name, then any). The
 * player uses it as a human would; the action finishes once the game's document window
 * has been drawn (og_capture_frame then shows it), with the window still open. A failed
 * or cancelled READ closes it.
 * CLOSE_DOCUMENT (v13): nothing; closes the open document window (as Esc does), finishes
 * at once; refused when none is open. */
typedef struct og_command {
  int32_t   kind;  /* OG_CMD_* */
  og_target target;
  uint32_t  index;
  float     seconds;
  int32_t   direction;  /* OG_STEP_* */
  uint32_t  millis;
  float     degrees;
  og_str    instance;   /* v9; game bytes, only read during og_submit */
  int32_t   trade_op;   /* v11: OG_TRADE_* */
  uint32_t  amount;     /* v11 */
  int32_t   hour;       /* v12 */
} og_command;

/* Starts a command as the host's one action, replacing (cancelling) a running one.
 * Validated now: unknown/stale ids, unknown waypoints, a command that does not fit the
 * current state (e.g. choose_dialog without a choice menu, moving while in a dialog,
 * a script driving the player) give OG_CTL_REJECTED and leave a running action alone.
 * reason (cap bytes, may be NULL if cap is 0) receives a NUL-terminated, possibly
 * truncated explanation for REJECTED/NOT_IN_GAME, in game bytes (Windows-1252).
 * Only inside on_frame. */
int og_submit(og_ctx* ctx, const og_command* cmd, char* reason, size_t cap);

/* Cancels the running host action (nothing is reported for it). Only inside on_frame. */
int og_cancel_action(og_ctx* ctx);

#define OG_ACTION_NONE     0  /* no host action */
#define OG_ACTION_RUNNING  1
#define OG_ACTION_FINISHED 2  /* reported once, then NONE */
#define OG_ACTION_FAILED   3  /* reported once with a reason, then NONE */

/* Advances the host action by one frame and returns its OG_ACTION_* state (or a
 * negative OG_CTL_* code). Call once per on_frame while an action may run; actions only
 * progress (and only touch the world) while the simulation runs: not while paused or
 * while a video plays. reason as in og_submit, written for FAILED.
 * A world change (load, new game) fails the running action. */
int og_action_update(og_ctx* ctx, char* reason, size_t cap);

/* ---- Frame capture (v6) ------------------------------------------------------ */

typedef struct og_frame_info {
  uint32_t width, height;  /* pixels; 0x0 unless OK or TOO_SMALL */
} og_frame_info;

#define OG_CAPTURE_OK        0
#define OG_CAPTURE_NO_IMAGE  1     /* the window has no area (minimised) or no window yet */
#define OG_CAPTURE_TOO_SMALL 2     /* buf is NULL or cap < width*height*4: info is set, nothing drawn */
#define OG_CAPTURE_BAD_ARGS  (-1)  /* outside on_frame, or info is NULL */
#define OG_CAPTURE_EXCEPTION (-2)  /* C++ exception (logged) */
#define OG_CAPTURE_READBACK  (-3)  /* the GPU readback had an unexpected size or format (logged; og_video_poll drops that copy) */

/* Draws the current frame again offscreen, world and UI as on screen (a playing video
 * included; the world is drawn as of this frame's tick, the UI as last shown), and copies it into buf as RGBA8: rows top to bottom, width*4 bytes each,
 * alpha not meaningful. Wider windows are scaled down (box filter) to max_width with
 * the window's aspect ratio (0 = no limit). Call with buf NULL to learn the size; the
 * size stays the same within one on_frame call. Waits for the GPU (tens of ms), so
 * do not call it every frame. Only inside on_frame (main thread). */
int og_capture_frame(og_ctx* ctx, uint32_t max_width, uint8_t* buf, size_t cap, og_frame_info* info);

/* ---- Saving (v7) --------------------------------------------------------------- */

/* Starts saving the game to path (a file name; game bytes, NUL-terminated) the way a
 * quick save does: the save is written on the game's loading thread over the next
 * frames, with no world loaded meanwhile (og_read_state reports none). Returns
 * OG_CTL_OK once the save has started; OG_CTL_REJECTED (reason written, as in
 * og_submit) when the game will not save now: a dialog or a cutscene runs (like the
 * save menu, a save there is refused); OG_CTL_NOT_IN_GAME without a world or while
 * loading/saving. Follow it with og_poll_save. Only inside on_frame. */
int og_save(og_ctx* ctx, const char* path, char* reason, size_t cap);

typedef struct og_save_status {
  int32_t  busy;      /* 1 while a load or save runs, 0 when idle */
  uint64_t failures;  /* saves that failed since start; compare before/after og_save */
} og_save_status;

/* Fills *out. Only inside on_frame. OG_CTL_OK or OG_CTL_BAD_ARGS. */
int og_poll_save(og_ctx* ctx, og_save_status* out);

/* ---- Video frames (v8) ---------------------------------------------------------- */

typedef struct og_video_frame {
  uint64_t tag;            /* the tag passed to og_video_request */
  uint32_t width, height;  /* pixels */
} og_video_frame;

#define OG_VIDEO_NONE 3  /* og_video_poll: no finished frame yet */

/* Asks for a copy of the next frame drawn, world and UI exactly as presented, scaled
 * down to at most max_width pixels wide like og_capture_frame (0 = no limit). Unlike
 * og_capture_frame it neither drains the GPU nor draws the world twice: that frame is
 * drawn offscreen, presented from there and copied in its own command buffer; the copy
 * becomes available through og_video_poll a few frames later (taking it waits for the
 * GPU queue to reach it: at most about one frame).
 * A second request before the next frame is drawn replaces the first (tag included).
 * No copy is made while the window has no area. OG_CAPTURE_OK, OG_CAPTURE_NO_IMAGE (no
 * window yet), OG_CAPTURE_BAD_ARGS, OG_CAPTURE_EXCEPTION. Only inside on_frame. */
int og_video_request(og_ctx* ctx, uint32_t max_width, uint64_t tag);

/* Takes the oldest finished copy into buf as RGBA8 (rows top to bottom, width*4
 * bytes each, alpha not meaningful) and fills *info: OG_CAPTURE_OK. OG_VIDEO_NONE when
 * none is finished. OG_CAPTURE_TOO_SMALL (buf NULL or cap < width*height*4): info is
 * set and the copy stays queued. OG_CAPTURE_READBACK: the copy had the wrong size and
 * was dropped (logged). OG_CAPTURE_BAD_ARGS, OG_CAPTURE_EXCEPTION as elsewhere. At most
 * 8 finished copies are kept; older ones are dropped (logged), so poll every frame.
 * Only inside on_frame. */
int og_video_poll(og_ctx* ctx, uint8_t* buf, size_t cap, og_video_frame* info);

#ifdef __cplusplus
}
#endif
