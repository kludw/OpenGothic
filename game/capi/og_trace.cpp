// opengothic-rs: og_read_world_trace (game/capi/og_capi.h, v17): the per-step world trace
// of task 5.5. Read-only: unlike og_read_state it hands out no seam ids (og_ids.h gives
// ids on first read, an observer effect a trace must not have) and sorts no inventory.
// Also og_dump_world (v20, task 5.18): the save written through Serialize's dump mode.

#include "og_capi.h"

#include <Tempest/Log>

#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>
#include <unordered_map>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "game/questlog.h"
#include "game/serialize.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"

namespace {

og_str str(std::string_view s) {
  return og_str{s.data(),s.size()};
  }

std::string_view symbolName(GameScript& script, size_t symbol) {
  auto* sym = script.findSymbol(symbol);
  return sym==nullptr ? std::string_view() : std::string_view(sym->name());
  }

void floatBits(OgFnv& h, float f) {
  uint32_t b = 0;
  std::memcpy(&b,&f,sizeof(b));
  h.u64(b);
  }

// Sum, not a chained hash: og_read_state and the inventory UI sort storage, and a trace
// must not depend on whether anyone looked.
void inventoryHash(const Npc& npc, og_trace_npc& out) {
  uint64_t sum = 0;
  uint32_t n   = 0;
  for(const auto& it:npc.inventory().ogItems()) {
    OgFnv h;
    h.u64(it->clsId());
    h.u64(it->count());
    h.u64(it->equipCount());
    sum += h.h;
    ++n;
    }
  out.inventory_count = n;
  out.inventory_hash  = sum;
  }

void itemsHash(World& w, og_trace_header& out) {
  OgFnv h;
  uint32_t n = 0;
  for(uint32_t i=0; Item* it=w.itmById(i); ++i) {
    const Tempest::Vec3 p = it->position();
    h.u64(it->clsId());
    h.u64(it->count());
    floatBits(h,p.x);
    floatBits(h,p.y);
    floatBits(h,p.z);
    ++n;
    }
  out.item_count = n;
  out.items_hash = h.h;
  }

void questsHash(const QuestLog* log, og_trace_header& out) {
  OgFnv h;
  if(log!=nullptr) {
    for(size_t i=0; i<log->questCount(); ++i) {
      const auto& q = log->quest(i);
      h.text(q.name);
      h.u64(uint64_t(q.section));
      h.u64(uint64_t(q.status));
      h.u64(q.entry.size());
      for(const auto& e:q.entry)
        h.text(e);
      }
    out.quest_count = uint32_t(log->questCount());
    }
  out.quests_hash = h.h;
  }

// Script state outside NPCs and items lives in global variables. Instance and function
// variables hold references (addresses), not comparable values; constants never change.
void globalsHash(GameScript& script, og_trace_header& out) {
  OgFnv h;
  uint32_t n = 0;
  const auto& syms = script.getVm().symbols();
  for(size_t i=0; i<syms.size(); ++i) {
    const auto& sym = syms[i];
    if(sym.is_const() || sym.is_member())
      continue;
    const auto type = sym.type();
    if(type!=zenkit::DaedalusDataType::INT && type!=zenkit::DaedalusDataType::FLOAT &&
       type!=zenkit::DaedalusDataType::STRING)
      continue;
    h.u64(i);
    for(uint32_t k=0; k<sym.count(); ++k) {
      const auto idx = uint16_t(k);
      switch(type) {
        case zenkit::DaedalusDataType::INT:    h.u64(uint64_t(int64_t(sym.get_int(idx)))); break;
        case zenkit::DaedalusDataType::FLOAT:  floatBits(h,sym.get_float(idx));            break;
        default:                               h.text(sym.get_string(idx));                break;
        }
      }
    ++n;
    }
  out.global_count = n;
  out.globals_hash = h.h;
  }

void visit(Gothic& g, World& w, const og_trace_visitor& v) {
  GameScript& script = w.script();
  const GameSession* session = g.gameSession();

  og_trace_header hdr = {};
  hdr.clock.world_generation = OgHooks::worldGeneration;
  if(session!=nullptr) {
    hdr.clock.in_session    = 1;
    hdr.clock.session_ticks = session->tickCount();
    hdr.clock.world_time_ms = session->time().toInt();
    }
  // Removed NPCs (Wld_RemoveNpc) leave the list, so later indices shift; World::npcId
  // is a linear search, hence one map for the targets.
  std::unordered_map<const Npc*,uint32_t> index;
  const uint32_t total = w.npcCount();
  for(uint32_t i=0; i<total; ++i)
    if(const Npc* npc = w.npcById(i))
      index.emplace(npc,i);
  hdr.npc_count = uint32_t(index.size());
  itemsHash(w,hdr);
  questsHash(g.questLog(),hdr);
  globalsHash(script,hdr);
  v.header(v.user,&hdr);

  const Npc* player = g.player();
  for(uint32_t i=0; i<total; ++i) {
    Npc* npc = w.npcById(i);
    if(npc==nullptr)
      continue;
    const Tempest::Vec3 p = npc->position();
    const Npc* target = npc->target();
    og_trace_npc n = {};
    n.index        = i;
    n.instance     = str(symbolName(script,npc->instanceSymbol()));
    n.player       = npc==player ? 1 : 0;
    n.position     = og_vec3{p.x,p.y,p.z};
    n.yaw          = npc->rotation();
    n.body_state   = uint32_t(npc->bodyState());
    n.ai_state     = str(npc->ogAiStateFunc()==size_t(-1) ? std::string_view()
                                                         : symbolName(script,npc->ogAiStateFunc()));
    n.queue_length = uint32_t(npc->ogAiQueueSize());
    n.queue_head   = uint32_t(npc->ogAiQueueHead());
    n.hp           = npc->attribute(ATR_HITPOINTS);
    n.hp_max       = npc->attribute(ATR_HITPOINTSMAX);
    n.mana         = npc->attribute(ATR_MANA);
    n.mana_max     = npc->attribute(ATR_MANAMAX);
    n.weapon_state = int32_t(npc->weaponState());
    n.walk_mode    = int32_t(npc->walkMode());
    n.target       = UINT32_MAX;
    if(target!=nullptr) {
      auto it = index.find(target);
      if(it!=index.end())
        n.target = it->second;
      }
    inventoryHash(*npc,n);
    v.npc(v.user,&n);
    }
  }
}

