#include "fightalgo.h"

#include <Tempest/Log>

#include "game/definitions/fightaidefinitions.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "gothic.h"
#include "serialize.h"
#include "capi/oghooks.h"
#include "capi/ogsimfight.h"

// According to Gothic1 scripts:
// W  - Weapon Range (FIGHT_RANGE_FIST * 3)
// G  - Walking range (3 * W). Buffer for ranged fighters in which they should switch to a melee weapon.
// FK - Long-range combat range (30m)

FightAlgo::FightAlgo() {
  }

void FightAlgo::load(Serialize &fin) {
  fin.read(reinterpret_cast<int32_t&>(queueId));
  for(int i=0;i<MV_MAX;++i)
    fin.read(reinterpret_cast<uint8_t&>(tr[i]));
  fin.read(hitFlg);
  }

void FightAlgo::save(Serialize &fout) {
  fout.write(int32_t(queueId));
  for(int i=0;i<MV_MAX;++i)
    fout.write(uint8_t(tr[i]));
  fout.write(hitFlg);
  }

og_sim_fight_side FightAlgo::pose(const Npc& npc) {
  auto c = npc.collosionCenter();
  og_sim_fight_side s = {};
  s.center[0] = c.x;
  s.center[1] = c.y;
  s.center[2] = c.z;
  s.rotation  = npc.rotationRad();
  return s;
  }

og_sim_fight_side FightAlgo::side(const Npc& npc, const Npc& other, GameScript& owner) {
  auto&  gv = owner.guildVal();
  auto   gl = npc.guild();
  auto   w  = npc.inventory().activeWeapon();
  og_sim_fight_side s = pose(npc);
  s.range_base   = gv.fight_range_base[gl];
  s.range_g      = gv.fight_range_g[gl];
  s.range_fist   = gv.fight_range_fist[gl];
  s.range_1ha    = gv.fight_range_1ha[gl];
  s.range_2ha    = gv.fight_range_2ha[gl];
  s.weapon_state = uint32_t(npc.weaponState());
  s.sword_length = w ? w->swordLength() : 0;
  s.run          = npc.bodyStateMasked()==BS_RUN ? 1 : 0;
  s.prehit       = npc.isPrehit() ? 1 : 0;
  s.dist2        = npc.fightDistanceTo(other).quadLength();
  return s;
  }

og_sim_fight_in FightAlgo::read(Npc& npc, const Npc& tg, GameScript& owner) {
  og_sim_fight_in in = {};
  in.game   = uint32_t(Gothic::inst().version().game);
  // opengothic-rs: the seam's attack lends the hero a tactic (heroes have none) without
  // touching the script instance, which saves would keep.
  in.tactic = npc.handle().fight_tactic;
  if(in.tactic==0 && npc.isPlayer() && OgHooks::hostFightsForPlayer)
    in.tactic = OgHooks::playerFightTactic;
  in.npc    = side(npc, tg, owner);
  in.tg     = side(tg, npc, owner);
  return in;
  }

og_sim_fight_state FightAlgo::state() const {
  og_sim_fight_state s = {};
  s.queue_id = int32_t(queueId);
  for(int i=0;i<MV_MAX;++i)
    s.tr[i] = uint8_t(tr[i]);
  s.hit_flg = hitFlg ? 1 : 0;
  return s;
  }

void FightAlgo::setState(const og_sim_fight_state& s) {
  queueId = zenkit::FightAiMove(s.queue_id);
  for(int i=0;i<MV_MAX;++i)
    tr[i] = Action(s.tr[i]);
  hitFlg = s.hit_flg!=0;
  }

