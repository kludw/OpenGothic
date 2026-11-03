// opengothic-rs: og_set_paused / og_submit / og_action_update (game/capi/og_capi.h) —
// drives the player like an AI NPC (AI queue, waynet pathfinding) or through the same
// key actions a human uses (motor commands).

#include "og_capi.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "og_ids.h"
#include "game/constants.h"
#include "game/gamescript.h"
#include "game/inventory.h"
#include "game/playercontrol.h"
#include "ui/dialogmenu.h"
#include "ui/documentmenu.h"
#include "ui/inventorymenu.h"
#include "ui/videowidget.h"
#include "utils/keycodec.h"
#include "world/aiqueue.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "gothic.h"
#include "marvin.h"

namespace {

// Distances in cm. A finished go_to farther than this from its target counts as failed.
constexpr float ARRIVE_POINT = 300;
constexpr float ARRIVE_NPC   = 500;
constexpr float ARRIVE_ITEM  = 300;
// Without a waypoint at the exact spot, go_to(position) ends at the nearest waypoint.
constexpr float ARRIVE_POS   = 500;
constexpr float TALK_RANGE   = 300;
// Items within this (player centre to item centre) can be taken. AI_GotoItem alone stops
// only within 20-60 cm, which items on tables or shelves never allow.
constexpr float ITEM_REACH   = 150;
// Simulation milliseconds.
constexpr uint64_t TALK_TIMEOUT = 5000;
constexpr uint64_t TURN_TIMEOUT = 6000;
constexpr uint64_t JUMP_TIMEOUT = 5000;
constexpr uint64_t JUMP_START   = 1500;
constexpr uint64_t TAKE_TIMEOUT = 1000;
// use_item: time to get the use animation going, then to finish it.
constexpr uint64_t USE_START    = 1000;
constexpr uint64_t USE_TIMEOUT  = 15000;
constexpr uint64_t DRAW_TIMEOUT = 3000;
// The fight AI re-queues AI_Attack while it finds no move; this long means it never will.
constexpr uint64_t FIGHT_STUCK  = 5000;
constexpr uint64_t FLEE_TIMEOUT = 30000;
constexpr float    FLEE_SAFE    = 1500;
// The trade option's lines are spoken before the trade screen opens.
constexpr uint64_t TRADE_OPEN_TIMEOUT = 30000;
// sleep: walking to the bed and lying down until its sleep dialog shows, then getting up.
constexpr uint64_t SLEEP_LIE_TIMEOUT   = 30000;
constexpr uint64_t SLEEP_GETUP_TIMEOUT = 15000;
// read: frames the document window must have been open before the action finishes, so
// the UI og_capture_frame draws (the last frame's) has it.
constexpr int      READ_FRAMES  = 3;
// read: time to be standing (a pick-up's animation can still play, seen > 1 s).
constexpr uint64_t READ_START   = 3000;
// Bed schemes (the visual's name up to its first '_'); scripts use these three.
constexpr std::string_view BED_SCHEMES[] = {"BEDHIGH","BEDLOW","BED"};
// Farther than this, attack walks the waynet first: the fight AI's own pathing went back
// and forth at ~20 m from a field raider by Xardas' tower for minutes.
constexpr float    ATTACK_NEAR  = 500;
constexpr float    TURN_DONE    = 2.f;  // degrees
// Upper bound for wait: keeps the seconds->ms conversion defined (og_capi.h).
constexpr float    WAIT_MAX_S   = 86400.f;

enum class Kind : uint8_t { None, GoTo, Turn, TalkTo, Wait, PickUp, Step, Jump, UseItem, Attack, Flee, Trade, Sleep, Read, Done };

struct Action {
  Kind             kind  = Kind::None;
  int              phase = 0;
  uint64_t         generation = 0;
  uint64_t         start = 0;
  uint64_t         until = 0;
  // Targets; NPCs and items are re-resolved by id every frame.
  int32_t          target = 0;  // OG_TARGET_*
  uint32_t         id     = 0;
  const WayPoint*  wp     = nullptr;
  Tempest::Vec3    pos    = {};
  float            arrive = 0;
  // Turn: heading to reach, or (turnTo) recomputed from the target each frame.
  bool             turnTo  = false;
  float            heading = 0;
  // Pressed key action (motor commands), released on end or cancel.
  KeyCodec::Action key    = KeyCodec::Idle;
  bool             usesAi = false;
  // use_item: item class, stack size before, and whether the use animation was seen.
  size_t           cls     = 0;
  size_t           count   = 0;
  bool             seenUse = false;
  // attack/flee: the OgHooks flags are set; when the last AI_Attack/AI_Flee was queued.
  bool             fight   = false;
  uint64_t         queued  = 0;
  uint64_t         logged  = 0;
  // trade: OG_TRADE_*, upper-case item instance name and pieces (BUY/SELL).
  int32_t          tradeOp = OG_TRADE_OPEN;
  std::string      ware;
  uint32_t         amount  = 0;
  // sleep: the bed's scheme, its top state and the hour to wake up at.
  std::string      mob;
  int32_t          mobState = 0;
  int32_t          hour     = 0;
  // read: update() calls that saw the document window open.
  int              docFrames = 0;
  };

Action act;
bool   hostPause = false;

void write(char* out, size_t cap, std::string_view msg) {
  if(out==nullptr || cap==0)
    return;
  const size_t n = std::min(cap-1,msg.size());
  std::copy_n(msg.data(),n,out);
  out[n] = '\0';
  }

std::string meters(float cm) {
  char buf[32];
  std::snprintf(buf,sizeof(buf),"%.1f m",double(cm/100.f));
  return buf;
  }

float wrapDeg(float a) {
  a = std::fmod(a,360.f);
  if(a>180.f)
    a -= 360.f;
  if(a<=-180.f)
    a += 360.f;
  return a;
  }

// Heading (og_player_state.heading_deg convention) that faces from `from` to `to`.
float headingTo(const Tempest::Vec3& from, const Tempest::Vec3& to) {
  return std::atan2(to.z-from.z,to.x-from.x)*180.f/float(M_PI);
  }

float dist(const Npc& pl, const Tempest::Vec3& p) {
  return (pl.position()-p).length();
  }

bool inReach(const Npc& pl, const Item& it) {
  return (pl.centerPosition()-it.midPosition()).length()<=ITEM_REACH;
  }

void press(KeyCodec::Action k) {
  if(OgHooks::playerControl==nullptr)
    return;
  OgHooks::playerControl->onKeyPressed(k,Tempest::Event::K_NoKey,KeyCodec::Mapping::Secondary);
  act.key = k;
  }

void releaseKey() {
  if(act.key==KeyCodec::Idle)
    return;
  if(OgHooks::playerControl!=nullptr)
    OgHooks::playerControl->onKeyReleased(act.key,KeyCodec::Mapping::Secondary);
  act.key = KeyCodec::Idle;
  }

// Gives the player back to PlayerControl after attack/flee. pl: as in cancel().
void endFight(Npc* pl) {
  if(!act.fight)
    return;
  OgHooks::hostDrivesPlayer    = false;
  OgHooks::hostFightsForPlayer = false;
  act.fight = false;
  if(pl==nullptr || act.generation!=OgHooks::worldGeneration)
    return;
  // Left-over moves would make the next attack or flee act on them (implAttack).
  pl->ogClearFightMoves();
  pl->clearGoTo();
  }

// Puts the weapon away after an attack (talk_to and use_item need free hands; the next
// attack draws again). The game refuses while a swing still plays: then the attack's
// sheathe phase retries; elsewhere (cancel, fail) it is logged.
void sheathe(Npc* pl) {
  if(act.kind!=Kind::Attack || act.phase<1 || pl==nullptr || act.generation!=OgHooks::worldGeneration)
    return;
  if(pl->weaponState()!=WeaponState::NoWeapon && !pl->closeWeapon(false))
    Tempest::Log::i("og_control: the weapon could not be put away (animation busy)");
  }

bool inDialog();
bool endBedDialog(World& w, Npc& pl);

bool documentOpen() {
  return OgHooks::documentMenu!=nullptr && OgHooks::documentMenu->isActive();
  }

void closeDocument() {
  if(!documentOpen())
    return;
  OgHooks::documentMenu->close();
  Tempest::Log::i("og_control: closed the document window");
  }

// Ends the current action without reporting it. pl: the player of the action's world,
// or nullptr if that world is gone.
void cancel(Npc* pl) {
  releaseKey();
  endFight(pl);
  sheathe(pl);
  const bool same = pl!=nullptr && act.generation==OgHooks::worldGeneration;
  if(act.usesAi && same)
    pl->clearAiQueue();
  // A cancelled sleep must not leave the hero lying in the bed, nor in its dialog (the
  // get-up waits in the AI queue until the dialog is gone). If END can't be picked yet,
  // the agent can still choose it.
  if(act.kind==Kind::Sleep && same && pl->interactive()!=nullptr) {
    const bool bedDialog = OgHooks::dialogMenu!=nullptr && OgHooks::dialogMenu->ogView().partner==pl;
    if(bedDialog && !endBedDialog(pl->world(),*pl))
      Tempest::Log::i("og_control: the bed's dialog stays open; choose its END option");
    pl->aiPush(AiQueue::aiUseMob(act.mob,-1));
    }
  // A read that didn't finish must not leave the window over the game.
  if(act.kind==Kind::Read)
    closeDocument();
  act = Action{};
  }

// The nearest NPC that is up and has the player as its target with a weapon (or claws)
// out, i.e. is fighting the player.
Npc* nearestAttacker(World& w, Npc& pl) {
  Npc*  best = nullptr;
  float bd   = 0;
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    Npc* n = w.npcById(i);
    if(n==nullptr || n==&pl || n->isDown() || n->target()!=&pl || n->weaponState()==WeaponState::NoWeapon)
      continue;
    const float d = dist(pl,n->position());
    if(best==nullptr || d<bd) {
      best = n;
      bd   = d;
      }
    }
  return best;
  }

