#pragma once

// opengothic-rs: the og-sim delegation table (game/capi/og_sim.h, task 5.11, ADR 0023)
// for engine code. Off unless the host registered a table with og_sim_set.

#include <cstdint>

#include "../../game/capi/og_sim.h"

class gtime;

namespace OgSim {
  // The module's mode (OG_SIM_OFF/SHADOW/USE).
  uint8_t mode(uint32_t module);
  inline bool on(uint32_t module) { return mode(module)!=OG_SIM_OFF; }
  // The registered table; only valid while on(module) for some module.
  const og_sim_vtable& table();
  }
