#include "ogsimwaynet.h"

#include <Tempest/Log>

#include <zenkit/Stream.hh>
#include <zenkit/Vfs.hh>

#include <stdexcept>
#include <string>
#include <vector>

#include "capi/ogsim.h"
#include "physics/dynamicworld.h"
#include "world/objects/interactive.h"
#include "world/waymatrix.h"
#include "world/world.h"

using namespace Tempest;

namespace {
// The world whose way net Rust holds: set at load, cleared when that world goes.
const WayMatrix* current = nullptr;

struct FilterCtx {
  const WayMatrix*                            m;
  const std::function<bool(const WayPoint&)>* filter;
  bool                                        bad;
  };

// Counts a mismatch this file found (already logged) on the Rust side.
void counted(const char* what) {
  auto& t = OgSim::table();
  t.waynet_mismatch(t.user, what);
  }
}

// The filter pointer the table stores has C language linkage.
extern "C" {
static int32_t ogSimWaynetFilter(void* ctx, og_sim_point p) {
  return OgSimWaynet::callFilter(ctx, p);
  }
}

bool OgSimWaynet::toPoint(const WayMatrix& m, const WayPoint* p, og_sim_point& out) {
  if(p==nullptr) {
    out = {OG_SIM_POINT_NONE, 0};
    return true;
    }
  const std::vector<WayPoint>* sets[3] = {&m.wayPoints, &m.freePoints, &m.startPoints};
  const uint32_t kinds[3] = {OG_SIM_POINT_WAY, OG_SIM_POINT_FREE, OG_SIM_POINT_START};
  std::less<const WayPoint*> lt;
  for(int k=0; k<3; ++k) {
    auto& v = *sets[k];
    if(!v.empty() && !lt(p, v.data()) && lt(p, v.data()+v.size())) {
      out = {kinds[k], uint32_t(p - v.data())};
      return true;
      }
    }
  // Not in the lists: startPoint()/deadPoint() return their statics only then.
  if(p==&m.startPoint()) {
    out = {OG_SIM_POINT_DEFAULT_START, 0};
    return true;
    }
  if(p==&m.deadPoint()) {
    out = {OG_SIM_POINT_DEFAULT_DEAD, 0};
    return true;
    }
  return false;
  }

bool OgSimWaynet::fromPoint(const WayMatrix& m, og_sim_point p, const WayPoint*& out) {
  auto at = [&](const std::vector<WayPoint>& v) {
    if(p.index>=v.size())
      return false;
    out = &v[p.index];
    return true;
    };
  // A default point only while the matrix falls back to it, as on the Rust side.
  auto fallback = [&](const WayPoint& s, uint32_t kind) {
    og_sim_point k;
    if(p.index!=0 || !toPoint(m, &s, k) || k.kind!=kind)
      return false;
    out = &s;
    return true;
    };
  switch(p.kind) {
    case OG_SIM_POINT_NONE:
      out = nullptr;
      return p.index==0;
    case OG_SIM_POINT_WAY:           return at(m.wayPoints);
    case OG_SIM_POINT_FREE:          return at(m.freePoints);
    case OG_SIM_POINT_START:         return at(m.startPoints);
    case OG_SIM_POINT_DEFAULT_START: return fallback(m.startPoint(), OG_SIM_POINT_DEFAULT_START);
    case OG_SIM_POINT_DEFAULT_DEAD:  return fallback(m.deadPoint(),  OG_SIM_POINT_DEFAULT_DEAD);
    }
  return false;
  }

int32_t OgSimWaynet::callFilter(void* ctx, og_sim_point p) {
  auto& c = *reinterpret_cast<FilterCtx*>(ctx);
  const WayPoint* wp = nullptr;
  if(!fromPoint(*c.m, p, wp) || wp==nullptr) {
    Log::e("og-sim waynet mismatch: findFreePoint's filter got point (",p.kind,", ",p.index,
           "), not one of the current way net");
    c.bad = true;
    return 0;
    }
  try {
    return (*c.filter)(*wp) ? 1 : 0;
    }
  catch(...) {
    // Must not unwind through the Rust frames between here and findFreePoint.
    Log::e("og-sim waynet mismatch: findFreePoint's filter threw");
    c.bad = true;
    return 0;
    }
  }

void OgSimWaynet::loaded(World& world, const WayMatrix& m, const zenkit::VfsNode& zen) {
  current = &m;
  // The world loads either way: a failure here only costs the delegation (counted).
  try {
    load(world, m, zen);
    }
  catch(const std::exception& e) {
    Log::e("og-sim waynet mismatch: handing ",world.name()," over failed: ",e.what());
    counted("load");
    }
  catch(...) {
    Log::e("og-sim waynet mismatch: handing ",world.name()," over failed");
    counted("load");
    }
  }