// Takes the player from PlayerControl. attack: also lets the NPC fight AI drive it;
// heroes have no fight tactic of their own (0 has no moves), so they fight like a strong
// human. Flee only runs (AI_Flee), with the fight AI off so a hit can't turn it around.
bool startFight(World& w, bool attack, std::string& why) {
  if(attack) {
    auto* sym = w.script().getVm().find_symbol_by_name("FAI_HUMAN_STRONG");
    if(sym==nullptr) {
      why = "the scripts define no FAI_HUMAN_STRONG fight tactic";
      return false;
      }
    OgHooks::playerFightTactic   = sym->get_int();
    OgHooks::hostFightsForPlayer = true;
    }
  OgHooks::hostDrivesPlayer = true;
  act.fight = true;
  Tempest::Log::i("og_control: ",attack ? "fight AI drives the player (tactic for heroes " : "player flees (",
                  OgHooks::playerFightTactic,")");
  return true;
  }

struct Current {
  Gothic& g;
  World*  w;
  Npc*    pl;
  bool ok() const { return g.checkLoading()==Gothic::LoadState::Idle && w!=nullptr && pl!=nullptr; }
  };

Current current() {
  Gothic& g = Gothic::inst();
  return Current{g,g.world(),g.player()};
  }

bool inDialog() {
  return OgHooks::dialogMenu!=nullptr && OgHooks::dialogMenu->ogView().partner!=nullptr;
  }

// Picks END (PC_NOSLEEP_INFO) in the hero's bed dialog, as a click would; false while the
// menu shows no options (e.g. it waits for queued output) or has no such option.
bool endBedDialog(World& w, Npc& pl) {
  if(OgHooks::dialogMenu==nullptr)
    return false;
  const DialogMenu::OgView v = OgHooks::dialogMenu->ogView();
  if(v.partner!=&pl || v.choices==nullptr)
    return false;
  const size_t end = w.script().findSymbolIndex("PC_NOSLEEP_INFO");
  if(end==size_t(-1))
    return false;
  for(size_t i=0; i<v.choices->size(); ++i)
    if((*v.choices)[i].scriptFn==end)
      return OgHooks::dialogMenu->ogChoose(i);
  return false;
  }

// The trade partner while the trade screen is open, else nullptr.
Npc* trader() {
  return OgHooks::inventoryMenu==nullptr ? nullptr : OgHooks::inventoryMenu->ogTrader();
  }

void closeTrade() {
  if(trader()==nullptr)
    return;
  OgHooks::inventoryMenu->close();
  Tempest::Log::i("og_control: closed the trade screen");
  }

// Pieces of cls the trader's page of the trade screen offers: not equipped, not gold
// (Inventory::T_Trade, InventoryMenu's TradePage). The hero's page lists the whole
// inventory (Npc::sellItem unequips what it sells).
size_t tradeable(const Inventory& inv, size_t cls) {
  size_t n = 0;
  for(auto i=inv.iterator(Inventory::T_Trade); i.isValid(); ++i)
    if((*i).clsId()==cls)
      n += i.count();
  return n;
  }

// Runs the action's BUY/SELL on the open trade screen with tr, all or nothing.
// Returns "" when done, else why not (nothing changed then).
std::string deal(World& w, Npc& pl, Npc& tr, const Action& a) {
  const std::string& name = a.ware;
  const size_t cls  = w.script().findSymbolIndex(name);
  const auto*  gold = w.script().goldId();
  if(cls==size_t(-1))
    return "unknown item '"+name+"'";
  if(gold!=nullptr && cls==gold->index())
    return "gold is what you pay with; it is not traded";
  const std::string who(tr.displayName());
  const size_t   goldBefore = pl.inventory().goldCount();
  const uint64_t n          = a.amount;
  if(a.tradeOp==OG_TRADE_SELL) {
    const size_t have = pl.inventory().itemCount(cls);
    if(have==0)
      return "no '"+name+"' in the inventory";
    if(have<n)
      return "the hero has only "+std::to_string(have)+" '"+name+"' to sell";
    const int64_t price  = pl.inventory().sellPriceOf(cls);
    // Npc::sellItem adds size_t(price)*n gold: a negative price would wrap.
    if(price<0)
      return "'"+name+"' has a negative value ("+std::to_string(price)+"); not sold";
    const size_t  before = pl.inventory().itemCount(cls);
    pl.sellItem(cls,tr,size_t(n));
    const int64_t got = int64_t(pl.inventory().goldCount())-int64_t(goldBefore);
    if(pl.inventory().itemCount(cls)+n!=before || got!=price*int64_t(n)) {
      Tempest::Log::e("og_control: sale of ",n," ",name," went wrong: price ",price,", gold +",got);
      return "the game did not complete the sale";
      }
    Tempest::Log::i("og_control: sold ",n," ",name," to ",who," for ",got," gold");
    return "";
    }
  const size_t have = tradeable(tr.inventory(),cls);
  if(have==0)
    return who+" does not sell '"+name+"'";
  if(have<n)
    return who+" has only "+std::to_string(have)+" '"+name+"'";
  const int64_t price = tr.inventory().priceOf(cls);
  const int64_t cost  = price*int64_t(n);
  if(cost>int64_t(goldBefore))
    return std::to_string(n)+" '"+name+"' cost "+std::to_string(cost)+" gold ("+std::to_string(price)+
           " each); the hero has "+std::to_string(goldBefore);
  const size_t before = pl.inventory().itemCount(cls);
  pl.buyItem(cls,tr,size_t(n));
  const int64_t paid = int64_t(goldBefore)-int64_t(pl.inventory().goldCount());
  if(pl.inventory().itemCount(cls)!=before+n || paid!=cost) {
    Tempest::Log::e("og_control: purchase of ",n," ",name," went wrong: cost ",cost,", paid ",paid);
    return "the game did not complete the purchase";
    }
  Tempest::Log::i("og_control: bought ",n," ",name," from ",who," for ",paid," gold");
  return "";
  }

