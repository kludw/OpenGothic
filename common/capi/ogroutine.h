#pragma once

// opengothic-rs (task 5.14): the C++ side of `og-parity sim-routine`. With
// `--routine-dump <path>` the game writes, as text lines:
//  - every routine change before the snapshot, in call order: `Npc::addRoutine` (with the
//    name the way point resolved to), `Npc::excRoutine`, `WorldObjects::setMobRoutine`;
//  - one snapshot, right after the new game's startup scripts ran (GameSession): every
//    NPC's routine table and `wp`, the answers of currentRoutine (with and without
//    assertWp), endTime and currentTaPoint at a fixed set of world times, and the mob
//    tables with stateByTime at the same times.
// Names are hex bytes ("-" for empty or "no point"), times raw gtime milliseconds. The
// format is read by crates/og-parity/src/sim_routine.rs, which documents it. Nothing is
// written after the snapshot. Without the flag every function is a no-op.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "game/gametime.h"

class Npc;
class World;
class WayPoint;

struct OgRoutineDump final {
  static void added(const Npc& npc, gtime s, gtime e, uint32_t callback,
                    std::string_view point, const WayPoint* wp);
  static void exchanged(const Npc& npc, size_t callback);
  static void mobRoutine(gtime time, std::string_view scheme, int32_t state);
  // Throws std::runtime_error (logged as "routine dump: …") when the file can't be written.
  static void snapshot(World& world);
  };
