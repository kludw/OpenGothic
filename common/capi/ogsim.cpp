#include "ogsim.h"

#include <Tempest/Log>

#include <cstdint>

#include "game/gametime.h"

using namespace Tempest;

namespace {
og_sim_vtable vtable  = {};
uint8_t       modes[OG_SIM_MODS] = {};
}

extern "C" int32_t og_sim_set(const og_sim_vtable* vt, const uint8_t* m) {
  if(vt==nullptr || m==nullptr) {
    Log::e("og_sim_set: NULL table or modes");
    return -1;
    }
  for(uint32_t i=0; i<OG_SIM_MODS; ++i) {
    if(m[i]>OG_SIM_USE) {
      Log::e("og_sim_set: module ",i," has mode ",int(m[i]));
      return -1;
      }
    }
  if(!vt->time || !vt->is_time || !vt->rng_seed || !vt->rng_next || !vt->rng_keyed ||
     !vt->rng_info || !vt->rng_load) {
    Log::e("og_sim_set: table with a missing callback");
    return -1;
    }
  vtable = *vt;
  for(uint32_t i=0; i<OG_SIM_MODS; ++i)
    modes[i] = m[i];
  Log::i("og_sim: time mode ",int(modes[OG_SIM_MOD_TIME]),", rng mode ",int(modes[OG_SIM_MOD_RNG]));
  return 0;
  }

extern "C" int64_t og_sim_test_gtime(uint32_t op, int64_t a, int64_t b, int64_t c) {
  switch(op) {
    case 0: return gtime(int32_t(a),int32_t(b)).toInt();
    case 1: return gtime(a,b,c).toInt();
    case 2: return gtime::fromInt(a).day();
    case 3: return gtime::fromInt(a).timeInDay().toInt();
    case 4: return gtime::fromInt(a).hour();
    case 5: return gtime::fromInt(a).minute();
    case 6: {
      gtime t = gtime::fromInt(a);
      t.addMilis(uint64_t(b));
      return t.toInt();
      }
    }
  return INT64_MIN;
  }

namespace OgSim {
  uint8_t mode(uint32_t module) {
    return module<OG_SIM_MODS ? modes[module] : OG_SIM_OFF;
    }

  const og_sim_vtable& table() {
    return vtable;
    }
  }