// Where a target currently is; false (with reason) if it is gone.
bool targetPos(World& w, const Action& a, Tempest::Vec3& out, std::string& why) {
  switch(a.target) {
    case OG_TARGET_WAYPOINT:
      out = a.wp->position();
      return true;
    case OG_TARGET_POSITION:
      out = a.pos;
      return true;
    case OG_TARGET_NPC:
      if(Npc* n = ogIds().findNpc(w,a.id)) {
        out = n->position();
        return true;
        }
      why = "the NPC is no longer in the world";
      return false;
    case OG_TARGET_ITEM:
      if(Item* i = ogIds().findItem(w,a.id)) {
        out = i->position();
        return true;
        }
      why = "the item is no longer lying in the world";
      return false;
    }
  why = "bad target";
  return false;
  }

// Fills act's target fields from a command target; false + reason if it can't be used.
bool resolveTarget(World& w, Npc& pl, const og_target& t, Action& a, std::string& why) {
  a.target = t.kind;
  switch(t.kind) {
    case OG_TARGET_WAYPOINT: {
      if(t.name.len>0 && t.name.ptr==nullptr) {
        why = "bad waypoint name";
        return false;
        }
      const std::string name(t.name.ptr==nullptr ? "" : t.name.ptr,t.name.len);
      a.wp = w.findPoint(name,false);
      if(a.wp==nullptr || a.wp->name!=name) {
        why = "unknown waypoint '"+name+"'";
        a.wp = nullptr;
        return false;
        }
      a.arrive = ARRIVE_POINT;
      return true;
      }
    case OG_TARGET_POSITION: {
      if(!std::isfinite(t.position.x) || !std::isfinite(t.position.y) || !std::isfinite(t.position.z)) {
        why = "position is not finite";
        return false;
        }
      a.pos    = Tempest::Vec3(t.position.x,t.position.y,t.position.z);
      a.wp     = w.findWayPoint(a.pos);
      a.arrive = ARRIVE_POS;
      return true;
      }
    case OG_TARGET_NPC: {
      Npc* n = ogIds().findNpc(w,t.id);
      if(n==&pl) {
        why = "that id is the player";
        return false;
        }
      if(n==nullptr) {
        why = "unknown NPC id "+std::to_string(t.id)+" (ids come from snapshots of this world)";
        return false;
        }
      a.id     = t.id;
      a.arrive = ARRIVE_NPC;
      return true;
      }
    case OG_TARGET_ITEM: {
      if(ogIds().findItem(w,t.id)==nullptr) {
        why = "unknown item id "+std::to_string(t.id)+" (not lying in the world, or never seen in a snapshot)";
        return false;
        }
      a.id     = t.id;
      a.arrive = ARRIVE_ITEM;
      return true;
      }
    }
  why = "unknown target kind "+std::to_string(t.kind);
  return false;
  }

// Pushes the AI walk towards act's target.
bool pushGoTo(World& w, Npc& pl, Action& a, std::string& why) {
  switch(a.target) {
    case OG_TARGET_WAYPOINT:
    case OG_TARGET_POSITION:
      if(a.wp==nullptr) {
        why = "the world has no waypoints";
        return false;
        }
      pl.aiPush(AiQueue::aiGoToPoint(*a.wp));
      break;
    case OG_TARGET_NPC:
      pl.aiPush(AiQueue::aiGoToNpc(ogIds().findNpc(w,a.id)));
      break;
    case OG_TARGET_ITEM:
      pl.aiPush(AiQueue::aiGotoItem(*ogIds().findItem(w,a.id)));
      break;
    }
  a.usesAi = true;
  return true;
  }

// Slots Inventory::use puts these in (equip/unequip); everything else is used up or toggled.
bool equippable(const Item& it) {
  const auto main = uint32_t(it.handle().main_flag);
  const auto flag = uint32_t(it.handle().flags);
  return (main & (ITM_CAT_NF|ITM_CAT_FF|ITM_CAT_ARMOR|ITM_CAT_RUNE))!=0 ||
         (flag & (ITM_SHIELD|ITM_BELT|ITM_AMULET|ITM_RING))!=0;
  }

std::string attributeName(int32_t a) {
  switch(a) {
    case ATR_HITPOINTS:    return "hit points";
    case ATR_HITPOINTSMAX: return "max hit points";
    case ATR_MANA:         return "mana";
    case ATR_MANAMAX:      return "max mana";
    case ATR_STRENGTH:     return "strength";
    case ATR_DEXTERITY:    return "dexterity";
    }
  return "attribute "+std::to_string(a);
  }

// Why the player can't equip it, or "" if the item's requirements are met.
std::string unmetCondition(const Npc& pl, const Item& it) {
  int32_t a = 0, v = 0;
  if(!it.checkCondUse(pl,a,v))
    return "needs "+attributeName(a)+" "+std::to_string(v)+" (the hero has "+std::to_string(pl.attribute(Attribute(a)))+")";
  if(!it.checkCondRune(pl,a,v))
    return "needs magic circle "+std::to_string(v)+" (the hero has "+std::to_string(a)+")";
  return "";
  }

// The stack of the player's inventory that s (an instance name, any case) names.
Item* heldItem(World& w, Npc& pl, const og_str& s, size_t& cls, std::string& name, std::string& why) {
  if(s.len>0 && s.ptr==nullptr) {
    why = "bad item instance name";
    return nullptr;
    }
  name.assign(s.ptr==nullptr ? "" : s.ptr,s.len);
  for(auto& c:name)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  if(name.empty()) {
    why = "no item instance name given";
    return nullptr;
    }
  cls = w.script().findSymbolIndex(name);
  Item* it = cls==size_t(-1) ? nullptr : pl.getItem(cls);
  if(it==nullptr)
    why = "no '"+name+"' in the inventory";
  return it;
  }

// The player's map of world w (READ with no instance), see og_capi.h; nullptr + why if none.
const Item* worldMap(World& w, Npc& pl, size_t& cls, std::string& name, std::string& why) {
  std::string stem(w.name());
  stem = stem.substr(0,stem.find('.'));
  for(auto& c:stem)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  const std::string exact = "ITWR_MAP_"+stem;
  const Item* best = nullptr;
  int         rank = 0;
  for(auto i=pl.inventory().iterator(Inventory::T_Inventory); i.isValid(); ++i) {
    const Item& it = *i;
    auto* sym = w.script().findSymbol(it.clsId());
    if(sym==nullptr || (uint32_t(it.handle().main_flag) & ITM_CAT_DOCS)==0)
      continue;
    const std::string n(sym->name());
    if(n.rfind("ITWR_MAP_",0)!=0)
      continue;
    const int r = n==exact ? 3 : (!stem.empty() && n.find(stem)!=std::string::npos) ? 2 : 1;
    if(r>rank) {
      best = &it;
      rank = r;
      name = n;
      cls  = it.clsId();
      }
    }
  if(best==nullptr)
    why = "the hero carries no map (no ITWR_MAP_* item); maps are bought or found";
  return best;
  }

