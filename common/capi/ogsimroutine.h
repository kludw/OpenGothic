#pragma once

// opengothic-rs (task 5.14 part 2): the routines' og-sim delegation (OG_SIM_MOD_ROUTINE in
// game/capi/og_sim.h). Npc hands every routine table change (addRoutine, excRoutine's
// clear, loadAiState, ~Npc) and its currentRoutine/currentTaPoint/endTime answers over;
// WorldObjects its mob routine changes (setMobRoutine, load, ~WorldObjects), the
// per-tick mob state check and resetPositionToTA's mob part. NPCs and worlds are keyed by
// address; routine points are (list, index) of the NPC's world's WayMatrix. Only called
// while the module is on.

#include <cstdint>
#include <string_view>

#include "game/gametime.h"
#include "../../game/capi/og_sim.h"

class Npc;
class WayPoint;
class World;
class WorldObjects;

struct OgSimRoutine final {
  static void    added(const Npc& npc, gtime s, gtime e, uint32_t callback,
                       std::string_view name, const WayPoint* wp);
  static void    cleared(const Npc& npc);
  static void    loaded(const Npc& npc);
  static void    dropped(const Npc& npc);
  // op: OG_SIM_ROUTINE_CURRENT/CURRENT_WP/TA_POINT; cpp: C++'s index (or EMPTY/NPC_WP).
  // Returns the index to use; an index the table doesn't have is logged, counted and
  // replaced by C++'s.
  static int64_t current(const Npc& npc, uint32_t op, gtime now, int64_t cpp);
  static gtime   endTime(gtime start, gtime end, gtime now, gtime cpp);

  static void    mobSet(const WorldObjects& w, gtime time, std::string_view scheme, int32_t state);
  static void    mobLoaded(const WorldObjects& w);
  static void    mobDropped(const WorldObjects& w);
  // WorldObjects::tick's mob loop: C++ updates its curState, the chosen changes go to
  // setMobState.
  static void    mobTick(WorldObjects& w);
  // The end of WorldObjects::resetPositionToTA: curStates, then every interactive's state.
  static void    mobReset(WorldObjects& w);

  private:
    static og_sim_point pointOf(const World& world, const WayPoint* wp, const char* what);
  };
