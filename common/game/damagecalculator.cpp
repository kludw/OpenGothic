#include "damagecalculator.h"

#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "world/bullet.h"
#include "gothic.h"
#include "capi/ogsimdamage.h"
#include "physics/dynamicworld.h"

// https://forum.worldofplayers.de/forum/threads/127320-Damage-System?p=2198181#post2198181
// https://strafkolonie-online.net/forum/board/thread/1895-info-erkl%C3%A4rung-der-berechnung-der-trefferchance-im-fernkampf/

static float mix(float x, float y, float a) {
  return x + (y-x)*a;
  }

// opengothic-rs (task 5.15): damageValue/damageFall read everything into an
// og_sim_damage_in and compute() works on that alone; the arithmetic, the order of the
// rand calls and the results are stock.
static og_sim_damage_in readNpcs(Npc& src, Npc& other) {
  og_sim_damage_in in = {};
  in.game       = uint32_t(Gothic::inst().version().game);
  in.strength   = src.attribute(Attribute::ATR_STRENGTH);
  in.is_monster = src.isMonster() ? 1 : 0;
  in.damage_type = DamageCalculator::damageTypeMask(src);
  in.crit_mul   = src.world().script().criticalDamageMultiplyer();
  if(auto w = src.inventory().activeWeapon())
    in.weapon = w->is2H() ? 2 : 1;
  for(int32_t t : {TALENT_UNKNOWN, TALENT_1H, TALENT_2H}) {
    in.hitchance[t] = src.handle().hitchance[t];
    in.talent[t]    = src.talentValue(Talent(t));
    }
  for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
    in.src_damage[i] = src.handle().damage[i];
    in.protection[i] = other.handle().protection[i];
    }
  return in;
  }

DamageCalculator::Val DamageCalculator::damageValue(Npc& src, Npc& other, const Bullet* b, bool isSpell, const DamageCalculator::Damage& splDmg, const CollideMask bMsk) {
  og_sim_damage_in in = readNpcs(src,other);
  in.coll_mask = uint32_t(bMsk);
  if(b!=nullptr) {
    in.kind            = OG_SIM_DAMAGE_BULLET;
    in.path_length     = b->pathLength();
    in.bullet_is_spell = b->isSpell() ? 1 : 0;
    in.hit_chance      = b->hitChance();
    in.crit_chance     = b->critChance();
    for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i)
      in.damage[i] = b->damage().val[i];
    } else
  if(isSpell) {
    in.kind = OG_SIM_DAMAGE_SPELL;
    for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i)
      in.damage[i] = splDmg.val[i];
    }
  else {
    in.kind = OG_SIM_DAMAGE_MELEE;
    }
  return OgSimDamage::run(in, src.world().script());
  }

DamageCalculator::Val DamageCalculator::damageFall(Npc& npc, float speed) {
  auto  gl = npc.guild();
  auto& g  = npc.world().script().guildVal();

  og_sim_damage_in in = {};
  in.kind            = OG_SIM_DAMAGE_FALL;
  in.game            = uint32_t(Gothic::inst().version().game);
  in.speed           = speed;
  in.falldown_height = g.falldown_height[gl];
  in.falldown_damage = g.falldown_damage[gl];
  in.prot_fall       = npc.protection(::PROT_FALL);
  return OgSimDamage::run(in, npc.world().script());
  }

DamageCalculator::Val DamageCalculator::compute(const og_sim_damage_in& in, const Rand& rand) {
  DamageCalculator::Val ret;
  switch(in.kind) {
    case OG_SIM_DAMAGE_BULLET:
      ret = rangeDamage(in,rand);
      break;
    case OG_SIM_DAMAGE_SPELL: {
      Damage dmg;
      for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i)
        dmg[i] = in.damage[i];
      ret = rangeDamage(in,dmg);
      break;
      }
    case OG_SIM_DAMAGE_FALL:
      return fallDamage(in);
    default:
      ret = swordDamage(in,rand);
      break;
    }

#if 0
  // debug
  ret.value = MinDamage;
#endif

  if(ret.hasHit && !ret.invincible && in.game==2)
    ret.value = std::max<int32_t>(ret.value,MinDamage);
  return ret;
  }

DamageCalculator::Val DamageCalculator::fallDamage(const og_sim_damage_in& in) {
  float   gravity     = DynamicWorld::gravity;
  float   fallTime    = in.speed/gravity;
  float   height      = 0.5f*std::abs(gravity)*fallTime*fallTime;
  float   h0          = float(in.falldown_height);
  float   dmgPerMeter = float(in.falldown_damage);
  int32_t prot        = in.prot_fall;

  Val ret;
  ret.invincible = (prot<0);
  ret.value      = int32_t(dmgPerMeter*(height-h0)/100.f - float(prot));
  if(ret.value<=0 || ret.invincible) {
    ret.value = 0;
    return ret;
    }
  ret.hasHit = true;
  return ret;
  }

