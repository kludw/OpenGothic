// opengothic-rs (task 5.18): see ogsaverefs.h.
#include "ogsaverefs.h"

#include <Tempest/Log>

#include <cstdint>
#include <string>

#include "game/gamescript.h"
#include "game/serialize.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

namespace {

enum Kind : uint8_t {
  InvalidNpc = 1,  // index in WorldObjects::npcInvalid
  MobsiItem  = 2,  // mobsi id, item class
  };

uint32_t invalidNpcId(const World& w, const Npc* npc) {
  const auto& list = OgSaveRefs::invalidNpcs(w);
  for(size_t i=0; i<list.size(); ++i)
    if(list[i].get()==npc)
      return uint32_t(i);
  return uint32_t(-1);
  }

}

const std::vector<std::unique_ptr<Npc>>& OgSaveRefs::invalidNpcs(const World& w) {
  return w.wobj.npcInvalid;
  }

void OgSaveRefs::save(Serialize& fout, GameScript& script, World& w) {
  auto& vm = script.getVm();
  struct Ref { std::string name; Kind kind; uint32_t a; uint32_t b; };
  std::vector<Ref> refs;
  for(uint32_t i=0; i<vm.symbols().size(); ++i) {
    auto* s = vm.find_symbol_by_index(i);
    if(s==nullptr || s->type()!=zenkit::DaedalusDataType::INSTANCE)
      continue;
    if(s->is_instance_of<zenkit::INpc>()) {
      auto hnpc = reinterpret_cast<const zenkit::INpc*>(s->get_instance().get());
      auto npc  = reinterpret_cast<const Npc*>(hnpc==nullptr ? nullptr : hnpc->user_ptr);
      if(npc==nullptr || w.npcId(npc)!=uint32_t(-1))
        continue;
      const uint32_t id = invalidNpcId(w,npc);
      if(id!=uint32_t(-1))
        refs.push_back(Ref{s->name(),InvalidNpc,id,0});
      }
    else if(s->is_instance_of<zenkit::IItem>()) {
      auto item = reinterpret_cast<const zenkit::IItem*>(s->get_instance().get());
      if(item==nullptr || w.itmId(item)!=uint32_t(-1))
        continue;
      // saveSym found it in an NPC's inventory (kind 3) if any NPC has its class.
      bool inNpc = false;
      for(uint32_t r=0; r<w.npcCount() && !inNpc; ++r)
        inNpc = w.npcById(r)->itemCount(item->symbol_index())>0;
      if(inNpc)
        continue;
      for(uint32_t m=0; Interactive* mob=w.mobsiById(m); ++m) {
        Item* it = mob->inventory().getItem(item->symbol_index());
        if(it!=nullptr && it->handlePtr().get()==item) {
          refs.push_back(Ref{s->name(),MobsiItem,m,uint32_t(item->symbol_index())});
          break;
          }
        }
      }
    }
  fout.write(uint32_t(refs.size()));
  for(auto& r:refs)
    fout.write(r.name,uint8_t(r.kind),r.a,r.b);
  }

void OgSaveRefs::load(Serialize& fin, GameScript& script, World& w) {
  uint32_t n = 0;
  fin.read(n);
  for(uint32_t i=0; i<n; ++i) {
    std::string name;
    uint8_t     kind = 0;
    uint32_t    a = 0, b = 0;
    fin.read(name,kind,a,b);
    auto* s = script.findSymbol(name);
    if(s==nullptr) {
      Tempest::Log::e("ogsaverefs: no symbol ",name," in this script");
      continue;
      }
    if(kind==InvalidNpc) {
      const auto& list = invalidNpcs(w);
      if(a<list.size())
        s->set_instance(list[a]->handlePtr()); else
        Tempest::Log::e("ogsaverefs: ",name,": parked NPC ",a," not in the save's world");
      }
    else if(kind==MobsiItem) {
      Interactive* mob = w.mobsiById(a);
      Item*        it  = mob!=nullptr ? mob->inventory().getItem(b) : nullptr;
      if(it!=nullptr)
        s->set_instance(it->handlePtr()); else
        Tempest::Log::e("ogsaverefs: ",name,": item ",b," not in mobsi ",a);
      }
    else {
      Tempest::Log::e("ogsaverefs: ",name,": unknown reference kind ",int(kind));
      }
    }
  }