KeyCodec::Action stepKey(int32_t dir) {
  switch(dir) {
    case OG_STEP_FORWARD:  return KeyCodec::Forward;
    case OG_STEP_BACKWARD: return KeyCodec::Back;
    case OG_STEP_LEFT:     return KeyCodec::Left;
    case OG_STEP_RIGHT:    return KeyCodec::Right;
    }
  return KeyCodec::Idle;
  }

int reject(char* out, size_t cap, std::string_view why) {
  Tempest::Log::i("og_submit: rejected: ",why);
  write(out,cap,why);
  return OG_CTL_REJECTED;
  }

int submit(const og_command& cmd, char* out, size_t cap) {
  Current s = current();
  if(!s.ok()) {
    write(out,cap,"not in game (menu, loading or saving)");
    return OG_CTL_NOT_IN_GAME;
    }
  World& w  = *s.w;
  Npc&   pl = *s.pl;

  if(cmd.kind==OG_CMD_CLOSE_DOCUMENT) {
    if(!documentOpen())
      return reject(out,cap,"no document is open");
    cancel(&pl);
    closeDocument();
    act            = Action{};
    act.generation = OgHooks::worldGeneration;
    act.kind       = Kind::Done;
    return OG_CTL_OK;
    }

  if(pl.isDead())
    return reject(out,cap,"the player is dead");

  const bool dialog = inDialog();
  if(dialog && cmd.kind!=OG_CMD_CHOOSE_DIALOG && cmd.kind!=OG_CMD_WAIT && cmd.kind!=OG_CMD_TRADE)
    return reject(out,cap,"in a dialog: only choose_dialog, trade and wait are possible");
  // Scripts (cutscenes, dialogs) also use the player's AI queue; never fight them.
  const bool ownAi = act.kind!=Kind::None && act.usesAi;
  if(!dialog && !ownAi && !pl.isAiQueueEmpty() && cmd.kind!=OG_CMD_WAIT)
    return reject(out,cap,"a game script is controlling the player right now; wait");

  Action      a;
  std::string why;
  a.generation = OgHooks::worldGeneration;
  a.start      = w.tickCount();

  switch(cmd.kind) {
    case OG_CMD_GO_TO:
      if(!resolveTarget(w,pl,cmd.target,a,why))
        return reject(out,cap,why);
      a.kind = Kind::GoTo;
      break;
    case OG_CMD_TURN_TO:
      if(!resolveTarget(w,pl,cmd.target,a,why))
        return reject(out,cap,why);
      a.kind   = Kind::Turn;
      a.turnTo = true;
      break;
    case OG_CMD_TALK_TO: {
      og_target t = cmd.target;
      t.kind = OG_TARGET_NPC;
      if(!resolveTarget(w,pl,t,a,why))
        return reject(out,cap,why);
      Npc* n = ogIds().findNpc(w,a.id);
      if(n->isDown())
        return reject(out,cap,"the NPC is unconscious or dead");
      if(pl.weaponState()!=WeaponState::NoWeapon)
        return reject(out,cap,"can't talk with a weapon drawn");
      a.kind   = Kind::TalkTo;
      a.arrive = TALK_RANGE;
      break;
      }
    case OG_CMD_PICK_UP: {
      og_target t = cmd.target;
      t.kind = OG_TARGET_ITEM;
      if(!resolveTarget(w,pl,t,a,why))
        return reject(out,cap,why);
      a.kind = Kind::PickUp;
      break;
      }
    case OG_CMD_CHOOSE_DIALOG: {
      if(OgHooks::dialogMenu==nullptr || !dialog)
        return reject(out,cap,"not in a dialog");
      const DialogMenu::OgView v = OgHooks::dialogMenu->ogView();
      if(v.choices==nullptr)
        return reject(out,cap,"no dialog options are shown right now; wait for them");
      if(cmd.index>=v.choices->size() || (*v.choices)[cmd.index].title.empty())
        return reject(out,cap,"no dialog option with index "+std::to_string(cmd.index));
      // While the trade screen is open the menu waits behind it (isChoiceMenuActive):
      // close it only once the choice is known to be valid.
      closeTrade();
      cancel(&pl);
      if(!OgHooks::dialogMenu->ogChoose(cmd.index))
        return reject(out,cap,"the dialog did not accept the choice");
      act      = a;
      act.kind = Kind::Done;
      Tempest::Log::i("og_submit: chose dialog option ",cmd.index);
      return OG_CTL_OK;
      }
    case OG_CMD_WAIT:
      if(!std::isfinite(cmd.seconds) || cmd.seconds<0 || cmd.seconds>WAIT_MAX_S)
        return reject(out,cap,"seconds must be between 0 and 86400");
      a.kind  = Kind::Wait;
      a.until = a.start+uint64_t(double(cmd.seconds)*1000.0);
      break;
    case OG_CMD_STEP:
      a.key = stepKey(cmd.direction);
      if(a.key==KeyCodec::Idle)
        return reject(out,cap,"unknown step direction "+std::to_string(cmd.direction));
      a.kind  = Kind::Step;
      a.until = a.start+cmd.millis;
      break;
    case OG_CMD_TURN:
      if(!std::isfinite(cmd.degrees))
        return reject(out,cap,"degrees must be finite");
      a.kind    = Kind::Turn;
      a.heading = pl.rotation()-cmd.degrees;
      break;
    case OG_CMD_JUMP:
      a.kind = Kind::Jump;
      break;
    case OG_CMD_ATTACK: {
      og_target t = cmd.target;
      t.kind = OG_TARGET_NPC;
      if(!resolveTarget(w,pl,t,a,why))
        return reject(out,cap,why);
      if(ogIds().findNpc(w,a.id)->isDown())
        return reject(out,cap,"the NPC is already down");
      a.kind  = Kind::Attack;
      a.until = a.start+DRAW_TIMEOUT;
      a.phase = dist(pl,ogIds().findNpc(w,a.id)->position())>ATTACK_NEAR ? -1 : 0;
      break;
      }
    case OG_CMD_FLEE: {
      Npc* from = nearestAttacker(w,pl);
      if(from==nullptr)
        return reject(out,cap,"nobody is fighting the hero; nothing to flee from");
      a.kind   = Kind::Flee;
      a.target = OG_TARGET_NPC;
      a.id     = ogIds().npc(*from);
      a.until  = a.start+FLEE_TIMEOUT;
      break;
      }
    case OG_CMD_TRADE: {
      og_target t = cmd.target;
      t.kind = OG_TARGET_NPC;
      if(!resolveTarget(w,pl,t,a,why))
        return reject(out,cap,why);
      Npc* n = ogIds().findNpc(w,a.id);
      const std::string who(n->displayName());
      if(cmd.trade_op!=OG_TRADE_OPEN && cmd.trade_op!=OG_TRADE_BUY && cmd.trade_op!=OG_TRADE_SELL)
        return reject(out,cap,"unknown trade operation "+std::to_string(cmd.trade_op));
      a.tradeOp = cmd.trade_op;
      if(a.tradeOp!=OG_TRADE_OPEN) {
        if(cmd.amount==0)
          return reject(out,cap,"amount must be at least 1");
        if(cmd.instance.len>0 && cmd.instance.ptr==nullptr)
          return reject(out,cap,"bad item instance name");
        a.ware.assign(cmd.instance.ptr==nullptr ? "" : cmd.instance.ptr,cmd.instance.len);
        for(auto& c:a.ware)
          c = char(std::toupper(static_cast<unsigned char>(c)));
        if(a.ware.empty())
          return reject(out,cap,"no item instance name given");
        a.amount = cmd.amount;
        }
      a.kind = Kind::Trade;
      if(Npc* tr = trader(); tr!=nullptr) {
        if(tr!=n)
          return reject(out,cap,"trading with "+std::string(tr->displayName())+", not "+who);
        if(a.tradeOp!=OG_TRADE_OPEN) {
          const std::string no = deal(w,pl,*n,a);
          if(!no.empty())
            return reject(out,cap,no);
          }
        // Only wait can be running in a dialog; the deal doesn't affect it.
        cancel(&pl);
        act      = a;
        act.kind = Kind::Done;
        return OG_CTL_OK;
        }
      const DialogMenu::OgView v = OgHooks::dialogMenu==nullptr ? DialogMenu::OgView{} : OgHooks::dialogMenu->ogView();
      if(v.partner!=n)
        return reject(out,cap,"not talking with "+who+"; talk_to them first, trading starts from their trade option");
      if(v.choices==nullptr)
        return reject(out,cap,"no dialog options are shown right now; wait for them");
      size_t opt = v.choices->size();
      for(size_t i=0; i<v.choices->size(); ++i)
        if((*v.choices)[i].isTrade && !(*v.choices)[i].title.empty()) {
          opt = i;
          break;
          }
      if(opt==v.choices->size())
        return reject(out,cap,who+" offers no trade option right now");
      cancel(&pl);
      if(!OgHooks::dialogMenu->ogChoose(opt))
        return reject(out,cap,"the dialog did not accept the trade option");
      act       = a;
      act.until = a.start+TRADE_OPEN_TIMEOUT;
      Tempest::Log::i("og_submit: chose trade option ",opt," of ",who);
      return OG_CTL_OK;
      }
    case OG_CMD_SLEEP: {
      if(cmd.hour<0 || cmd.hour>23)
        return reject(out,cap,"hour must be between 0 and 23");
      if(pl.weaponState()!=WeaponState::NoWeapon)
        return reject(out,cap,"can't lie down with a weapon drawn");
      Interactive* bed = nullptr;
      for(auto scheme:BED_SCHEMES) {
        Interactive* i = w.availableMob(pl,scheme);
        if(i!=nullptr && (bed==nullptr || pl.qDistTo(*i)<pl.qDistTo(*bed))) {
          bed   = i;
          a.mob = std::string(scheme);
          }
        }
      if(bed==nullptr)
        return reject(out,cap,"no free bed within "+meters(float(MOBSI_SEARCH_DISTANCE))+"; go next to one first");
      a.kind     = Kind::Sleep;
      a.usesAi   = true;
      a.hour     = cmd.hour;
      a.mobState = bed->stateCount();
      a.until    = a.start+SLEEP_LIE_TIMEOUT;
      Tempest::Log::i("og_submit: sleep until ",cmd.hour,":00 in a ",a.mob," bed ",
                      meters(std::sqrt(pl.qDistTo(*bed)))," away (state ",a.mobState,")");
      break;
      }
    case OG_CMD_USE_ITEM: {
      std::string name;
      Item* it = heldItem(w,pl,cmd.instance,a.cls,name,why);
      if(it==nullptr)
        return reject(out,cap,why);
      if(equippable(*it))
        return reject(out,cap,"'"+name+"' is equipment; use equip");
      // A document's script opens DocumentMenu, which only a key closes: no command could.
      if((uint32_t(it->handle().main_flag) & ITM_CAT_DOCS)!=0)
        return reject(out,cap,"'"+name+"' is a document; read it with READ");
      a.kind  = Kind::UseItem;
      a.count = pl.itemCount(a.cls);
      a.until = a.start+USE_START;
      break;
      }
    case OG_CMD_READ: {
      if(documentOpen())
        return reject(out,cap,"a document is already open; close it first");
      std::string name;
      const Item* it = (cmd.instance.len==0) ? worldMap(w,pl,a.cls,name,why)
                                       : heldItem(w,pl,cmd.instance,a.cls,name,why);
      if(it==nullptr)
        return reject(out,cap,why);
      if((uint32_t(it->handle().main_flag) & ITM_CAT_DOCS)==0)
        return reject(out,cap,"'"+name+"' is not a document (map, letter or book)");
      a.kind  = Kind::Read;
      a.until = a.start+READ_START;
      Tempest::Log::i("og_submit: read ",name);
      break;
      }
    case OG_CMD_EQUIP:
    case OG_CMD_UNEQUIP: {
      std::string name;
      size_t      cls = 0;
      Item* it = heldItem(w,pl,cmd.instance,cls,name,why);
      if(it==nullptr)
        return reject(out,cap,why);
      if(!equippable(*it))
        return reject(out,cap,"'"+name+"' can't be equipped; use use_item");
      const bool on = cmd.kind==OG_CMD_EQUIP;
      // A second ring of a stack goes on the other hand (Inventory::use).
      const bool ring = (uint32_t(it->handle().flags) & ITM_RING)!=0;
      const bool full = ring ? it->equipCount()>=it->count() : it->isEquipped();
      if(on && full)
        return reject(out,cap,"'"+name+"' is already equipped");
      if(!on && !it->isEquipped())
        return reject(out,cap,"'"+name+"' is not equipped");
      if(w.isCutsceneLock())
        return reject(out,cap,"a cutscene is running; wait");
      const uint8_t before = it->equipCount();
      if(on) {
        const std::string unmet = unmetCondition(pl,*it);
        if(!unmet.empty())
          return reject(out,cap,"can't equip '"+name+"': "+unmet);
        pl.useItem(cls);
        } else {
        pl.unequipItem(cls);
        }
      it = pl.getItem(cls);
      const bool changed = it!=nullptr && (on ? it->equipCount()>before : !it->isEquipped());
      if(!changed)
        return reject(out,cap,on ? "the game did not equip '"+name+"' (no free slot, or busy)"
                                 : "the game did not take off '"+name+"'");
      // Equipped while the previous action still runs: nothing it depends on changes,
      // and a refused equip must leave it running (og_submit's contract).
      cancel(&pl);
      act      = a;
      act.kind = Kind::Done;
      Tempest::Log::i("og_submit: ",on ? "equipped " : "unequipped ",name);
      return OG_CTL_OK;
      }
    default:
      return reject(out,cap,"unknown command kind "+std::to_string(cmd.kind));
    }

  if(a.kind==Kind::Step || a.kind==Kind::Turn || a.kind==Kind::Jump) {
    if(OgHooks::playerControl==nullptr)
      return reject(out,cap,"no player control (no main window)");
    // PlayerControl ignores and clears keys during a cutscene.
    if(w.isCutsceneLock())
      return reject(out,cap,"a cutscene is running; wait");
    }

  cancel(&pl);
  const KeyCodec::Action key = a.key;
  a.key = KeyCodec::Idle;
  act   = a;

  switch(act.kind) {
    case Kind::GoTo:
    case Kind::PickUp:
      if(!pushGoTo(w,pl,act,why)) {
        act = Action{};
        return reject(out,cap,why);
        }
      break;
    case Kind::TalkTo: {
      Tempest::Vec3 p;
      targetPos(w,act,p,why);
      if(dist(pl,p)>TALK_RANGE)
        pushGoTo(w,pl,act,why);
      break;
      }
    case Kind::Attack:
      if(act.phase==-1 && !pushGoTo(w,pl,act,why)) {
        act = Action{};
        return reject(out,cap,why);
        }
      break;
    case Kind::Step:
      if(act.until>act.start)
        press(key);
      break;
    case Kind::Sleep:
      pl.aiPush(AiQueue::aiUseMob(act.mob,act.mobState));
      break;
    default:
      break;
    }
  Tempest::Log::i("og_submit: started command ",cmd.kind);
  return OG_CTL_OK;
  }

