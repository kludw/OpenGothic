#include "ogsimroutine.h"

#include <Tempest/Log>

#include <string>
#include <vector>

#include "capi/ogsim.h"
#include "capi/ogsimwaynet.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/waymatrix.h"
#include "world/world.h"
#include "world/worldobjects.h"

using namespace Tempest;

namespace {
// Counts a mismatch this file found (already logged) on the Rust side.
void counted(const char* what) {
  auto& t = OgSim::table();
  t.routine_mismatch(t.user, what);
  }

uint64_t key(const void* p) {
  return uint64_t(reinterpret_cast<uintptr_t>(p));
  }

const uint8_t* bytes(std::string_view s) {
  return reinterpret_cast<const uint8_t*>(s.data());
  }
}

// The point in the NPC's world's matrix. A pointer the matrix doesn't hold can't be
// named to Rust: logged, counted, passed as UNMAPPED.
og_sim_point OgSimRoutine::pointOf(const World& world, const WayPoint* wp, const char* what) {
  og_sim_point p = {OG_SIM_POINT_NONE, 0};
  if(wp==nullptr)
    return p;
  if(world.wmatrix!=nullptr && OgSimWaynet::toPoint(*world.wmatrix, wp, p))
    return p;
  Log::e("og-sim routine: ", what, ": a way point outside its world's matrix");
  counted(what);
  return {OG_SIM_POINT_UNMAPPED, 0};
  }

void OgSimRoutine::added(const Npc& npc, gtime s, gtime e, uint32_t callback,
                         std::string_view name, const WayPoint* wp) {
  og_sim_routine r = {};
  r.start    = s.toInt();
  r.end      = e.toInt();
  r.callback = callback;
  r.point    = pointOf(npc.owner, wp, "routine add");
  r.name     = bytes(name);
  r.name_len = uint32_t(name.size());
  auto& t = OgSim::table();
  t.routine_npc(t.user, key(&npc), OG_SIM_ROUTINE_ADD, &r, 1);
  }

void OgSimRoutine::cleared(const Npc& npc) {
  auto& t = OgSim::table();
  t.routine_npc(t.user, key(&npc), OG_SIM_ROUTINE_CLEAR, nullptr, 0);
  }

void OgSimRoutine::loaded(const Npc& npc) {
  std::vector<og_sim_routine> rows(npc.routines.size());
  for(size_t i=0; i<rows.size(); ++i) {
    auto& src = npc.routines[i];
    auto& r   = rows[i];
    r.start    = src.start.toInt();
    r.end      = src.end.toInt();
    r.callback = src.callback.ptr;
    r.point    = pointOf(npc.owner, src.point, "routine load");
    r.name     = bytes(src.fallbackName);
    r.name_len = uint32_t(src.fallbackName.size());
    }
  auto& t = OgSim::table();
  t.routine_npc(t.user, key(&npc), OG_SIM_ROUTINE_LOAD, rows.data(), uint32_t(rows.size()));
  }

void OgSimRoutine::dropped(const Npc& npc) {
  auto& t = OgSim::table();
  t.routine_npc(t.user, key(&npc), OG_SIM_ROUTINE_DROP, nullptr, 0);
  }

int64_t OgSimRoutine::current(const Npc& npc, uint32_t op, gtime now, int64_t cpp) {
  og_sim_routine        row = {};
  const og_sim_routine* ptr = nullptr;
  if(cpp>=0) {
    auto& src = npc.routines[size_t(cpp)];
    row.start    = src.start.toInt();
    row.end      = src.end.toInt();
    row.callback = src.callback.ptr;
    row.point    = pointOf(npc.owner, src.point, "routine current");
    row.name     = bytes(src.fallbackName);
    row.name_len = uint32_t(src.fallbackName.size());
    ptr          = &row;
    }
  auto&   t = OgSim::table();
  int64_t i = t.routine_current(t.user, key(&npc), op, now.toInt(), cpp, ptr);
  const bool ok = (i>=0 && uint64_t(i)<npc.routines.size()) || i==OG_SIM_ROUTINE_EMPTY ||
                  (i==OG_SIM_ROUTINE_NPC_WP && op==OG_SIM_ROUTINE_TA_POINT);
  if(!ok) {
    Log::e("og-sim routine: Rust answered index ",i," for a table of ",npc.routines.size(),
           " (op ",op,"); C++'s ",cpp," is used");
    counted("routine current");
    return cpp;
    }
  return i;
  }

