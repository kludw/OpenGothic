#include "phystrace.h"

#include "../../game/capi/og_phys_trace.h"

#include <Tempest/Log>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

using namespace Tempest;

namespace {

struct State {
  std::mutex                                       sync;
  std::FILE*                                       file = nullptr;
  std::string                                      path;
  // Dropped records (write failures), reported once at close.
  uint64_t                                         failed = 0;
  uint32_t                                         nextWorld = 0;
  uint32_t                                         nextObject = 0;
  uint32_t                                         nextShape = 0;
  std::unordered_map<const void*,uint32_t>         worlds;
  std::unordered_map<const void*,PhysTrace::Object> objects;
  struct Shape {
    uint32_t id   = 0;
    uint64_t hash = 0;
    };
  std::unordered_map<const void*,Shape>            shapes;
  uint32_t                                         nextPayload = 0;
  std::unordered_map<const void*,uint32_t>         payloads;
  };

State&            state() {
  static State s;
  return s;
  }

std::atomic<bool> isActive{false};
thread_local int  depth = 0;

// Large writes: a world load adds tens of thousands of objects, NPCs query every tick.
constexpr size_t  BufferSize = 4u << 20;

void writeAll(State& s, const void* p, size_t n) {
  if(std::fwrite(p,1,n,s.file)!=n)
    s.failed++;
  }

}

void PhysTrace::open(const std::string& path) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file!=nullptr)
    throw std::runtime_error("physics trace: already open (" + s.path + ")");
  std::FILE* f = std::fopen(path.c_str(),"wb");
  if(f==nullptr)
    throw std::runtime_error("physics trace: can't open \"" + path + "\": " + std::strerror(errno));
  std::setvbuf(f,nullptr,_IOFBF,BufferSize);
  s.file = f;
  s.path = path;
  const uint32_t version = OG_PT_VERSION;
  writeAll(s,OG_PT_MAGIC,4);
  writeAll(s,&version,sizeof(version));
  if(s.failed>0) {
    std::fclose(f);
    s.file = nullptr;
    throw std::runtime_error("physics trace: can't write header to \"" + path + "\"");
    }
  isActive.store(true);
  Log::i("physics trace: opened \"", path, "\"");
  }

void PhysTrace::close() {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file==nullptr)
    return;
  isActive.store(false);
  if(std::fclose(s.file)!=0)
    s.failed++;
  s.file = nullptr;
  if(s.failed>0)
    Log::e("physics trace: ", s.failed, " writes to \"", s.path, "\" failed; the trace is incomplete");
  else
    Log::i("physics trace: closed \"", s.path, "\"");
  }

bool PhysTrace::active() {
  return isActive.load(std::memory_order_relaxed);
  }

PhysTrace::Call::Call() {
  if(!active())
    return;
  counted = true;
  outer   = (depth==0);
  depth++;
  if(outer)
    start = std::chrono::steady_clock::now();
  }

PhysTrace::Call::~Call() {
  if(counted)
    depth--;
  }

uint64_t PhysTrace::Call::ns() const {
  auto d = std::chrono::steady_clock::now() - start;
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
  }

PhysTrace::Callback::Callback(uint32_t world, uint8_t kind):world(world) {
  if(!active())
    return;
  counted = true;
  saved   = depth;
  if(depth>0) {
    marked = true;
    Rec(OG_PT_CALLBACK_BEGIN,world).u8(kind);
    }
  depth = 0;
  }

PhysTrace::Callback::~Callback() {
  if(!counted)
    return;
  depth = saved;
  if(marked)
    Rec r(OG_PT_CALLBACK_END,world);
  }

PhysTrace::Rec::Rec(uint8_t kind, uint32_t world):kind(kind) {
  body.reserve(128);
  u32(world);
  }

PhysTrace::Rec::~Rec() {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  if(s.file==nullptr)
    return;
  if(broken) {
    s.failed++;
    return;
    }
  const uint32_t len = uint32_t(body.size());
  writeAll(s,&kind,1);
  writeAll(s,&len,sizeof(len));
  writeAll(s,body.data(),body.size());
  }

