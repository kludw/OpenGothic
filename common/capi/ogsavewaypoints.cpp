// opengothic-rs (task 5.18): see ogsavewaypoints.h.
#include "ogsavewaypoints.h"

#include <Tempest/Log>

#include <algorithm>

#include "capi/ogsimwaynet.h"
#include "game/serialize.h"
#include "world/waymatrix.h"
#include "world/waypoint.h"
#include "world/world.h"

using namespace Tempest;

namespace {

bool before(const Serialize::OgWpRef& a, std::string_view entry, uint32_t seq) {
  return a.entry!=entry ? a.entry<entry : a.seq<seq;
  }

}

void OgSaveWaypoints::record(Serialize& fout, const World& world, uint32_t seq, const WayPoint* wp) {
  if(wp==nullptr || world.findPoint(wp->name,false)==wp)
    return;
  og_sim_point p = {};
  if(!OgSimWaynet::toPoint(*world.wmatrix, wp, p)) {
    // Not a point of this world's matrix: the load can't name it either; stock behaviour.
    Log::e("save: way point \"", wp->name, "\" (entry ", fout.ogEntry, ", way point ", seq,
           ") is not in the way net of ", world.name(), "; saved by name only");
    return;
    }
  fout.ogWpRefs.push_back({fout.ogEntry, seq, p.kind, p.index});
  }

void OgSaveWaypoints::resolve(Serialize& fin, const World& world, uint32_t seq, std::string_view name,
                              const WayPoint*& wp) {
  auto& refs = fin.ogWpRefs;
  auto  it   = std::lower_bound(refs.begin(), refs.end(), seq, [&](const Serialize::OgWpRef& a, uint32_t s) {
    return before(a, fin.ogEntry, s);
    });
  if(it==refs.end() || it->entry!=fin.ogEntry || it->seq!=seq)
    return;
  const WayPoint* exact = nullptr;
  if(!OgSimWaynet::fromPoint(*world.wmatrix, og_sim_point{it->kind, it->index}, exact) || exact==nullptr ||
     exact->name!=name) {
    // Fail loud, keep the stock answer: a save from another way net, or a read order that
    // no longer matches the write order.
    Log::e("load: way point \"", name, "\" (entry ", fin.ogEntry, ", way point ", seq, ") listed as (",
           it->kind, ", ", it->index, "), which is ", exact==nullptr ? std::string("no point") :
           "\"" + exact->name + "\"", " in ", world.name(), "; taking the first point of that name");
    return;
    }
  wp = exact;
  }

void OgSaveWaypoints::save(Serialize& fout, const World& world) {
  auto refs = std::move(fout.ogWpRefs);
  fout.ogWpRefs.clear();
  fout.setEntry("worlds/",world.name(),"/opengothic-rs/waypoints");
  fout.write(uint32_t(refs.size()));
  for(auto& r:refs)
    fout.write(r.entry,r.seq,r.kind,r.index);
  }

void OgSaveWaypoints::load(Serialize& fin, const World& world) {
  fin.ogWpRefs.clear();
  if(!fin.setEntry("worlds/",world.name(),"/opengothic-rs/waypoints"))
    return; // a stock save: names only, as stock
  uint32_t size = 0;
  fin.read(size);
  std::vector<Serialize::OgWpRef> refs(size);
  for(auto& r:refs)
    fin.read(r.entry,r.seq,r.kind,r.index);
  std::sort(refs.begin(), refs.end(), [](const Serialize::OgWpRef& a, const Serialize::OgWpRef& b) {
    return before(a, b.entry, b.seq);
    });
  fin.ogWpRefs = std::move(refs);
  }

void OgSaveWaypoints::checkSaved(Serialize& fout) {
  for(auto& r:fout.ogWpRefs)
    Log::e("save: way point ", r.seq, " of entry ", r.entry, " was written after the world's way point list; ",
           "a load takes the first point of its name");
  fout.ogWpRefs.clear();
  }
