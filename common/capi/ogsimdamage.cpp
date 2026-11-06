#include "ogsimdamage.h"

#include <vector>

#include "capi/ogsim.h"
#include "game/gamescript.h"

DamageCalculator::Val OgSimDamage::run(const og_sim_damage_in& in, GameScript& script) {
  if(!OgSim::on(OG_SIM_MOD_DAMAGE))
    return DamageCalculator::compute(in, [&script](uint32_t max) { return script.rand(max); });

  std::vector<og_sim_damage_draw> draws;
  auto cpp = DamageCalculator::compute(in, [&script,&draws](uint32_t max) {
    uint32_t v = script.rand(max);
    draws.push_back({max, v});
    return v;
    });
  og_sim_damage_out c   = {cpp.value, cpp.hasHit ? 1 : 0, cpp.invincible ? 1 : 0};
  og_sim_damage_out out = c;
  auto& t = OgSim::table();
  t.damage(t.user, &in, draws.empty() ? nullptr : draws.data(), uint32_t(draws.size()), &c, &out);
  if(OgSim::mode(OG_SIM_MOD_DAMAGE)==OG_SIM_USE)
    return DamageCalculator::Val(out.value, out.has_hit!=0, out.invincible!=0);
  return cpp;
  }

extern "C" uint32_t og_sim_test_damage(const og_sim_damage_in* in, const uint32_t* raw, uint32_t raw_count,
                                       og_sim_damage_out* out, og_sim_damage_draw* draws, uint32_t cap) {
  uint32_t n = 0;
  auto r = DamageCalculator::compute(*in, [&](uint32_t max) {
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
  *out = {r.value, r.hasHit ? 1 : 0, r.invincible ? 1 : 0};
  return n;
  }
