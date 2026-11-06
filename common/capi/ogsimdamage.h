#pragma once

// opengothic-rs: DamageCalculator's og-sim delegation (task 5.15, og_sim.h module
// OG_SIM_MOD_DAMAGE). C++ computes (drawing from GameScript::rand as stock), Rust
// computes the same from the inputs and C++'s draws; USE mode takes Rust's result.

#include "game/damagecalculator.h"

class GameScript;

namespace OgSimDamage {
  DamageCalculator::Val run(const og_sim_damage_in& in, GameScript& script);
  }