void FightAlgo::fillQueue(const og_sim_fight_in& in, const FightAi::FA& ai, const Rand& rand) {
  auto  ws = WeaponState(in.npc.weapon_state);
  auto& npc = in.npc;
  auto& tg  = in.tg;
  if(hitFlg) {
    hitFlg = false;
    if(fillQueue(ai.my_w_strafe,rand))
      return;
    }

  static const float focusAngle = std::cos(float(30.0*M_PI/180.0));
  const bool focus = angleTest(npc,tg,focusAngle);

  if(tg.prehit && isInWRange(in.game,tg,npc) && angleTest(tg,npc,focusAngle) && focus){
    if(tg.run)
      if(fillQueue(ai.enemy_stormprehit,rand))
        return;
    if(fillQueue(ai.enemy_prehit,rand))
      return;
    }

  if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
    if(isInWRange(in.game,npc,tg)) {
      if(focus && npc.run)
        if(fillQueue(ai.my_w_runto,rand))
          return;
      if(focus && !npc.run)
        if(fillQueue(ai.my_w_focus,rand))
          return;
      if(fillQueue(ai.my_w_nofocus,rand))
        return;
      }

    if(isInGRange(in.game,npc,tg)) {
      if(focus && npc.run)
        if(fillQueue(ai.my_g_runto,rand))
          return;
      if(focus && !npc.run)
        if(fillQueue(ai.my_g_focus,rand))
          return;
      }
    }

  if(ws==WeaponState::Mage) {
    if(isInWRange(in.game,npc,tg))
      if(fillQueue(ai.my_fk_focus_mag,rand))
        return;
    if(fillQueue(ai.my_fk_nofocus_mag,rand))
      return;
    }

  if(isInWRange(in.game,npc,tg) && focus)
    if(fillQueue(ai.my_fk_focus_far,rand))
      return;
  if(fillQueue(ai.my_fk_nofocus_far,rand))
    return;

  fillQueue(ai.my_w_nofocus,rand);
  }

bool FightAlgo::fillQueue(const zenkit::IFightAi& src, const Rand& rand) {
  uint32_t sz=0;
  for(size_t i=0; i<zenkit::IFightAi::move_count; ++i){
    if(src.move[i]==zenkit::FightAiMove::NOP)
      break;
    sz++;
    }
  if(sz==0)
    return false;
  queueId = zenkit::FightAiMove(src.move[rand(sz)]);
  return queueId!=zenkit::FightAiMove::NOP;
  }

FightAlgo::Action FightAlgo::nextFromQueue(Npc&, Npc&, GameScript& owner) {
  return Action(OgSimFight::run(*this, OG_SIM_FIGHT_NEXT, nullptr, &owner));
  }

FightAlgo::Action FightAlgo::nextOn(const Rand& rand) {
  using zenkit::FightAiMove;

  if(tr[0]==MV_NULL) {
    switch(queueId) {
      case FightAiMove::TURN:
        tr[0] = MV_TURN;
        break;
      case FightAiMove::RUN:{
        tr[0] = MV_MOVE;
        break;
        }
      case FightAiMove::RUN_BACK:{
        tr[0] = MV_NULL; //TODO
        break;
        }
      case FightAiMove::JUMP_BACK:{
        tr[0] = MV_JUMPBACK;
        break;
        }
      case FightAiMove::STRAFE:{
        tr[0] = rand(2) ? MV_STRAFEL : MV_STRAFER;
        tr[1] = FightAlgo::MV_STRAFE_E;
        break;
        }
      case FightAiMove::ATTACK:{
        tr[0] = MV_ATTACK;
        break;
        }
      case FightAiMove::ATTACK_SIDE:{
        tr[0] = MV_ATTACKL;
        tr[1] = MV_ATTACKR;
        break;
        }
      case FightAiMove::ATTACK_FRONT:{
        tr[0] = rand(2) ? MV_ATTACKL : MV_ATTACKR;
        tr[1] = MV_ATTACK;
        break;
        }
      case FightAiMove::ATTACK_TRIPLE:{
        if(rand(2)){
          tr[0] = MV_ATTACK;
          tr[1] = MV_ATTACKR;
          tr[2] = MV_ATTACKL;
          } else {
          tr[0] = MV_ATTACKL;
          tr[1] = MV_ATTACKR;
          tr[2] = MV_ATTACK;
          }
        break;
        }
      case FightAiMove::ATTACK_WHIRL:{
        tr[0] = MV_ATTACKL;
        tr[1] = MV_ATTACKR;
        tr[2] = MV_ATTACKL;
        tr[3] = MV_ATTACKR;
        break;
        }
      case FightAiMove::ATTACK_MASTER:{
        tr[0] = MV_ATTACKL;
        tr[1] = MV_ATTACKR;
        tr[2] = MV_ATTACK;
        tr[3] = MV_ATTACK;
        tr[4] = MV_ATTACK;
        tr[5] = MV_ATTACK;
        break;
        }
      case FightAiMove::TURN_TO_HIT:{
        tr[0] = MV_TURN2HIT;
        break;
        }
      case FightAiMove::PARRY:{
        tr[0] = MV_BLOCK;
        break;
        }
      case FightAiMove::STAND_UP:{
        break;
        }
      case FightAiMove::WAIT:
      case FightAiMove::WAIT_EXT:{
        tr[0] = MV_WAIT;
        break;
        }
      case FightAiMove::WAIT_LONGER:{
        tr[0] = MV_WAITLONG;
        break;
        }
      default: {
        static std::set<FightAiMove> inst;
        if(inst.find(queueId)==inst.end()) {
          Tempest::Log::d("unrecognized FAI instruction: ", int(queueId));
          inst.insert(queueId);
          }
        }
      }
    queueId = FightAiMove::NOP;
    }
  return tr[0];
  }