void OgSimWaynet::load(World& world, const WayMatrix& m, const zenkit::VfsNode& zen) {
  auto rd = zen.open_read();
  rd->seek(0, zenkit::Whence::END);
  std::vector<uint8_t> bytes(rd->tell());
  rd->seek(0, zenkit::Whence::BEG);
  if(rd->read(bytes.data(), bytes.size())!=bytes.size())
    throw std::runtime_error("short read of the ZEN");

  // buildIndex's land rays, again in its order: physics is deterministic and nothing
  // moved since (the 5.12 dump relies on the same).
  std::vector<og_sim_land_ray> rays;
  rays.reserve(m.wayPoints.size()+m.freePoints.size()+m.startPoints.size());
  for(auto* v:{&m.wayPoints, &m.freePoints, &m.startPoints}) {
    for(auto& p:*v) {
      auto ray = world.physic()->landRay(p.position());
      rays.push_back({{ray.v.x, ray.v.y, ray.v.z}, ray.hasCol ? 1 : 0});
      }
    }
  // calculateLadderPoints' inputs, in its order.
  std::vector<og_sim_ladder> ladders;
  for(uint32_t i=0;; ++i) {
    auto inter = world.mobsiById(i);
    if(inter==nullptr)
      break;
    if(!inter->isLadder())
      continue;
    auto box = inter->bBox();
    ladders.push_back({i, {box[0].x, box[0].y, box[0].z, box[1].x, box[1].y, box[1].z}});
    }

  const std::string name(world.name());
  auto& t = OgSim::table();
  int32_t rc = t.waynet_load(t.user, name.c_str(), bytes.data(), bytes.size(),
                             uint32_t(m.wayPoints.size()), uint32_t(m.freePoints.size()),
                             uint32_t(m.startPoints.size()), rays.data(), ladders.data(),
                             uint32_t(ladders.size()));
  // Rust counted its own failure.
  if(rc!=0)
    Log::e("og-sim waynet: Rust couldn't load the way net of ",world.name()," (",rc,")");
  else
    Log::i("og-sim waynet: ",world.name()," loaded (",m.wayPoints.size()," way, ",m.freePoints.size(),
           " free, ",m.startPoints.size()," start points, ",ladders.size()," ladders)");
  }

void OgSimWaynet::unloaded(const WayMatrix& m) {
  if(current!=&m)
    return;
  current = nullptr;
  auto& t = OgSim::table();
  t.waynet_unload(t.user);
  }

WayPath OgSimWaynet::wayTo(const WayMatrix& m, const WayPoint** begin, size_t beginSz,
                           const Vec3& exactBegin, const WayPoint& end, WayPath cpp) {
  if(&m!=current) {
    Log::e("og-sim waynet mismatch: wayTo on a way net that isn't the current one");
    counted("wayTo");
    return cpp;
    }
  std::vector<og_sim_point> b(beginSz);
  og_sim_point e;
  bool ok = toPoint(m, &end, e);
  for(size_t i=0; i<beginSz; ++i)
    ok = toPoint(m, begin[i], b[i]) && ok;
  std::vector<og_sim_point> path(cpp.dat.size());
  // C++ holds the path end first; the table takes it in pop order.
  for(size_t i=0; i<cpp.dat.size(); ++i)
    ok = toPoint(m, cpp.dat[cpp.dat.size()-1-i], path[i]) && ok;
  if(!ok) {
    Log::e("og-sim waynet mismatch: wayTo with a point outside the current way net");
    counted("wayTo");
    return cpp;
    }
  // A path visits each way point at most once, plus a free-point end.
  std::vector<og_sim_point> out(m.wayPoints.size()+1);
  const float exact[3] = {exactBegin.x, exactBegin.y, exactBegin.z};
  auto& t = OgSim::table();
  uint32_t n = t.waynet_way_to(t.user, b.data(), uint32_t(b.size()), exact, e,
                               path.data(), uint32_t(path.size()), out.data(), uint32_t(out.size()));
  if(n==OG_SIM_KEEP_PATH)
    return cpp;
  if(n>out.size()) {
    Log::e("og-sim waynet mismatch: wayTo got ",n," points for room for ",out.size(),"; C++'s path is used");
    counted("wayTo");
    return cpp;
    }
  WayPath ret;
  for(uint32_t i=n; i>0; --i) {
    const WayPoint* p = nullptr;
    if(!fromPoint(m, out[i-1], p) || p==nullptr) {
      Log::e("og-sim waynet mismatch: wayTo got point (",out[i-1].kind,", ",out[i-1].index,
             "), not one of the current way net; C++'s path is used");
      counted("wayTo");
      return cpp;
      }
    ret.add(*p);
    }
  return ret;
  }

const WayPoint* OgSimWaynet::findFreePoint(const WayMatrix& m, const Vec3& at, std::string_view name,
                                           const std::function<bool(const WayPoint&)>& filter,
                                           const WayPoint* cpp) {
  og_sim_point c;
  if(&m!=current) {
    Log::e("og-sim waynet mismatch: findFreePoint on a way net that isn't the current one");
    counted("findFreePoint");
    return cpp;
    }
  if(!toPoint(m, cpp, c)) {
    Log::e("og-sim waynet mismatch: findFreePoint's C++ answer ",cpp->name," is not a point of the current way net");
    counted("findFreePoint");
    return cpp;
    }
  FilterCtx ctx = {&m, &filter, false};
  const float pos[3] = {at.x, at.y, at.z};
  auto& t = OgSim::table();
  og_sim_point r = t.waynet_find_free_point(t.user, pos, reinterpret_cast<const uint8_t*>(name.data()),
                                            uint32_t(name.size()), &ogSimWaynetFilter, &ctx, c);
  const WayPoint* ret = nullptr;
  if(ctx.bad || !fromPoint(m, r, ret)) {
    // A bad filter call was logged by callFilter; counted here, after Rust returned.
    Log::e("og-sim waynet mismatch: findFreePoint got point (",r.kind,", ",r.index,
           "); C++'s answer is used");
    counted("findFreePoint");
    return cpp;
    }
  return ret;
  }

void OgSimWaynet::lockChanged(const WayPoint& p, int32_t delta) {
  og_sim_point k;
  if(current==nullptr || !toPoint(*current, &p, k)) {
    Log::d("og-sim waynet: lock change ",delta," on ",p.name,", not a point of the current way net");
    return;
    }
  auto& t = OgSim::table();
  uint32_t n = t.waynet_lock(t.user, k, delta, p.useCount);
  if(OgSim::mode(OG_SIM_MOD_WAYNET)==OG_SIM_USE)
    p.useCount = n;
  }
