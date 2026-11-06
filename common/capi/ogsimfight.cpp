#include "ogsimfight.h"

#include <Tempest/Log>

#include "capi/ogsim.h"
#include "game/fightalgo.h"
#include "game/gamescript.h"
#include "gothic.h"

using namespace Tempest;

static int32_t apply(FightAlgo& f, uint32_t op, const og_sim_fight_in* in, const FightAi::FA& ai,
                     const FightAlgo::Rand& rand) {
  switch(op) {
    case OG_SIM_FIGHT_FETCH:
      return f.fetchOn(*in, ai, rand) ? 1 : 0;
    case OG_SIM_FIGHT_NEXT:
      return int32_t(f.nextOn(rand));
    case OG_SIM_FIGHT_CONSUME:
      f.consumeOn();
      return 0;
    case OG_SIM_FIGHT_CLEAR:
      f.clearTargetOn();
      return 0;
    case OG_SIM_FIGHT_TAKE_HIT:
      f.takeHitOn();
      return 0;
    }
  return INT32_MIN;
  }

static const FightAi::FA& tacticOf(const og_sim_fight_in* in) {
  static const FightAi::FA none = {};
  // FightAlgo's Gothic::fai()[size_t(tactic)]: a negative tactic is out of range too.
  return in!=nullptr ? Gothic::fai()[size_t(in->tactic)] : none;
  }

int32_t OgSimFight::run(FightAlgo& f, uint32_t op, const og_sim_fight_in* in, GameScript* script) {
  auto& ai = tacticOf(in);
  if(!OgSim::on(OG_SIM_MOD_FIGHTAI))
    return apply(f, op, in, ai, [script](uint32_t max) { return script->rand(max); });

  const og_sim_fight_state before = f.state();
  std::vector<og_sim_damage_draw> draws;
  const int32_t r = apply(f, op, in, ai, [script,&draws](uint32_t max) {
    uint32_t v = script->rand(max);
    draws.push_back({max, v});
    return v;
    });
  og_sim_fight_out c   = {f.state(), r};
  og_sim_fight_out out = c;
  auto& t = OgSim::table();
  t.fight(t.user, uint64_t(reinterpret_cast<uintptr_t>(&f)), op, in,
          draws.empty() ? nullptr : draws.data(), uint32_t(draws.size()), &before, &c, &out);
  if(OgSim::mode(OG_SIM_MOD_FIGHTAI)==OG_SIM_USE) {
    f.setState(out.state);
    return out.result;
    }
  return r;
  }

void OgSimFight::load(const std::vector<FightAi::FA>& fAi) {
  if(!OgSim::on(OG_SIM_MOD_FIGHTAI))
    return;
  // FightAi's createPhoenixVm("Fight.dat") reads the same bytes.
  auto dat = Gothic::inst().loadScriptBytes("Fight.dat", ScriptLang::NONE);
  std::vector<int32_t> table;
  table.reserve(fAi.size()*OG_SIM_FAI_SITUATIONS*OG_SIM_FAI_MOVES);
  for(auto& fa:fAi)
    for(auto list:FightAi::Lists)
      for(auto m:(fa.*list).move)
        table.push_back(int32_t(m));
  auto& t = OgSim::table();
  if(t.fight_load(t.user, dat.data(), dat.size(), table.data(), uint32_t(fAi.size()))!=0)
    Log::e("og_sim: Rust could not build the fight AI table (see the host log)");
  }

extern "C" uint32_t og_sim_test_fightai(uint32_t op, const og_sim_fight_in* in, const int32_t* tactic,
                                        og_sim_fight_state* state, int32_t* result, const uint32_t* raw,
                                        uint32_t raw_count, og_sim_damage_draw* draws, uint32_t cap) {
  if(op>OG_SIM_FIGHT_TAKE_HIT || (op==OG_SIM_FIGHT_FETCH && in==nullptr))
    return UINT32_MAX-1;
  FightAi::FA ai = {};
  if(tactic!=nullptr) {
    size_t k = 0;
    for(auto list:FightAi::Lists)
      for(auto& m:(ai.*list).move)
        m = zenkit::FightAiMove(tactic[k++]);
    }
  FightAlgo f;
  f.setState(*state);
  uint32_t n = 0;
  *result = apply(f, op, in, ai, [&](uint32_t max) {
    if(n>=raw_count) {
      ++n;
      return uint32_t(0);
      }
    // GameScript::rand: OgRng::next(Gameplay)%max
    uint32_t v = raw[n]%max;
    if(n<cap)
      draws[n] = {max, v};
    ++n;
    return v;
    });
  if(n>raw_count)
    return UINT32_MAX;
  *state = f.state();
  return n;
  }
