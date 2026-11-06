#pragma once

#include <zenkit/addon/daedalus.hh>

#include <functional>

#include "game/constants.h"
#include "../../game/capi/og_sim.h"

class Npc;
class Bullet;

class DamageCalculator {
  public:
    enum {
      MinDamage = 5, //TODO: NPC_MINIMAL_DAMAGE?
      };

    struct Val final {
      Val()=default;
      Val(int32_t v,bool b):value(v),hasHit(b){}
      Val(int32_t v,bool b,bool i):value(v),hasHit(b),invincible(i){}

      int32_t value      = 0;
      bool    hasHit     = false;
      bool    invincible = false;
      };

    struct Damage final {
      int32_t  val[zenkit::DamageType::NUM] = {};
      int32_t& operator[](size_t i) { return val[i]; }
      void     operator *= (int32_t v) { for(auto& i:val) i*=v; }
      void     operator /= (int32_t v) { for(auto& i:val) i/=v; }
      };

    // GameScript::rand(max)
    using Rand = std::function<uint32_t(uint32_t)>;

    static Val     damageValue(Npc& src, Npc& other, const Bullet* b, bool isSpell, const DamageCalculator::Damage& splDmg, const CollideMask bMsk);
    static Val     damageFall(Npc& src, float speed);
    static auto    rangeDamageValue(Npc& src) -> Damage;
    static int32_t damageTypeMask(Npc& npc);

    // opengothic-rs (task 5.15): the computation on what damageValue/damageFall read
    // (og_sim.h), so og-sim can be compared with it (og_sim_test_damage, OgSimDamage).
    static Val     compute(const og_sim_damage_in& in, const Rand& rand);

  private:
    static bool    checkDamageMask(const og_sim_damage_in& in);

    static Val     rangeDamage(const og_sim_damage_in& in, const Rand& rand);
    static Val     rangeDamage(const og_sim_damage_in& in, Damage dmg);
    static Val     swordDamage(const og_sim_damage_in& in, const Rand& rand);
    static Val     fallDamage(const og_sim_damage_in& in);
  };

