#pragma once

// opengothic-rs: host-facing NPC/item ids shared by og_state.cpp and og_control.cpp.

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "capi/oghooks.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

// Valid within one loaded world. The counter is never reset, so an id from an earlier
// world (or a freed item) never names a different object. Main thread only.
struct OgIds {
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
    sync();
    auto [it,inserted] = npcs.try_emplace(&n,0u);
    if(inserted)
      it->second = next++;
    return it->second;
    }

  uint32_t item(Item& i) {
    sync();
    const auto& h  = i.handlePtr();
    auto&       e  = items[h.get()];
    if(e.id==0 || e.handle.expired()) {
      e.handle = h;
      e.id     = next++;
      }
    return e.id;
    }

  // The NPC with this id if it is still part of the world (not removed), else nullptr.
  Npc* findNpc(World& w, uint32_t id) {
    sync();
    for(const auto& [ptr,nid]:npcs) {
      if(nid!=id)
        continue;
      if(w.npcId(ptr)==uint32_t(-1))
        return nullptr;
      return w.npcById(w.npcId(ptr));
      }
    return nullptr;
    }

  // The item with this id if it still lies in the world, else nullptr.
  Item* findItem(World& w, uint32_t id) {
    sync();
    for(const auto& [ptr,e]:items) {
      if(e.id!=id)
        continue;
      if(e.handle.expired())
        return nullptr;
      for(uint32_t i=0; Item* it=w.itmById(i); ++i)
        if(it->handlePtr().get()==ptr)
          return it;
      return nullptr;
      }
    return nullptr;
    }
  };

OgIds& ogIds();
