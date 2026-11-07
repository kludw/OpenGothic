// opengothic-rs (task 5.18): way points the stock save can't name. Serialize writes a
// WayPoint* as its name and the load takes WayMatrix::findPoint(name), the first point of
// that name; 33 names occur more than once in NEWWORLD and in OLDWORLD (2 in ADDONWORLD, 1 in
// DRAGONISLAND), so an NPC on the second FP_ROAM_CITY_TO_FOREST_15 is, after a load, on
// one 3.4 km away (UB-38). Every WayPoint* write is numbered within its entry; a write the
// name alone would resolve to another point is listed with the exact point in the
// fork-only entry "worlds/<world>/opengothic-rs/waypoints" (stock OpenGothic never opens
// it), written after the world's entries and read before them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class Serialize;
class World;
class WayPoint;

struct OgSaveWaypoints {
  // Serialize::implWrite(const WayPoint*): the `seq`-th way point of the open entry.
  static void record(Serialize& fout, const World& world, uint32_t seq, const WayPoint* wp);
  // Serialize::implRead(const WayPoint*&): `wp` is findPoint(name)'s answer; replaced by
  // the listed point when this read is listed.
  static void resolve(Serialize& fin, const World& world, uint32_t seq, std::string_view name,
                      const WayPoint*& wp);
  static void save(Serialize& fout, const World& world);
  static void load(Serialize& fin,  const World& world);
  // End of GameSession::save: a way point recorded after World::save wrote the list
  // would be lost; logged as an error.
  static void checkSaved(Serialize& fout);
  };