bool FightAlgo::hasInstructions() const {
  return tr[0]!=MV_NULL;
  }

bool FightAlgo::fetchInstructions(Npc &npc, Npc &tg, GameScript& owner) {
  auto in = read(npc,tg,owner);
  return OgSimFight::run(*this, OG_SIM_FIGHT_FETCH, &in, &owner)!=0;
  }

bool FightAlgo::fetchOn(const og_sim_fight_in& in, const FightAi::FA& ai, const Rand& rand) {
  fillQueue(in,ai,rand);
  if(queueId==zenkit::FightAiMove::NOP)
    return false;
  nextOn(rand);
  return true;
  }

void FightAlgo::consumeAction() {
  OgSimFight::run(*this, OG_SIM_FIGHT_CONSUME, nullptr, nullptr);
  }

void FightAlgo::onClearTarget() {
  OgSimFight::run(*this, OG_SIM_FIGHT_CLEAR, nullptr, nullptr);
  }

void FightAlgo::onTakeHit() {
  OgSimFight::run(*this, OG_SIM_FIGHT_TAKE_HIT, nullptr, nullptr);
  }

void FightAlgo::consumeOn() {
  for(size_t i=1;i<MV_MAX;++i)
    tr[i-1]=tr[i];
  tr[MV_MAX-1]=MV_NULL;
  }

void FightAlgo::clearTargetOn() {
  queueId = zenkit::FightAiMove::NOP;
  tr[0]   = MV_NULL;
  }

void FightAlgo::takeHitOn() {
  hitFlg = true;
  for(auto& i:tr)
    i = MV_NULL;
  queueId = zenkit::FightAiMove::NOP;
  }

float FightAlgo::qDistTo(const Npc& npc, const Npc& tg) const {
  return npc.fightDistanceTo(tg).quadLength();
  }

float FightAlgo::baseDistance(const og_sim_fight_side& npc, const og_sim_fight_side& tg) {
  float  baseTg  = float(tg.range_base);
  float  baseNpc = float(npc.range_base);
  return baseTg + baseNpc;
  }

float FightAlgo::prefferedAttackDistance(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg) {
  float  baseTg  = float(tg.range_base);
  float  baseNpc = float(npc.range_base);
  return baseTg + baseNpc + weaponRange(game,npc);
  }

float FightAlgo::prefferedGDistance(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg) {
  float  baseTg  = float(tg.range_base);
  float  baseNpc = float(npc.range_base);
  return float(baseTg + baseNpc + float(npc.range_g)) + weaponRange(game,npc);
  }

bool FightAlgo::isInWRange(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg) {
  // tested in vanilla on Bloofly's:
  //  60 weapon range (Spiked club) is not enough to hit
  //  70 weapon range (Rusty Sword) is good to hit
  static float padding = 0; // padding
  auto dist = npc.dist2;
  auto pd   = prefferedAttackDistance(game,npc,tg) + padding;
  return (dist<=pd*pd);
  }

bool FightAlgo::isInGRange(uint32_t game, const og_sim_fight_side& npc, const og_sim_fight_side& tg) {
  auto dist = npc.dist2;
  auto pd   = prefferedGDistance(game,npc,tg);
  return (dist<=pd*pd);
  }

float FightAlgo::baseDistance(const Npc& npc, const Npc& tg,  GameScript &owner) const {
  return baseDistance(side(npc,tg,owner), side(tg,npc,owner));
  }

float FightAlgo::prefferedAttackDistance(const Npc& npc, const Npc& tg,  GameScript &owner) const {
  return prefferedAttackDistance(uint32_t(Gothic::inst().version().game), side(npc,tg,owner), side(tg,npc,owner));
  }

