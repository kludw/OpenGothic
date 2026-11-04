#include "ogroutine.h"

#include <Tempest/Log>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "world/worldobjects.h"
#include "commandline.h"

using namespace Tempest;

namespace {

[[noreturn]] void fail(const std::string& what) {
  Log::e("routine dump: ", what);
  throw std::runtime_error("routine dump: " + what);
  }

std::string hexName(std::string_view s) {
  if(s.empty())
    return "-";
  std::string ret;
  char buf[3];
  for(char c:s) {
    std::snprintf(buf, sizeof(buf), "%02x", unsigned(uint8_t(c)));
    ret += buf;
    }
  return ret;
  }

std::string pointName(const WayPoint* wp) {
  // A resolved point always has a name in practice; an empty one would read as "none",
  // so refuse it instead of writing an ambiguous line.
  if(wp==nullptr)
    return "-";
  if(wp->name.empty())
    fail("a resolved way point has an empty name");
  return hexName(wp->name);
  }

struct State {
  std::ofstream                           out;
  bool                                    opened = false;
  bool                                    done   = false;
  std::unordered_map<const Npc*,uint32_t> ids;

  // nullptr when the flag is off or the snapshot was written.
  std::ofstream* file() {
    if(done)
      return nullptr;
    if(!opened) {
      auto path = CommandLine::inst().routineDump();
      if(path.empty()) {
        done = true;
        return nullptr;
        }
      out.open(std::string(path), std::ios::binary | std::ios::trunc);
      if(!out)
        fail("can't write " + std::string(path));
      out << "routine 1\n";
      opened = true;
      }
    return &out;
    }

  uint32_t id(const Npc& npc) {
    auto it = ids.find(&npc);
    if(it!=ids.end())
      return it->second;
    uint32_t n = uint32_t(ids.size());
    ids.emplace(&npc, n);
    return n;
    }
  };

State& state() {
  static State s;
  return s;
  }

constexpr int64_t dayMs = 24*60*60*1000;

// The world times every NPC and mob table is asked about: spread over three days,
// minutes and seconds, plus the snapshot time itself.
std::vector<int64_t> commonTimes(gtime now) {
  std::vector<int64_t> t;
  for(int64_t k=0; k<200; ++k)
    t.push_back((k%3)*dayMs + ((k*37)%1440)*60*1000 + ((k*7)%60)*1000);
  t.push_back(now.toInt());
  return t;
  }

// Each boundary, a millisecond before it, and the same a day later.
void addBoundary(std::vector<int64_t>& t, gtime b) {
  for(int64_t d:{int64_t(0), dayMs}) {
    if(b.toInt()+d>=0)
      t.push_back(b.toInt()+d);
    if(b.toInt()+d-1>=0)
      t.push_back(b.toInt()+d-1);
    }
  }

}

void OgRoutineDump::added(const Npc& npc, gtime s, gtime e, uint32_t callback,
                          std::string_view point, const WayPoint* wp) {
  auto& st = state();
  auto* f  = st.file();
  if(f==nullptr)
    return;
  *f << "add " << st.id(npc) << ' ' << s.toInt() << ' ' << e.toInt() << ' ' << callback
     << ' ' << hexName(point) << ' ' << pointName(wp) << '\n';
  }

void OgRoutineDump::exchanged(const Npc& npc, size_t callback) {
  auto& st = state();
  auto* f  = st.file();
  if(f==nullptr)
    return;
  *f << "exc " << st.id(npc) << ' ' << uint64_t(callback) << '\n';
  }

void OgRoutineDump::mobRoutine(gtime time, std::string_view scheme, int32_t state_) {
  auto& st = state();
  auto* f  = st.file();
  if(f==nullptr)
    return;
  *f << "mob " << time.toInt() << ' ' << hexName(scheme) << ' ' << state_ << '\n';
  }

void OgRoutineDump::snapshot(World& world) {
  auto& st = state();
  auto* f  = st.file();
  if(f==nullptr)
    return;

  const gtime now = world.time();
  *f << "snapshot " << hexName(world.name()) << ' ' << now.toInt() << '\n';

  auto dumpNpc = [&](const Npc& npc, int list) {
    *f << "npc " << st.id(npc) << ' ' << list << ' ' << npc.hnpc->symbol_index()
       << ' ' << hexName(npc.hnpc->wp) << ' ' << pointName(world.findPoint(npc.hnpc->wp,false)) << '\n';
    for(auto& r:npc.routines)
      *f << "r " << r.start.toInt() << ' ' << r.end.toInt() << ' ' << uint64_t(r.callback.ptr)
         << ' ' << pointName(r.point) << ' ' << hexName(r.fallbackName) << '\n';

    auto times = commonTimes(now);
    for(auto& r:npc.routines) {
      addBoundary(times, r.start);
      addBoundary(times, r.end);
      }
    auto index = [&](const Npc::Routine& r) -> int64_t {
      if(npc.routines.empty() || &r<npc.routines.data() || &r>=npc.routines.data()+npc.routines.size())
        return -1; // the static empty routine
      return int64_t(&r - npc.routines.data());
      };
    for(int64_t t:times) {
      const gtime wt = gtime::fromInt(t);
      auto& r0 = npc.currentRoutineAt(wt,false);
      auto& r1 = npc.currentRoutineAt(wt,true);
      *f << "q " << t << ' ' << index(r0) << ' ' << index(r1) << ' ' << npc.endTimeAt(wt,r0).toInt()
         << ' ' << pointName(npc.currentTaPointAt(wt)) << '\n';
      }
    };
  for(auto& n:world.wobj.npcArr)
    dumpNpc(*n, 0);
  for(auto& n:world.wobj.npcInvalid)
    dumpNpc(*n, 1);

  for(auto& m:world.wobj.routines) {
    *f << "scheme " << hexName(m.scheme) << ' ' << m.curState << '\n';
    auto times = commonTimes(now);
    for(auto& r:m.routines) {
      *f << "m " << r.time.toInt() << ' ' << r.state << '\n';
      addBoundary(times, r.time);
      }
    for(int64_t t:times)
      *f << "mq " << t << ' ' << m.stateByTime(gtime::fromInt(t)) << '\n';
    }

  *f << "end\n";
  f->flush();
  if(!*f)
    fail("writing " + std::string(CommandLine::inst().routineDump()) + " failed");
  f->close();
  st.done = true;
  Log::i("routine dump: snapshot of ", world.wobj.npcArr.size(), "+", world.wobj.npcInvalid.size(),
         " npcs and ", world.wobj.routines.size(), " mob schemes written");
  }