int finish(Npc* pl) {
  releaseKey();
  endFight(pl);
  act = Action{};
  return OG_ACTION_FINISHED;
  }

int fail(char* out, size_t cap, Npc* pl, std::string_view why) {
  Tempest::Log::i("og_action_update: action failed: ",why);
  cancel(pl);
  write(out,cap,why);
  return OG_ACTION_FAILED;
  }

// One frame of a turn: hold RotateL/RotateR until the heading is reached or passed.
int tickTurn(World& w, Npc& pl, char* out, size_t cap) {
  if(act.turnTo) {
    Tempest::Vec3 p;
    std::string   why;
    if(!targetPos(w,act,p,why))
      return fail(out,cap,&pl,why);
    act.heading = headingTo(pl.position(),p);
    }
  const float rest = wrapDeg(act.heading-pl.rotation());
  const bool  left = rest>0;
  const bool  passed = (act.key==KeyCodec::RotateL && !left) || (act.key==KeyCodec::RotateR && left);
  if(std::fabs(rest)<=TURN_DONE || passed)
    return finish(&pl);
  if(w.tickCount()>act.start+TURN_TIMEOUT)
    return fail(out,cap,&pl,"could not turn (blocked?)");
  const KeyCodec::Action want = left ? KeyCodec::RotateL : KeyCodec::RotateR;
  if(act.key!=want) {
    releaseKey();
    press(want);
    }
  return OG_ACTION_RUNNING;
  }

