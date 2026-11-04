#include "ogwaynet.h"

#include <Tempest/Log>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "world/objects/interactive.h"
#include "world/waymatrix.h"
#include "world/world.h"

using namespace Tempest;

namespace {

[[noreturn]] void fail(const std::string& what) {
  Log::e("waynet dump: ", what);
  throw std::runtime_error("waynet dump: " + what);
  }

uint32_t bits(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  return u;
  }

float fromBits(const std::string& hex) {
  if(hex.size()!=8)
    fail("float \"" + hex + "\" is not 8 hex digits");
  uint32_t u = uint32_t(std::stoul(hex, nullptr, 16));
  float f = 0;
  std::memcpy(&f, &u, 4);
  return f;
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

std::string unhexName(const std::string& hex) {
  if(hex=="-")
    return "";
  if(hex.size()%2!=0)
    fail("name \"" + hex + "\" has an odd number of hex digits");
  std::string ret;
  for(size_t i=0; i<hex.size(); i+=2)
    ret += char(std::stoul(hex.substr(i, 2), nullptr, 16));
  return ret;
  }

struct Writer {
  std::ofstream out;
  char          buf[64];

  Writer& vec(const Vec3& v) {
    std::snprintf(buf, sizeof(buf), " %08" PRIx32 " %08" PRIx32 " %08" PRIx32, bits(v.x), bits(v.y), bits(v.z));
    out << buf;
    return *this;
    }
  };

}

// Friend of WayMatrix: reads its private vectors and owner.
void OgWaynetDump::write(World& world, const WayMatrix& m, std::string_view dumpPath, std::string_view queriesPath) {
  Writer w;
  w.out.open(std::string(dumpPath), std::ios::binary | std::ios::trunc);
  if(!w.out)
    fail("can't write " + std::string(dumpPath));

  // startPoint/deadPoint's function-local statics, when they fall back to them.
  const WayPoint* start = &m.startPoint();
  const WayPoint* dead  = &m.deadPoint();
  auto ref = [&](const WayPoint* p) -> std::string {
    if(p==nullptr)
      return "-";
    const std::vector<WayPoint>* sets[3] = {&m.wayPoints, &m.freePoints, &m.startPoints};
    const char kinds[3] = {'w', 'f', 's'};
    std::less<const WayPoint*> lt;
    for(int k=0; k<3; ++k) {
      auto& v = *sets[k];
      if(!v.empty() && !lt(p, v.data()) && lt(p, v.data()+v.size()))
        return kinds[k] + std::to_string(p - v.data());
      }
    if(p==start || p==dead)
      return "S";
    fail("a way point outside the way matrix");
    };

  w.out << "waynet 1\n";
  w.out << "world " << hexName(world.name()) << "\n";
  auto points = [&](char kind, const std::vector<WayPoint>& v) {
    for(size_t i=0; i<v.size(); ++i) {
      auto& p   = v[i];
      // adjustWaypoints' query, again: physics is deterministic and nothing moved since.
      auto  ray = world.physic()->landRay(p.position());
      w.out << kind << " " << i << " " << hexName(p.name);
      w.vec(p.pos).vec(p.dir);
      w.out << " " << int(p.underWater) << " " << int(p.freePoint) << " " << int(ray.hasCol);
      w.vec(ray.v).vec(p.groundPos);
      w.out << " " << (p.ladder==nullptr ? int64_t(-1) : int64_t(world.mobsiId(p.ladder))) << "\n";
      }
    };
  points('w', m.wayPoints);
  points('f', m.freePoints);
  points('s', m.startPoints);

  for(auto& e:m.edges)
    w.out << "e " << e.a << " " << e.b << "\n";
  for(size_t i=0; i<m.wayPoints.size(); ++i) {
    auto& conn = m.wayPoints[i].connections();
    w.out << "c " << i << " " << conn.size();
    for(auto& c:conn)
      w.out << " " << ref(c.point) << " " << c.len;
    w.out << "\n";
    }
  w.out << "idx";
  for(auto p:m.indexPoints)
    w.out << " " << ref(p);
  w.out << "\nfpind";
  for(auto p:m.fpInd)
    w.out << " " << ref(p);
  w.out << "\n";

  // calculateLadderPoints' inputs, in its order.
  for(uint32_t i=0;; ++i) {
    auto inter = world.mobsiById(i);
    if(inter==nullptr)
      break;
    if(!inter->isLadder())
      continue;
    auto box = inter->bBox();
    w.out << "ladder " << i;
    w.vec(box[0]).vec(box[1]);
    w.out << "\n";
    }

  w.out << "start " << ref(start) << "\n";
  w.out << "dead " << ref(dead) << "\n";

  size_t queries = 0;
  if(!queriesPath.empty()) {
    std::ifstream in{std::string(queriesPath)};
    if(!in)
      fail("can't read " + std::string(queriesPath));
    std::string line;
    while(std::getline(in, line)) {
      std::istringstream ls(line);
      std::string op, a, b, c, d;
      ls >> op >> a >> b >> c >> d;
      auto pos = [&]() { return Vec3(fromBits(a), fromBits(b), fromBits(c)); };
      const WayPoint* r = nullptr;
      if(op=="wp" && !c.empty())
        r = world.findWayPoint(pos());
      else if(op=="wpn" && !d.empty())
        r = world.findWayPoint(pos(), unhexName(d));
      else if(op=="fp" && !d.empty())
        r = world.findFreePoint(pos(), unhexName(d));
      else if(op=="next" && !c.empty())
        r = m.findNextPoint(pos());
      else if(op=="point" && (b=="0" || b=="1"))
        r = world.findPoint(unhexName(a), b=="1");
      else if(op=="wpname" && !a.empty())
        r = world.findWayPoint(unhexName(a));
      else
        fail("bad query line " + std::to_string(queries+1) + ": \"" + line + "\"");
      w.out << "q " << ref(r) << "\n";
      ++queries;
      }
    }
  w.out << "end\n";
  w.out.flush();
  if(!w.out)
    fail("write to " + std::string(dumpPath) + " failed");
  Log::i("waynet dump: ", world.name(), ": ", m.wayPoints.size(), " way points, ", m.freePoints.size(),
         " free points, ", m.startPoints.size(), " start points, ", queries, " queries -> ", dumpPath);
  }