PhysTrace::Rec& PhysTrace::Rec::u8(uint8_t v) {
  body.push_back(v);
  return *this;
  }

PhysTrace::Rec& PhysTrace::Rec::u16(uint16_t v) {
  auto p = reinterpret_cast<const uint8_t*>(&v);
  body.insert(body.end(),p,p+sizeof(v));
  return *this;
  }

PhysTrace::Rec& PhysTrace::Rec::u32(uint32_t v) {
  auto p = reinterpret_cast<const uint8_t*>(&v);
  body.insert(body.end(),p,p+sizeof(v));
  return *this;
  }

PhysTrace::Rec& PhysTrace::Rec::i32(int32_t v) {
  return u32(uint32_t(v));
  }

PhysTrace::Rec& PhysTrace::Rec::u64(uint64_t v) {
  auto p = reinterpret_cast<const uint8_t*>(&v);
  body.insert(body.end(),p,p+sizeof(v));
  return *this;
  }

PhysTrace::Rec& PhysTrace::Rec::f32(float v) {
  uint32_t b = 0;
  std::memcpy(&b,&v,sizeof(b));
  return u32(b);
  }

PhysTrace::Rec& PhysTrace::Rec::vec3(const Tempest::Vec3& v) {
  return f32(v.x).f32(v.y).f32(v.z);
  }

PhysTrace::Rec& PhysTrace::Rec::mat4(const Tempest::Matrix4x4& m) {
  for(int x=0; x<4; ++x)
    for(int y=0; y<4; ++y)
      f32(m.at(x,y));
  return *this;
  }

PhysTrace::Rec& PhysTrace::Rec::str(const char* s) {
  if(s==nullptr)
    return u16(OG_PT_STR_NULL);
  return str(std::string_view(s));
  }

PhysTrace::Rec& PhysTrace::Rec::str(std::string_view s) {
  if(s.size()>=OG_PT_STR_NULL) {
    // Never throw from a hook: the record is dropped and close() reports the failure.
    Log::e("physics trace: string of ", s.size(), " bytes in a kind ", int(kind), " record; record dropped");
    broken = true;
    return *this;
    }
  u16(uint16_t(s.size()));
  body.insert(body.end(),s.begin(),s.end());
  return *this;
  }

uint32_t PhysTrace::addWorld(const void* world) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  uint32_t id = ++s.nextWorld;
  s.worlds[world] = id;
  return id;
  }

uint32_t PhysTrace::world(const void* world) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  auto i = s.worlds.find(world);
  if(i==s.worlds.end()) {
    Log::e("physics trace: world ", world, " was never added");
    return 0;
    }
  return i->second;
  }

void PhysTrace::removeWorld(const void* world) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  s.worlds.erase(world);
  }

PhysTrace::Object PhysTrace::addObject(const void* obj, uint32_t world) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  Object o{world, ++s.nextObject};
  s.objects[obj] = o;
  return o;
  }

PhysTrace::Object PhysTrace::object(const void* obj) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  auto i = s.objects.find(obj);
  if(i==s.objects.end()) {
    Log::e("physics trace: object ", obj, " was never added");
    return Object{};
    }
  return i->second;
  }

PhysTrace::Object PhysTrace::removeObject(const void* obj) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  auto i = s.objects.find(obj);
  if(i==s.objects.end()) {
    Log::e("physics trace: deleted object ", obj, " was never added");
    return Object{};
    }
  auto o = i->second;
  s.objects.erase(i);
  return o;
  }

uint32_t PhysTrace::shape(const void* shp, uint64_t hash, bool& first) {
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  auto i = s.shapes.find(shp);
  if(i!=s.shapes.end() && i->second.hash==hash) {
    first = false;
    return i->second.id;
    }
  first = true;
  State::Shape sh{++s.nextShape, hash};
  s.shapes[shp] = sh;
  return sh.id;
  }

uint32_t PhysTrace::payload(const void* p) {
  if(p==nullptr)
    return 0;
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.sync);
  auto i = s.payloads.find(p);
  if(i!=s.payloads.end())
    return i->second;
  uint32_t id = ++s.nextPayload;
  s.payloads[p] = id;
  return id;
  }