int update(char* out, size_t cap) {
  if(act.kind==Kind::None)
    return OG_ACTION_NONE;
  Current s = current();
  if(act.generation!=OgHooks::worldGeneration || !s.ok())
    return fail(out,cap,nullptr,"the world changed (loading, new game or menu)");
  World& w  = *s.w;
  Npc&   pl = *s.pl;
  const uint64_t now = w.tickCount();

  if(act.kind==Kind::Done)
    return finish(&pl);
  // Nothing may act on the world while the simulation stands still (interact() would
  // run take/dialog scripts while paused).
  if(s.g.isPause() || (OgHooks::video!=nullptr && OgHooks::video->isActive()))
    return OG_ACTION_RUNNING;
  if(pl.isDead())
    return fail(out,cap,&pl,"the player died");

  switch(act.kind) {
    case Kind::Wait:
      return now>=act.until ? finish(&pl) : OG_ACTION_RUNNING;

    case Kind::Step:
      return now>=act.until ? finish(&pl) : OG_ACTION_RUNNING;

    case Kind::Turn:
      return tickTurn(w,pl,out,cap);

    case Kind::Jump: {
      const bool air = pl.isJumpAnim() || pl.isJump() || pl.isJumpUp() || pl.isInAir() ||
                       pl.isFalling() || pl.isSlide();
      if(act.phase==0) {
        press(KeyCodec::Jump);
        act.phase = 1;
        return OG_ACTION_RUNNING;
        }
      if(act.phase==1) {
        // Hold the key like a player would until the jump starts: frames can outpace game
        // ticks, and right after moving the player is not "standing" for a few ticks.
        if(!air) {
          if(now>act.start+JUMP_START)
            return fail(out,cap,&pl,"the jump did not start (blocked?)");
          return OG_ACTION_RUNNING;
          }
        releaseKey();
        act.phase = 2;
        }
      if(!air)
        return finish(&pl);
      if(now>act.start+JUMP_TIMEOUT)
        return fail(out,cap,&pl,"still in the air after the jump");
      return OG_ACTION_RUNNING;
      }

    case Kind::GoTo: {
      if(act.target==OG_TARGET_ITEM && !pl.isAiQueueEmpty()) {
        if(Item* it = ogIds().findItem(w,act.id); it!=nullptr && inReach(pl,*it))
          pl.clearAiQueue();
        }
      if(!pl.isAiQueueEmpty())
        return OG_ACTION_RUNNING;
      Tempest::Vec3 p;
      std::string   why;
      if(!targetPos(w,act,p,why))
        return fail(out,cap,&pl,why);
      const float d = dist(pl,p);
      if(d>act.arrive) {
        if(act.target==OG_TARGET_POSITION)
          return fail(out,cap,&pl,"reached the nearest waypoint '"+act.wp->name+"' but stopped "+meters(d)+
                                  " from the position; use step for the rest");
        return fail(out,cap,&pl,"stopped "+meters(d)+" from the target (no path, or blocked)");
        }
      return finish(&pl);
      }

    case Kind::PickUp: {
      Item* it = ogIds().findItem(w,act.id);
      if(it==nullptr)
        return fail(out,cap,&pl,"the item is no longer lying in the world");
      if(act.phase==0) {
        if(!pl.isAiQueueEmpty() && inReach(pl,*it))
          pl.clearAiQueue();
        if(!pl.isAiQueueEmpty())
          return OG_ACTION_RUNNING;
        if(!inReach(pl,*it))
          return fail(out,cap,&pl,"stopped "+meters(dist(pl,it->position()))+" from the item (no path, or blocked)");
        act.usesAi = false;
        act.phase  = 1;
        act.until  = now+TAKE_TIMEOUT;
        }
      // Right after walking the body state may not be "standing" yet: retry briefly.
      if(OgHooks::playerControl!=nullptr && OgHooks::playerControl->interact(*it))
        return finish(&pl);
      if(now>act.until)
        return fail(out,cap,&pl,"could not pick up the item (busy, or weapon drawn)");
      return OG_ACTION_RUNNING;
      }

    case Kind::TalkTo: {
      Npc* n = ogIds().findNpc(w,act.id);
      if(n==nullptr)
        return fail(out,cap,&pl,"the NPC is no longer in the world");
      if(act.phase==0) {
        if(!pl.isAiQueueEmpty())
          return OG_ACTION_RUNNING;
        const float d = dist(pl,n->position());
        if(d>ARRIVE_NPC)
          return fail(out,cap,&pl,"could not get close to the NPC ("+meters(d)+" away)");
        act.usesAi = false;
        act.phase  = 2;
        act.until  = now+TAKE_TIMEOUT;
        }
      if(act.phase==2) {
        // Right after walking or a step the body state may not be "standing" yet
        // (PlayerControl::interact refuses then): retry briefly, as pick_up does.
        if(OgHooks::playerControl!=nullptr && OgHooks::playerControl->interact(*n)) {
          act.phase = 1;
          act.until = now+TALK_TIMEOUT;
          return OG_ACTION_RUNNING;
          }
        if(now>act.until)
          return fail(out,cap,&pl,"can't talk right now (busy, or weapon drawn)");
        return OG_ACTION_RUNNING;
        }
      if(OgHooks::dialogMenu!=nullptr && OgHooks::dialogMenu->ogView().partner==n)
        return finish(&pl);
      if(now>act.until)
        return fail(out,cap,&pl,"the NPC did not start a conversation");
      return OG_ACTION_RUNNING;
      }

    case Kind::Trade: {
      Npc* n = ogIds().findNpc(w,act.id);
      if(n==nullptr)
        return fail(out,cap,&pl,"the trader is no longer in the world");
      Npc* tr = trader();
      if(tr==nullptr) {
        if(!inDialog())
          return fail(out,cap,&pl,"the dialog ended before the trade screen opened");
        if(now>act.until)
          return fail(out,cap,&pl,"the trade screen did not open");
        return OG_ACTION_RUNNING;
        }
      if(tr!=n)
        return fail(out,cap,&pl,"the trade screen opened with someone else");
      if(act.tradeOp!=OG_TRADE_OPEN) {
        const std::string no = deal(w,pl,*n,act);
        if(!no.empty())
          return fail(out,cap,&pl,no);
        }
      return finish(&pl);
      }

    case Kind::Sleep: {
      // 0: walking to the bed and lying down until its sleep dialog shows its options.
      if(act.phase==0) {
        const DialogMenu::OgView v = OgHooks::dialogMenu==nullptr ? DialogMenu::OgView{} : OgHooks::dialogMenu->ogView();
        if(v.partner!=nullptr && v.partner!=&pl)
          return fail(out,cap,&pl,std::string(v.partner->displayName())+" started talking before the hero lay down");
        if(v.partner==nullptr || v.choices==nullptr) {
          if(pl.isAiQueueEmpty() && pl.interactive()==nullptr)
            return fail(out,cap,&pl,"could not lie down in the bed (taken or out of reach)");
          if(now>act.until)
            return fail(out,cap,&pl,"the bed offered no sleep (no sleep dialog)");
          return OG_ACTION_RUNNING;
          }
        // The bed's options each call PC_SLEEP with a fixed hour; any hour is the same
        // call. Run outside the menu it leaves the menu open with its options (seen), so
        // the menu is then ended by its own END option (PC_NOSLEEP_INFO, the stock click
        // path), which makes it recheck its options and close.
        const gtime before = w.time();
        act.phase = 1;
        act.until = now+SLEEP_GETUP_TIMEOUT;
        bool slept = false;
        try {
          slept = w.script().ogCallInt(pl,"PC_SLEEP",act.hour);
          }
        catch(const std::exception& e) {
          return fail(out,cap,&pl,std::string("PC_SLEEP failed: ")+e.what());
          }
        if(!slept)
          return fail(out,cap,&pl,"the scripts have no PC_SLEEP(int)");
        Tempest::Log::i("og_action_update: slept from ",before.hour(),":",before.minute(),
                        " (day ",before.day(),") until ",w.time().hour(),":",w.time().minute(),
                        " (day ",w.time().day(),")");
        }
      // 1: picking END once the menu takes it; 2: waiting for the dialog to close;
      // 3: getting up.
      if(now>act.until)
        return fail(out,cap,&pl,act.phase<=2 ? "the bed's dialog did not end" : "the hero did not get up");
      if(act.phase==1) {
        if(!endBedDialog(w,pl))
          return OG_ACTION_RUNNING;
        act.phase = 2;
        return OG_ACTION_RUNNING;
        }
      if(act.phase==2) {
        if(inDialog())
          return OG_ACTION_RUNNING;
        if(pl.interactive()!=nullptr && pl.isAiQueueEmpty()) {
          pl.aiPush(AiQueue::aiUseMob(act.mob,-1));
          Tempest::Log::i("og_action_update: the bed's dialog ended; getting up");
          }
        act.phase = 3;
        return OG_ACTION_RUNNING;
        }
      if(pl.interactive()!=nullptr || !pl.isAiQueueEmpty())
        return OG_ACTION_RUNNING;
      return finish(&pl);
      }

    case Kind::UseItem: {
      // Use once, when standing: right after walking the body state may not be "standing"
      // yet. Items without a use animation run their script at once (Inventory::use).
      if(act.phase==0) {
        if(pl.bodyStateMasked()!=BS_STAND) {
          if(now>act.until)
            return fail(out,cap,&pl,"could not use the item (weapon drawn, swimming, or busy)");
          return OG_ACTION_RUNNING;
          }
        Item* it = pl.getItem(act.cls);
        if(it==nullptr)
          return fail(out,cap,&pl,"the item is no longer in the inventory");
        // A torch toggles inside Inventory::use; putting one out plays no animation.
        const bool instant = it->handle().scheme_name.empty() ||
                             (uint32_t(it->handle().flags) & ITM_TORCH)!=0;
        if(!pl.useItem(act.cls,Item::NSLOT,false))
          return fail(out,cap,&pl,"the game refused to use the item (weapon drawn, or busy)");
        if(instant)
          return finish(&pl);
        act.phase = 1;
        act.until = now+USE_START;
        }
      const bool using_ = pl.bodyStateMasked()==BS_ITEMINTERACT;
      if(using_ && !act.seenUse) {
        act.seenUse = true;
        act.until   = now+USE_TIMEOUT;
        }
      if(using_) {
        if(now>act.until)
          return fail(out,cap,&pl,"still using the item after 15 s");
        return OG_ACTION_RUNNING;
        }
      if(act.seenUse || pl.itemCount(act.cls)!=act.count)
        return finish(&pl);
      if(now>act.until)
        return fail(out,cap,&pl,"the game did not use the item (busy, or it can't be used now)");
      return OG_ACTION_RUNNING;
      }

    case Kind::Read: {
      // As use_item: use once standing; the item's script opens the document window.
      if(act.phase==0) {
        if(pl.bodyStateMasked()!=BS_STAND) {
          if(now>act.until)
            return fail(out,cap,&pl,"could not read (weapon drawn, swimming, or busy; body state "+
                        std::to_string(pl.bodyStateMasked())+")");
          return OG_ACTION_RUNNING;
          }
        if(pl.getItem(act.cls)==nullptr)
          return fail(out,cap,&pl,"the document is no longer in the inventory");
        if(!pl.useItem(act.cls,Item::NSLOT,false))
          return fail(out,cap,&pl,"the game refused to use the document (weapon drawn, or busy)");
        act.phase = 1;
        act.until = now+USE_TIMEOUT;
        }
      if(documentOpen()) {
        // finish() leaves the window open for the host's capture; only cancel() closes it.
        if(++act.docFrames>=READ_FRAMES)
          return finish(&pl);
        return OG_ACTION_RUNNING;
        }
      if(act.docFrames>0)
        return fail(out,cap,&pl,"the document window closed again");
      if(now>act.until)
        return fail(out,cap,&pl,"the document opened no window");
      return OG_ACTION_RUNNING;
      }

    case Kind::Attack: {
      Npc* n = ogIds().findNpc(w,act.id);
      if(n==nullptr)
        return fail(out,cap,&pl,"the NPC is no longer in the world");
      if(n->isDown() && act.phase<2) {
        if(act.phase<1)
          return finish(&pl);
        // The fight is over: hand the player back, then put the weapon away.
        endFight(&pl);
        pl.clearAiQueue();
        act.usesAi = false;
        act.phase  = 2;
        act.until  = now+DRAW_TIMEOUT;
        return OG_ACTION_RUNNING;
        }
      if(pl.isDown())
        return fail(out,cap,&pl,"the hero was knocked down");
      if(act.phase==-1) {
        const float d = dist(pl,n->position());
        if(d<=ATTACK_NEAR && !pl.isAiQueueEmpty())
          pl.clearAiQueue();
        if(!pl.isAiQueueEmpty())
          return OG_ACTION_RUNNING;
        if(d>ATTACK_NEAR*2)
          return fail(out,cap,&pl,"could not get close to the NPC ("+meters(d)+" away)");
        act.usesAi = false;
        act.phase  = 0;
        act.until  = now+DRAW_TIMEOUT;
        }
      if(act.phase==0) {
        const auto ws = pl.weaponState();
        if(ws!=WeaponState::Fist && ws!=WeaponState::W1H && ws!=WeaponState::W2H) {
          if(now>act.until)
            return fail(out,cap,&pl,"could not draw a weapon (busy)");
          if(pl.currentMeleeWeapon()!=nullptr)
            pl.drawWeaponMelee(); else
            pl.drawWeaponFist();
          return OG_ACTION_RUNNING;
          }
        std::string why;
        if(!startFight(w,true,why))
          return fail(out,cap,&pl,why);
        act.usesAi = true;
        act.phase  = 1;
        }
      if(act.phase==2) {
        if(pl.weaponState()==WeaponState::NoWeapon)
          return finish(&pl);
        if(now>act.until) {
          Tempest::Log::i("og_control: attack: the NPC is down, but the weapon is still out after 3 s");
          return finish(&pl);
          }
        pl.closeWeapon(false);
        return OG_ACTION_RUNNING;
        }
      if(now>=act.logged+2000) {
        act.logged = now;
        Tempest::Log::i("og_control: attack: target ",meters(dist(pl,n->position()))," away, its hp ",
                        n->attribute(ATR_HITPOINTS),", hero body state ",int(pl.bodyStateMasked()),
                        ", fight moves ",pl.ogHasFightMoves() ? "yes" : "no",
                        ", queue ",pl.isAiQueueEmpty() ? "empty" : "busy");
        }
      // PlayerControl::tickFocus retargets the player to whatever is in focus.
      pl.setTarget(n);
      if(pl.isAiQueueEmpty()) {
        act.queued = 0;
        if(!pl.ogHasFightMoves()) {
          pl.aiPush(AiQueue::aiAttack());
          act.queued = now;
          }
        }
      else if(act.queued!=0 && now>act.queued+FIGHT_STUCK) {
        return fail(out,cap,&pl,"the fight AI found no move for the hero");
        }
      return OG_ACTION_RUNNING;
      }

    case Kind::Flee: {
      Npc* n = ogIds().findNpc(w,act.id);
      if(n==nullptr || n->isDown())
        return finish(&pl);
      const float d = dist(pl,n->position());
      if(d>=FLEE_SAFE)
        return finish(&pl);
      if(pl.isDown())
        return fail(out,cap,&pl,"the hero was knocked down");
      if(now>act.until)
        return fail(out,cap,&pl,"still "+meters(d)+" from the attacker after 30 s (cornered?)");
      if(act.phase==0) {
        std::string why;
        if(!startFight(w,false,why))
          return fail(out,cap,&pl,why);
        act.usesAi = true;
        act.phase  = 1;
        }
      pl.setTarget(n);
      // Each AI_Flee picks the waypoint farthest from the attacker and turns there, then
      // the run (GT_Flee) goes on; repeated, the heading follows an attacker who gives
      // chase. Not "queue empty": the run itself keeps isAiQueueEmpty() false.
      if(act.queued==0 || now>=act.queued+1000) {
        pl.aiPush(AiQueue::aiFlee());
        act.queued = now;
        }
      return OG_ACTION_RUNNING;
      }

    case Kind::None:
    case Kind::Done:
      break;
    }
  return OG_ACTION_NONE;
  }

