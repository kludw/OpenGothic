#pragma once

#include <cstdint>
#include <functional>
#include <zenkit/addon/daedalus.hh>
#include <Tempest/Vec>

#include "../../game/capi/og_sim.h"
#include "game/definitions/fightaidefinitions.h"

class Npc;
class GameScript;
class Serialize;

class FightAlgo final {
  public:
    FightAlgo();

    void load(Serialize& fin);
    void save(Serialize& fout);

    enum Action : uint8_t {
      MV_NULL     = 0,
      MV_MOVEG    = 1,
      MV_MOVE     = 2,
      MV_JUMPBACK = 3,
      MV_ATTACK   = 4,
      MV_ATTACKL  = 5,
      MV_ATTACKR  = 6,
      MV_STRAFEL  = 7,
      MV_STRAFER  = 8,
      MV_BLOCK    = 9,
      MV_WAIT     = 10,
      MV_WAITLONG = 11,
      MV_TURN2HIT = 12,
      MV_TURN     = 13,
      MV_TURNG    = 14,
      MV_STRAFE_E = 15,

      MV_MAX      = 6
      };

    Action nextFromQueue(Npc &npc, Npc &tg, GameScript& owner);
    void   consumeAction();
    void   onClearTarget();
    void   onTakeHit();

    bool   hasInstructions() const;
    bool   fetchInstructions(Npc &npc, Npc &tg, GameScript& owner);
    float  qDistTo(const Npc& npc, const Npc& tg) const;

    float  baseDistance           (const Npc &npc, const Npc &tg, GameScript &owner) const;
    float  prefferedAttackDistance(const Npc &npc, const Npc &tg, GameScript &owner) const;
    float  prefferedGDistance     (const Npc &npc, const Npc &tg, GameScript &owner) const;
    float  attackFinishDistance   (GameScript &owner) const;

    bool   isInAttackRange        (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInFinishRange        (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInCloseupRange       (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInWRange             (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInGRange             (const Npc &npc, const Npc &tg, GameScript &owner) const;
    bool   isInFocusAngle         (const Npc &npc, const Npc &tg) const;
    bool   isInFocusAngle         (const Npc &npc, const Npc &tg, float ang) const;
    bool   isInJumpBackAngle      (const Npc &npc, const Npc &tg) const;

    // opengothic-rs (task 5.16): what fillQueue reads, as og_sim.h holds it, and the
    // stock logic on it, so og-sim can be compared with it (og_sim_test_fightai,
    // OgSimFight). Rand answers GameScript::rand(max).
    using Rand = std::function<uint32_t(uint32_t)>;
    static og_sim_fight_side pose(const Npc& npc);  // center and rotation only
    static og_sim_fight_side side(const Npc& npc, const Npc& other, GameScript& owner);
    static og_sim_fight_in   read(Npc& npc, const Npc& tg, GameScript& owner);
    og_sim_fight_state state() const;
    void               setState(const og_sim_fight_state& s);
    bool   fetchOn(const og_sim_fight_in& in, const FightAi::FA& ai, const Rand& rand);
    Action nextOn(const Rand& rand);
    void   consumeOn();
    void   clearTargetOn();
    void   takeHitOn();

    static float  weaponRange(uint32_t game, const og_sim_fight_side& npc);
    static float  baseDistance(const og_sim_fight_side& npc, const og_sim_fight_side& tg);
    static float  prefferedAttackDistance(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg);
    static float  prefferedGDistance(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg);
    static bool   isInWRange(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg);
    static bool   isInGRange(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg);
    static bool   angleTest(const og_sim_fight_side& npc, const og_sim_fight_side& tg, float cosMax);

  private:
    void   fillQueue(const og_sim_fight_in& in, const FightAi::FA& ai, const Rand& rand);
    bool   fillQueue(const zenkit::IFightAi& src, const Rand& rand);

    zenkit::FightAiMove queueId = zenkit::FightAiMove::NOP;
    Action              tr   [MV_MAX]={};
    bool                hitFlg=false;
  };
