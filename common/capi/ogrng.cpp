#include "ogrng.h"

#include <Tempest/Log>

#include <cstdio>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/serialize.h"

namespace {

// Bump when the save entry's layout changes; load refuses other versions.
constexpr uint32_t EntryVersion = 1;

constexpr const char* Names[OgRng::Count] = {"gameplay","anim","sound","visual"};

uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
  }

struct StreamState {
  uint64_t     seed = 0;  // keyed draws derive from this; the engine is seeded from its low half
  std::mt19937 engine;
  OgRng::Info  info;
  };

struct State {
  std::mutex  sync;
  bool        seeded = false;
  StreamState streams[OgRng::Count];
  };

State& state() {
  static State st;
  return st;
  }

// Holds the lock; the log lines are written after unlocking, so a log sink that reads the
// streams (og_rng_read from on_log) can't deadlock on the non-recursive mutex.
struct Locked {
  State&                       st;
  std::unique_lock<std::mutex> lock;
  // One line each: Tempest splits lines longer than ~255 bytes.
  std::vector<std::string>     logLines;

  Locked() : st(state()), lock(st.sync) {}
  ~Locked() {
    lock.unlock();
    for(auto& line:logLines)
      Tempest::Log::i(line);
    }
  };

std::string engineText(const std::mt19937& e) {
  std::ostringstream out;
  out << e;
  return out.str();
  }

uint64_t stateHash(const std::mt19937& e) {
  const auto text = engineText(e);
  return OgRng::hash(OgRng::HashStart,text.data(),text.size());
  }

void seedLocked(Locked& l, uint64_t seed, const char* source) {
  auto& st = l.st;
  for(uint32_t i=0; i<OgRng::Count; ++i) {
    auto& s = st.streams[i];
    // Stream i's seed: an independent 64-bit value per stream (Rust's og_sim::rng
    // reproduces this: splitmix64(seed + i), mt19937 seeded with its low 32 bits).
    s.seed   = splitmix64(seed + i);
    s.engine = std::mt19937(uint32_t(s.seed));
    s.info   = OgRng::Info();
    s.info.digest = OgRng::HashStart;
    }
  st.seeded = true;
  l.logLines.push_back("ogrng: seed "+std::to_string(seed)+" ("+source+")");
  }

void ensureSeeded(Locked& l) {
  if(l.st.seeded)
    return;
  std::random_device dev;
  const uint64_t seed = (uint64_t(dev()) << 32) | uint64_t(dev());
  seedLocked(l,seed,"random_device, no host seed");
  }

// One line per stream: draws, seed, digest, generator state and keyed counters, all
// that a load restores.
void summaryLocked(Locked& l, const char* prefix) {
  for(uint32_t i=0; i<OgRng::Count; ++i) {
    auto& st = l.st;
    auto& s = st.streams[i];
    char buf[160] = {};
    std::snprintf(buf,sizeof(buf),"%s%s=%llu:%016llx:%016llx:%016llx:%llu:%016llx",prefix,Names[i],
                  static_cast<unsigned long long>(s.info.draws),
                  static_cast<unsigned long long>(s.seed),
                  static_cast<unsigned long long>(s.info.digest),
                  static_cast<unsigned long long>(stateHash(s.engine)),
                  static_cast<unsigned long long>(s.info.keyedDraws),
                  static_cast<unsigned long long>(s.info.keyedSum));
    l.logLines.push_back(buf);
    }
  }

}

uint64_t OgRng::hash(uint64_t h, const void* data, uint64_t size) {
  auto p = static_cast<const uint8_t*>(data);
  for(uint64_t i=0; i<size; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
    }
  return h;
  }

void OgRng::seed(uint64_t seed) {
  Locked l;
  seedLocked(l,seed,"host");
  }

uint32_t OgRng::next(Stream s) {
  Locked l;
  ensureSeeded(l);
  auto& st = l.st;
  auto&          ss = st.streams[s];
  const uint32_t v  = uint32_t(ss.engine());
  ss.info.draws++;
  ss.info.digest = hash(ss.info.digest,&v,sizeof(v));
  return v;
  }

uint32_t OgRng::keyed(Stream s, uint64_t key) {
  Locked l;
  ensureSeeded(l);
  auto& st = l.st;
  auto&          ss = st.streams[s];
  const uint32_t v  = uint32_t(splitmix64(ss.seed ^ splitmix64(key)) >> 32);
  ss.info.keyedDraws++;
  ss.info.keyedSum += v;
  return v;
  }

OgRng::Info OgRng::info(Stream s) {
  Locked l;
  ensureSeeded(l);
  auto& st = l.st;
  auto ret = st.streams[s].info;
  ret.stateHash = stateHash(st.streams[s].engine);
  return ret;
  }

const char* OgRng::name(Stream s) {
  return s<Count ? Names[s] : "?";
  }

void OgRng::save(Serialize& fout) {
  Locked l;
  ensureSeeded(l);
  auto& st = l.st;
  fout.write(EntryVersion,uint32_t(Count));
  for(uint32_t i=0; i<Count; ++i) {
    auto& s = st.streams[i];
    fout.write(std::string(Names[i]),s.seed,s.info.draws,s.info.keyedDraws,s.info.digest,s.info.keyedSum,
               engineText(s.engine));
    }
  // Tests compare this line with the one load writes (apps/gothic/tests/rng.rs).
  summaryLocked(l,"ogrng: saved ");
  }

void OgRng::load(Serialize& fin) {
  uint32_t version = 0, count = 0;
  fin.read(version,count);
  if(version!=EntryVersion || count!=Count)
    throw std::runtime_error("ogrng: unsupported save entry opengothic-rs/rng (version "+std::to_string(version)+
                             ", "+std::to_string(count)+" streams)");
  StreamState loaded[Count];
  for(uint32_t i=0; i<Count; ++i) {
    auto&       s = loaded[i];
    std::string streamName, text;
    fin.read(streamName,s.seed,s.info.draws,s.info.keyedDraws,s.info.digest,s.info.keyedSum,text);
    if(streamName!=Names[i])
      throw std::runtime_error("ogrng: save entry has stream '"+streamName+"' where '"+Names[i]+"' belongs");
    std::istringstream in(text);
    in >> s.engine;
    if(in.fail())
      throw std::runtime_error(std::string("ogrng: unreadable generator state for stream ")+Names[i]);
    }

  Locked l;
  for(uint32_t i=0; i<Count; ++i)
    l.st.streams[i] = loaded[i];
  l.st.seeded = true;
  summaryLocked(l,"ogrng: loaded ");
  }