template<class F>
int guarded(const char* name, og_ctx* ctx, F&& f) {
  try {
    if(ctx==nullptr || !ctx->inFrame) {
      Tempest::Log::e(name,": called outside on_frame");
      return OG_CTL_BAD_ARGS;
      }
    return f();
    }
  catch(const std::exception& e) {
    Tempest::Log::e(name,": exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e(name,": unknown exception");
    }
  return OG_CTL_EXCEPTION;
  }
}

int og_set_paused(og_ctx* ctx, int paused) {
  return guarded("og_set_paused",ctx,[&]{
    if(paused!=0 && paused!=1) {
      Tempest::Log::e("og_set_paused: paused must be 0 or 1, got ",paused);
      return OG_CTL_BAD_ARGS;
      }
    if(bool(paused)!=hostPause) {
      hostPause = bool(paused);
      if(hostPause)
        Gothic::inst().pushPause(); else
        Gothic::inst().popPause();
      Tempest::Log::i("og_set_paused: ",hostPause ? "paused" : "resumed"," by host");
      }
    return OG_CTL_OK;
    });
  }

int og_skip_videos(og_ctx* ctx) {
  return guarded("og_skip_videos",ctx,[&]{
    if(OgHooks::video==nullptr || !OgHooks::video->isActive())
      return 0;
    Tempest::Log::i("og_skip_videos: skipping video");
    OgHooks::video->ogStopAll();
    return 1;
    });
  }