DamageCalculator::Val DamageCalculator::rangeDamage(const og_sim_damage_in& in, const Rand& rand) {
  float dist       = in.path_length;
  bool  noHit      = dist>float(MaxMagRange);
  bool  invincible = !checkDamageMask(in);
  const CollideMask bMsk = CollideMask(in.coll_mask);
  Damage dmg;
  for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i)
    dmg[i] = in.damage[i];

  if(in.bullet_is_spell==0) {
    float hitChance = float(rand(100))/100.f;
    float hitCh     = 0;
    bool  g2        = in.game==2;
    float refRange  = g2 ? ReferenceBowRangeG2 : ReferenceBowRangeG1;
    float maxRange  = float(MaxBowRange);
    float chance    = in.hit_chance;

    if(dist<refRange)
      hitCh = mix(1.f, chance, (dist / refRange));
    else if(dist<maxRange)
      hitCh = mix(chance, 0.f, (dist-refRange) / (maxRange-refRange));
    else
      hitCh = 0;

    noHit = (dist>float(MaxBowRange) || hitCh<=hitChance);

    if(!g2 && !noHit && !invincible) {
      const int32_t mul        = in.crit_mul;
      const int     critChance = int(rand(100));
      if(std::lround(100.f * in.crit_chance)>critChance)
        dmg *= mul;
      }
    }

  if(noHit)
    return Val(0,false,invincible);

  if(invincible)
    return Val(0,true,true);

  if((bMsk & (COLL_APPLYDAMAGE | COLL_APPLYDOUBLEDAMAGE | COLL_APPLYHALVEDAMAGE | COLL_DOEVERYTHING))==0)
    return Val(0,true,true);

  return rangeDamage(in,dmg);
  }

DamageCalculator::Val DamageCalculator::rangeDamage(const og_sim_damage_in& in, Damage dmg) {
  const CollideMask bMsk = CollideMask(in.coll_mask);

  if(bMsk & COLL_APPLYDOUBLEDAMAGE)
    dmg*=2;
  if(bMsk & COLL_APPLYHALVEDAMAGE)
    dmg/=2;

  int  value = 0;
  bool invincible = true;
  for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
    if(dmg[size_t(i)]==0)
      continue;
    int vd = std::max(dmg[size_t(i)] - in.protection[i],0);
    if(in.protection[i]>=0) { // Filter immune
      value     += vd;
      invincible = false;
      }
    }

  return Val(value,true,invincible);
  }

DamageCalculator::Val DamageCalculator::swordDamage(const og_sim_damage_in& in, const Rand& rand) {
  if(!checkDamageMask(in))
    return Val(0,true,true);

  // Swords/Fists
  const int dtype      = in.damage_type;
  Talent    tal        = TALENT_UNKNOWN;
  int       str        = in.strength;
  int       critChance = int(rand(100));

  int value = 0;

  if(in.weapon!=0) {
    if(in.weapon==2)
      tal = TALENT_2H; else
      tal = TALENT_1H;
    }

  if(in.game==2) {
    if(in.is_monster!=0 && tal==TALENT_UNKNOWN) {
      // regular monsters always do critical damage
      critChance = -1;
      }

    bool invincible = true;
    for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
      if((dtype & (1<<i))==0)
        continue;
      int vd = std::max(str + in.src_damage[i] - in.protection[i],0);
      if(in.hitchance[tal]<=critChance)
        vd = (vd-1)/10;
      if(in.protection[i]>=0) { // Filter immune
        value += vd;
        invincible = false;
        }
      }

    return Val(value,true,invincible);
    } else {
    bool invincible = true;
    const int32_t mul = in.crit_mul;
    for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
      if((dtype & (1<<i))==0)
        continue;
      int vd = 0;
      if(in.talent[tal]<=critChance)
        vd = std::max(str +     in.src_damage[i] - in.protection[i],0); else
        vd = std::max(str + mul*in.src_damage[i] - in.protection[i],0);
      if(in.protection[i]>=0) { // Filter immune
        value += vd;
        invincible = false;
        }
      }

    return Val(value,true,invincible);
    }
  }

int32_t DamageCalculator::damageTypeMask(Npc& npc) {
  if(auto w = npc.inventory().activeWeapon())
    return w->handle().damage_type;
  return npc.handle().damage_type;
  }

bool DamageCalculator::checkDamageMask(const og_sim_damage_in& in) {
  if(in.kind==OG_SIM_DAMAGE_BULLET) {
    for(unsigned int i=0;i<zenkit::DamageType::NUM;++i) {
      if(in.damage[i]>0 && in.protection[i]>=0)
        return true;
      }
    } else {
    const int dtype = in.damage_type;
    for(unsigned int i=0;i<zenkit::DamageType::NUM;++i){
      if((dtype & (1<<i))==0)
        continue;
      return true;
      }
    }

  return false;
  }

DamageCalculator::Damage DamageCalculator::rangeDamageValue(Npc& src) {
  const int dtype = damageTypeMask(src);
  int d = Gothic::inst().version().game==2 ? src.attribute(Attribute::ATR_DEXTERITY) : 0;
  Damage ret={};
  for(unsigned int i=0;i<zenkit::DamageType::NUM;++i){
    if((dtype & (1<<i))==0)
      continue;
    ret[size_t(i)] = d + src.handle().damage[i];
    }
  return ret;
  }
