// opengothic-rs (task 5.18): script instance references the stock save can't express.
// GameScript::saveSym writes an NPC symbol as its index in the world's NPC list and an
// item symbol as its world item or as (NPC, item class); an NPC parked outside that list
// (WorldObjects::npcInvalid: invalid spawn point, at the dead point) or an item inside a
// container (a mobsi's inventory) becomes "-1" and the load leaves the symbol empty.
// These are written into the fork-only entry "opengothic-rs/daedalus-refs" (stock
// OpenGothic never opens it) and restored after loadVar.
#pragma once

#include <memory>
#include <vector>

class GameScript;
class Npc;
class Serialize;
class World;

struct OgSaveRefs {
  static void save(Serialize& fout, GameScript& script, World& world);
  static void load(Serialize& fin,  GameScript& script, World& world);
  static const std::vector<std::unique_ptr<Npc>>& invalidNpcs(const World& world);
  };