int og_dev_console(og_ctx* ctx, og_str command) {
  return guarded("og_dev_console",ctx,[&]{
    if(command.len>0 && command.ptr==nullptr) {
      Tempest::Log::e("og_dev_console: NULL command");
      return OG_CTL_BAD_ARGS;
      }
    if(!current().ok())
      return OG_CTL_NOT_IN_GAME;
    const std::string line(command.ptr==nullptr ? "" : command.ptr,command.len);
    // Marvin::exec takes an empty or blank line as success.
    if(line.find_first_not_of(' ')==std::string::npos) {
      Tempest::Log::i("og_dev_console: empty command refused");
      return OG_CTL_REJECTED;
      }
    static Marvin marvin;
    const bool ok = marvin.exec(line);
    Tempest::Log::i("og_dev_console: '",line,ok ? "' ran" : "' refused");
    return ok ? OG_CTL_OK : OG_CTL_REJECTED;
    });
  }

// After every call that may start or end an action (UB-30).
static int syncWalkHook(int ret) {
  OgHooks::hostWalksPlayer = act.kind!=Kind::None && act.usesAi && act.generation==OgHooks::worldGeneration;
  return ret;
  }

int og_submit(og_ctx* ctx, const og_command* cmd, char* reason, size_t cap) {
  return syncWalkHook(guarded("og_submit",ctx,[&]{
    if(cmd==nullptr || (reason==nullptr && cap>0)) {
      Tempest::Log::e("og_submit: NULL command or reason buffer");
      return OG_CTL_BAD_ARGS;
      }
    return submit(*cmd,reason,cap);
    }));
  }

int og_cancel_action(og_ctx* ctx) {
  return syncWalkHook(guarded("og_cancel_action",ctx,[&]{
    if(act.kind!=Kind::None)
      Tempest::Log::i("og_cancel_action: cancelled by host");
    cancel(Gothic::inst().player());
    return OG_CTL_OK;
    }));
  }

int og_action_update(og_ctx* ctx, char* reason, size_t cap) {
  return syncWalkHook(guarded("og_action_update",ctx,[&]{
    if(reason==nullptr && cap>0) {
      Tempest::Log::e("og_action_update: NULL reason buffer");
      return OG_CTL_BAD_ARGS;
      }
    return update(reason,cap);
    }));
  }
