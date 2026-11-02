// opengothic-rs: og_read_state (game/capi/og_capi.h) — copies game state out to the host.

#include "og_capi.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "game/gamescript.h"
#include "game/inventory.h"
#include "game/questlog.h"
#include "ui/dialogmenu.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "gothic.h"

namespace {

og_str str(std::string_view s) {
  return og_str{s.data(),s.size()};
  }

og_vec3 vec(const Tempest::Vec3& v) {
  return og_vec3{v.x,v.y,v.z};
  }

// Host-facing ids, valid within one loaded world. The counter is never reset, so an id
// from an earlier world (or a freed item) never names a different object.
struct Ids {
  uint64_t generation = UINT64_MAX;
  uint32_t next       = 1;
  // Npc objects live until their World is destroyed (removed ones are parked), so the
  // address is a safe key within a generation.
  std::unordered_map<const Npc*,uint32_t> npcs;
  // Items are freed on pickup/merge and their memory reused: the weak_ptr tells a
  // reused address from the original instance.
  struct ItemEntry {
    std::weak_ptr<zenkit::IItem> handle;
    uint32_t                     id = 0;
    };
  std::unordered_map<const zenkit::IItem*,ItemEntry> items;

  void sync() {
    if(generation==OgHooks::worldGeneration)
      return;
    generation = OgHooks::worldGeneration;
    npcs.clear();
    items.clear();
    }

  uint32_t npc(const Npc& n) {
    auto [it,inserted] = npcs.try_emplace(&n,0u);
    if(inserted)
      it->second = next++;
    return it->second;
    }