int og_read_world_trace(og_ctx* ctx, const og_trace_visitor* v) {
  try {
    if(ctx==nullptr || !ctx->inFrame) {
      Tempest::Log::e("og_read_world_trace: called outside on_frame");
      return OG_STATE_BAD_ARGS;
      }
    if(v==nullptr || v->header==nullptr || v->npc==nullptr) {
      Tempest::Log::e("og_read_world_trace: invalid visitor");
      return OG_STATE_BAD_ARGS;
      }
    Gothic& g = Gothic::inst();
    World*  w = g.world();
    if(g.checkLoading()!=Gothic::LoadState::Idle || w==nullptr || g.player()==nullptr ||
       g.gameSession()==nullptr)
      return OG_STATE_NOT_IN_GAME;
    visit(g,*w,*v);
    return OG_STATE_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_read_world_trace: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_read_world_trace: unknown exception");
    }
  return OG_STATE_EXCEPTION;
  }

int og_dump_world(og_ctx* ctx, const og_dump_sink* sink) {
  try {
    if(ctx==nullptr || !ctx->inFrame) {
      Tempest::Log::e("og_dump_world: called outside on_frame");
      return OG_STATE_BAD_ARGS;
      }
    if(sink==nullptr || sink->write==nullptr) {
      Tempest::Log::e("og_dump_world: invalid sink");
      return OG_STATE_BAD_ARGS;
      }
    Gothic&      g       = Gothic::inst();
    GameSession* session = g.gameSession();
    if(g.checkLoading()!=Gothic::LoadState::Idle || g.world()==nullptr || g.player()==nullptr ||
       session==nullptr)
      return OG_STATE_NOT_IN_GAME;
    std::string json = "[";
    {
    Serialize out(json);
    session->save(out,"og_dump_world",Tempest::Pixmap());
    }
    json.push_back(']');
    sink->write(sink->user,json.data(),json.size());
    return OG_STATE_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_dump_world: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_dump_world: unknown exception");
    }
  return OG_STATE_EXCEPTION;
  }