float FightAlgo::prefferedGDistance(const Npc& npc, const Npc& tg, GameScript &owner) const {
  return prefferedGDistance(uint32_t(Gothic::inst().version().game), side(npc,tg,owner), side(tg,npc,owner));
  }

float FightAlgo::attackFinishDistance(GameScript &owner) const {
  float NPC_ATTACK_FINISH_DISTANCE = 180;
  if(auto var = owner.findSymbol("NPC_ATTACK_FINISH_DISTANCE")) {
    if(var->type()==zenkit::DaedalusDataType::INT)
      NPC_ATTACK_FINISH_DISTANCE = float(var->get_int());
    else if(var->type()==zenkit::DaedalusDataType::FLOAT)
      NPC_ATTACK_FINISH_DISTANCE = var->get_float();
    }
  return NPC_ATTACK_FINISH_DISTANCE;
  }

bool FightAlgo::isInAttackRange(const Npc &npc, const Npc &tg, GameScript &owner) const {
  auto dist = qDistTo(npc, tg);
  auto pd   = prefferedAttackDistance(npc,tg,owner);
  return (dist<=pd*pd);
  }

bool FightAlgo::isInFinishRange(const Npc& npc, const Npc& tg, GameScript& owner) const {
  auto dist = qDistTo(npc, tg);
  auto pd   = attackFinishDistance(owner);
  return (dist<=pd*pd);
  }

bool FightAlgo::isInCloseupRange(const Npc& npc, const Npc& tg, GameScript& owner) const {
  // script: automatic movement of the figure (if too close) to 0.75*FightRange
  auto dist = qDistTo(npc, tg);
  auto pd   = baseDistance(npc,tg,owner) * 0.75f;
  return (dist<=pd*pd);
  }

bool FightAlgo::isInWRange(const Npc& npc, const Npc& tg, GameScript& owner) const {
  return isInWRange(uint32_t(Gothic::inst().version().game), side(npc,tg,owner), side(tg,npc,owner));
  }

bool FightAlgo::isInGRange(const Npc &npc, const Npc &tg, GameScript &owner) const {
  return isInGRange(uint32_t(Gothic::inst().version().game), side(npc,tg,owner), side(tg,npc,owner));
  }

bool FightAlgo::isInFocusAngle(const Npc &npc, const Npc &tg) const {
  static const float maxAngle = std::cos(float(30.0*M_PI/180.0));
  return angleTest(pose(npc), pose(tg), maxAngle);
  }

bool FightAlgo::isInFocusAngle(const Npc& npc, const Npc& tg, float ang) const {
  const float maxAngle = std::cos(float(ang*M_PI/180.0));
  return angleTest(pose(npc), pose(tg), maxAngle);
  }

bool FightAlgo::isInJumpBackAngle(const Npc& npc, const Npc& tg) const {
  static const float maxAngle = std::cos(float(90.0*M_PI/180.0));
  return angleTest(pose(npc), pose(tg), maxAngle);
  }

bool FightAlgo::angleTest(const og_sim_fight_side& npc, const og_sim_fight_side& tg, float cosMax) {
  // must be consistent with collisions
  const Tempest::Vec3 dpos = Tempest::Vec3(tg.center[0],tg.center[1],tg.center[2]) -
                             Tempest::Vec3(npc.center[0],npc.center[1],npc.center[2]);
  const float plAng = npc.rotation;

  const float da = plAng-std::atan2(dpos.z,dpos.x);
  const float c  = std::cos(da);

  if(c<cosMax)
    return false;
  return true;
  }

float FightAlgo::weaponRange(uint32_t game, const og_sim_fight_side& npc) {
  /*
  NOTE: comments from G2 scripts:
    Bip01 bis BBox
    FAI_W = BASE + ItemRange (or Fist)
    FAI_G = BASE + ItemRange (or Fist) + G
  */
  int   add = npc.sword_length;
  auto  bR  = game==2 ? ReferenceBowRangeG2 : ReferenceBowRangeG1;

  switch(WeaponState(npc.weapon_state)) {
    case WeaponState::W1H:
      return float(npc.range_1ha + add);
    case WeaponState::W2H:
      return float(npc.range_2ha + add);
    case WeaponState::NoWeapon:
    case WeaponState::Fist:
      return float(npc.range_fist);
    case WeaponState::Bow:
    case WeaponState::CBow:
      return float(bR);
    case WeaponState::Mage:
      return float(MaxMagRange);
    }
  return 0;
  }