gtime OgSimRoutine::endTime(gtime start, gtime end, gtime now, gtime cpp) {
  auto& t = OgSim::table();
  return gtime::fromInt(t.routine_end_time(t.user, start.toInt(), end.toInt(), now.toInt(), cpp.toInt()));
  }

void OgSimRoutine::mobSet(const WorldObjects& w, gtime time, std::string_view scheme, int32_t state) {
  auto& t = OgSim::table();
  t.routine_mob_set(t.user, key(&w), time.toInt(), bytes(scheme), uint32_t(scheme.size()), state);
  }

void OgSimRoutine::mobLoaded(const WorldObjects& w) {
  std::vector<std::vector<og_sim_mob_routine>> rows;
  std::vector<og_sim_mob_states>               states;
  rows.reserve(w.routines.size());
  for(auto& s:w.routines) {
    std::vector<og_sim_mob_routine> r(s.routines.size());
    for(size_t i=0; i<r.size(); ++i)
      r[i] = {s.routines[i].time.toInt(), s.routines[i].state};
    rows.push_back(std::move(r));
    og_sim_mob_states st = {};
    st.scheme     = bytes(s.scheme);
    st.scheme_len = uint32_t(s.scheme.size());
    st.cur_state  = s.curState;
    st.routines   = rows.back().data();
    st.count      = uint32_t(rows.back().size());
    states.push_back(st);
    }
  auto& t = OgSim::table();
  t.routine_mob_load(t.user, key(&w), states.data(), uint32_t(states.size()));
  }

void OgSimRoutine::mobDropped(const WorldObjects& w) {
  auto& t = OgSim::table();
  t.routine_mob_drop(t.user, key(&w));
  }

void OgSimRoutine::mobTick(WorldObjects& w) {
  const gtime                    now = w.owner.time();
  std::vector<og_sim_mob_change> cpp;
  std::vector<const std::string*> schemes;
  uint32_t idx = 0;
  for(auto& i:w.routines) {
    schemes.push_back(&i.scheme);
    auto s = i.stateByTime(now);
    if(s!=i.curState) {
      cpp.push_back({idx, s});
      i.curState = s;
      }
    ++idx;
    }
  std::vector<og_sim_mob_change> out(schemes.size());
  auto& t = OgSim::table();
  uint32_t n = t.routine_mob_tick(t.user, key(&w), now.toInt(), cpp.data(), uint32_t(cpp.size()),
                                  out.data(), uint32_t(out.size()));
  if(OgSim::mode(OG_SIM_MOD_ROUTINE)!=OG_SIM_USE) {
    out = std::move(cpp);
    } else {
    bool ok = n<=out.size();
    for(uint32_t i=0; ok && i<n; ++i)
      ok = out[i].scheme<schemes.size();
    if(!ok) {
      Log::e("og-sim routine: Rust's mob changes name a scheme outside the table (",n,
             " changes, ",schemes.size()," schemes); C++'s are used");
      counted("routine mob tick");
      out = std::move(cpp);
      } else {
      out.resize(n);
      }
    }
  for(auto& c:out)
    w.setMobState(*schemes[c.scheme], c.state);
  }

void OgSimRoutine::mobReset(WorldObjects& w) {
  const gtime          now = w.owner.time();
  std::vector<int32_t> cpp;
  for(auto& i:w.routines) {
    i.curState = i.stateByTime(now);
    cpp.push_back(i.curState);
    }
  auto& t = OgSim::table();
  t.routine_mob_reset(t.user, key(&w), now.toInt(), cpp.data(), uint32_t(cpp.size()));
  for(auto& i:w.interactiveObj) {
    int32_t state = -1;
    for(auto& r:w.routines) {
      if(i->schemeName()==r.scheme)
        state = r.curState;
      }
    auto scheme = i->schemeName();
    state = t.routine_mob_scheme_state(t.user, key(&w), bytes(scheme), uint32_t(scheme.size()), state);
    i->resetPositionToTA(state);
    }
  }