  uint32_t item(Item& i) {
    const auto& h  = i.handlePtr();
    auto&       e  = items[h.get()];
    if(e.id==0 || e.handle.expired()) {
      e.handle = h;
      e.id     = next++;
      }
    return e.id;
    }
  };

Ids ids;

// GIL_* constant names by value; the same for every world of one game script.
std::unordered_map<int32_t,std::string> guildNames;

std::string_view guildName(GameScript& script, int32_t guild) {
  if(guildNames.empty()) {
    for(const auto& sym:script.getVm().symbols()) {
      if(!sym.is_const() || sym.type()!=zenkit::DaedalusDataType::INT || sym.count()!=1)
        continue;
      if(sym.name().rfind("GIL_",0)!=0)
        continue;
      // First declaration wins: aliases such as GIL_SEPERATOR_* come after the guilds.
      guildNames.try_emplace(sym.get_int(),sym.name());
      }
    if(guildNames.empty())
      Tempest::Log::e("og_read_state: script has no GIL_* constants");
    }
  auto it = guildNames.find(guild);
  return it==guildNames.end() ? std::string_view() : std::string_view(it->second);
  }

std::string_view instanceName(GameScript& script, size_t symbol) {
  auto* sym = script.findSymbol(symbol);
  return sym==nullptr ? std::string_view() : std::string_view(sym->name());
  }

template<class T>
struct Near {
  T*    obj;
  float dist;
  };

template<class T>
void sortByDistance(std::vector<Near<T>>& v) {
  std::sort(v.begin(),v.end(),[](const Near<T>& a, const Near<T>& b){ return a.dist<b.dist; });
  }

bool validQuery(const og_state_query* q) {
  return std::isfinite(q->npc_range)  && q->npc_range>=0 &&
         std::isfinite(q->item_range) && q->item_range>=0;
  }

bool validVisitor(const og_state_visitor* v) {
  return v->header && v->player && v->npc && v->item && v->waypoint && v->dialog &&
         v->dialog_option && v->inventory_item && v->quest && v->quest_entry;
  }

void visit(Gothic& g, World& w, Npc& pl, const og_state_query& q, const og_state_visitor& v) {
  GameScript& script = w.script();
  void*       u      = v.user;
  ids.sync();

  const gtime t = w.time();
  og_state_header h = {};
  h.day    = t.day();
  h.hour   = int32_t(t.hour());
  h.minute = int32_t(t.minute());
  h.paused = g.isPause() ? 1 : 0;
  v.header(u,&h);

  og_player_state p = {};
  p.name             = str(pl.displayName());
  p.guild            = str(guildName(script,int32_t(pl.guild())));
  p.position         = vec(pl.position());
  p.heading_deg      = pl.rotation();
  p.hp               = pl.attribute(ATR_HITPOINTS);
  p.hp_max           = pl.attribute(ATR_HITPOINTSMAX);
  p.mana             = pl.attribute(ATR_MANA);
  p.mana_max         = pl.attribute(ATR_MANAMAX);
  p.level            = pl.level();
  p.experience       = pl.experience();
  p.experience_next  = pl.experienceNext();
  p.learning_points  = pl.learningPoints();
  p.strength         = pl.attribute(ATR_STRENGTH);
  p.dexterity        = pl.attribute(ATR_DEXTERITY);
  v.player(u,&p);

  std::vector<Near<Npc>> npcs;
  for(uint32_t i=0, n=w.npcCount(); i<n; ++i) {
    Npc* npc = w.npcById(i);
    if(npc==nullptr || npc==&pl)
      continue;
    const float d = std::sqrt(npc->qDistTo(pl));
    if(d<=q.npc_range)
      npcs.push_back({npc,d});
    }
  sortByDistance(npcs);
  for(auto& e:npcs) {
    Npc& npc = *e.obj;
    const Attitude tmp = npc.tempAttitude();
    og_npc_state s = {};
    s.id        = ids.npc(npc);
    s.name      = str(npc.displayName());
    s.guild     = str(guildName(script,int32_t(npc.guild())));
    s.position  = vec(npc.position());
    s.distance  = e.dist;
    s.attitude  = tmp!=ATT_NULL ? tmp : script.personAttitude(npc,pl);
    s.hp        = npc.attribute(ATR_HITPOINTS);
    s.hp_max    = npc.attribute(ATR_HITPOINTSMAX);
    s.in_dialog = g.isNpcInDialog(npc) ? 1 : 0;
    v.npc(u,&s);
    }

  const Tempest::Vec3 plPos = pl.position();
  std::vector<Near<Item>> items;
  for(uint32_t i=0; Item* it=w.itmById(i); ++i) {
    const float d = (it->position()-plPos).length();
    if(d<=q.item_range)
      items.push_back({it,d});
    }
  sortByDistance(items);
  for(auto& e:items) {
    Item& it = *e.obj;
    og_item_state s = {};
    s.id       = ids.item(it);
    s.instance = str(instanceName(script,it.clsId()));
    s.name     = str(it.displayName());
    s.amount   = it.count();
    s.position = vec(it.position());
    s.distance = e.dist;
    v.item(u,&s);
    }

  // No public waypoint list: a filter that never accepts visits every waynet point.
  std::vector<Near<const WayPoint>> wps;
  w.findWayPoint(plPos,[&](const WayPoint& wp) {
    wps.push_back({&wp,(wp.position()-plPos).length()});
    return false;
    });
  const size_t nWp = std::min<size_t>(wps.size(),q.max_waypoints);
  std::partial_sort(wps.begin(),wps.begin()+ptrdiff_t(nWp),wps.end(),
                    [](const Near<const WayPoint>& a, const Near<const WayPoint>& b){ return a.dist<b.dist; });
  for(size_t i=0; i<nWp; ++i) {
    og_waypoint_state s = {};
    s.name     = str(wps[i].obj->name);
    s.position = vec(wps[i].obj->position());
    s.distance = wps[i].dist;
    v.waypoint(u,&s);
    }

  if(OgHooks::dialogMenu!=nullptr) {
    const DialogMenu::OgView dlg = OgHooks::dialogMenu->ogView();
    if(dlg.partner!=nullptr) {
      og_dialog_state s = {};
      s.partner      = ids.npc(*dlg.partner);
      s.partner_name = str(dlg.partner->displayName());
      if(dlg.line!=nullptr) {
        s.has_line     = 1;
        s.line_speaker = str(dlg.lineIsPlayer ? pl.displayName() : dlg.partner->displayName());
        s.line_text    = str(*dlg.line);
        }
      v.dialog(u,&s);
      if(dlg.choices!=nullptr) {
        for(size_t i=0; i<dlg.choices->size(); ++i) {
          const auto& c = (*dlg.choices)[i];
          // Untitled infos run on their own and are never shown.
          if(c.title.empty())
            continue;
          og_dialog_option_state o = {};
          o.index = uint32_t(i);
          o.text  = str(c.title);
          v.dialog_option(u,&o);
          }
        }
      }
    }

  // Side effect: iterator() sorts the inventory (as the inventory UI does); scripts that
  // walk it by slot can see a different order after a read.
  for(auto i=pl.inventory().iterator(Inventory::T_Inventory); i.isValid(); ++i) {
    const Item& it = *i;
    og_inventory_item_state s = {};
    s.instance = str(instanceName(script,it.clsId()));
    s.name     = str(it.displayName());
    s.amount   = i.count();
    s.equipped = i.isEquipped() ? 1 : 0;
    v.inventory_item(u,&s);
    }

  if(const QuestLog* log = g.questLog()) {
    for(size_t i=0; i<log->questCount(); ++i) {
      const auto& quest = log->quest(i);
      og_quest_state s = {};
      s.topic   = str(quest.name);
      s.section = int32_t(quest.section);
      s.status  = int32_t(quest.status);
      v.quest(u,&s);
      for(const auto& entry:quest.entry)
        v.quest_entry(u,str(entry));
      }
    }
  }
}

int og_read_state(og_ctx* ctx, const og_state_query* q, const og_state_visitor* v) {
  try {
    if(ctx==nullptr || !ctx->inFrame) {
      Tempest::Log::e("og_read_state: called outside on_frame");
      return OG_STATE_BAD_ARGS;
      }
    if(q==nullptr || v==nullptr || !validVisitor(v) || !validQuery(q)) {
      Tempest::Log::e("og_read_state: invalid query or visitor");
      return OG_STATE_BAD_ARGS;
      }
    Gothic& g  = Gothic::inst();
    World*  w  = g.world();
    Npc*    pl = g.player();
    if(g.checkLoading()!=Gothic::LoadState::Idle || w==nullptr || pl==nullptr)
      return OG_STATE_NOT_IN_GAME;
    visit(g,*w,*pl,*q,*v);
    return OG_STATE_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_read_state: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_read_state: unknown exception");
    }
  return OG_STATE_EXCEPTION;
  }
