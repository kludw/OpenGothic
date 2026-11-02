// opengothic-rs: og_set_paused / og_submit / og_action_update (game/capi/og_capi.h) —
// drives the player like an AI NPC (AI queue, waynet pathfinding) or through the same
// key actions a human uses (motor commands).

#include "og_capi.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "og_ids.h"
#include "game/playercontrol.h"
#include "ui/dialogmenu.h"
#include "ui/videowidget.h"
#include "utils/keycodec.h"
#include "world/aiqueue.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "gothic.h"

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
constexpr float    TURN_DONE    = 2.f;  // degrees
// Upper bound for wait: keeps the seconds->ms conversion defined (og_capi.h).
constexpr float    WAIT_MAX_S   = 86400.f;

enum class Kind : uint8_t { None, GoTo, Turn, TalkTo, Wait, PickUp, Step, Jump, Done };

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

// Ends the current action without reporting it. pl: the player of the action's world,
// or nullptr if that world is gone.
void cancel(Npc* pl) {
  releaseKey();
  if(act.usesAi && pl!=nullptr && act.generation==OgHooks::worldGeneration)
    pl->clearAiQueue();
  act = Action{};
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

  if(pl.isDead())
    return reject(out,cap,"the player is dead");

  const bool dialog = inDialog();
  if(dialog && cmd.kind!=OG_CMD_CHOOSE_DIALOG && cmd.kind!=OG_CMD_WAIT)
    return reject(out,cap,"in a dialog: only choose_dialog and wait are possible");
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
    case Kind::Step:
      if(act.until>act.start)
        press(key);
      break;
    default:
      break;
    }
  Tempest::Log::i("og_submit: started command ",cmd.kind);
  return OG_CTL_OK;
  }

int finish() {
  releaseKey();
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
    return finish();
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
    return finish();
  // Nothing may act on the world while the simulation stands still (interact() would
  // run take/dialog scripts while paused).
  if(s.g.isPause() || (OgHooks::video!=nullptr && OgHooks::video->isActive()))
    return OG_ACTION_RUNNING;
  if(pl.isDead())
    return fail(out,cap,&pl,"the player died");

  switch(act.kind) {
    case Kind::Wait:
      return now>=act.until ? finish() : OG_ACTION_RUNNING;

    case Kind::Step:
      return now>=act.until ? finish() : OG_ACTION_RUNNING;

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
        return finish();
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
      return finish();
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
        return finish();
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
        if(OgHooks::playerControl==nullptr || !OgHooks::playerControl->interact(*n))
          return fail(out,cap,&pl,"can't talk right now (busy, or weapon drawn)");
        act.phase = 1;
        act.until = now+TALK_TIMEOUT;
        return OG_ACTION_RUNNING;
        }
      if(OgHooks::dialogMenu!=nullptr && OgHooks::dialogMenu->ogView().partner==n)
        return finish();
      if(now>act.until)
        return fail(out,cap,&pl,"the NPC did not start a conversation");
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

int og_submit(og_ctx* ctx, const og_command* cmd, char* reason, size_t cap) {
  return guarded("og_submit",ctx,[&]{
    if(cmd==nullptr || (reason==nullptr && cap>0)) {
      Tempest::Log::e("og_submit: NULL command or reason buffer");
      return OG_CTL_BAD_ARGS;
      }
    return submit(*cmd,reason,cap);
    });
  }

int og_cancel_action(og_ctx* ctx) {
  return guarded("og_cancel_action",ctx,[&]{
    if(act.kind!=Kind::None)
      Tempest::Log::i("og_cancel_action: cancelled by host");
    cancel(Gothic::inst().player());
    return OG_CTL_OK;
    });
  }

int og_action_update(og_ctx* ctx, char* reason, size_t cap) {
  return guarded("og_action_update",ctx,[&]{
    if(reason==nullptr && cap>0) {
      Tempest::Log::e("og_action_update: NULL reason buffer");
      return OG_CTL_BAD_ARGS;
      }
    return update(reason,cap);
    });
  }
