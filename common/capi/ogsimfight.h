#pragma once

// opengothic-rs: FightAlgo's og-sim delegation (task 5.16, og_sim.h module
// OG_SIM_MOD_FIGHTAI). C++ runs every operation as stock (drawing from GameScript::rand);
// Rust runs the same on the state before it, the inputs and C++'s draws; USE mode takes
// Rust's state and result.

#include <cstdint>
#include <vector>

#include "game/definitions/fightaidefinitions.h"
#include "../../game/capi/og_sim.h"

class FightAlgo;
class GameScript;

namespace OgSimFight {
  // A FightAlgo operation (OG_SIM_FIGHT_*): its result (FETCH's bool, NEXT's Action, else
  // 0). `in` is set for FETCH only; `script` answers rand and may be NULL for
  // CONSUME/CLEAR/TAKE_HIT.
  int32_t run(FightAlgo& f, uint32_t op, const og_sim_fight_in* in, GameScript* script);
  // FightAi's constructor built `fAi`: hands it and FIGHT.DAT to Rust.
  void    load(const std::vector<FightAi::FA>& fAi);
  }
