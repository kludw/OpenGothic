#pragma once

#include <Tempest/Matrix4x4>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// opengothic-rs (task 6.2): the `--physics-trace <path>` recorder. Format and recording
// rules: game/capi/og_phys_trace.h. Inactive (nothing opened, every hook a no-op) unless
// open() was called; the game calls it once at start-up, before any world exists.
namespace PhysTrace {
  // Opens (truncates) the trace and writes its header. Throws std::runtime_error when
  // the file can't be written, or when a trace is already open.
  void open(const std::string& path);
  // Flushes and closes the trace; later records are dropped. No-op when inactive.
  void close();
  bool active();

  // Marks one public DynamicWorld call. Calls nest (landRay calls ray, tryMove calls
  // setPosition); only the outermost call on a thread is recorded.
  class Call final {
    public:
      Call();
      ~Call();
      Call(const Call&)=delete;
      Call& operator=(const Call&)=delete;

      // True when this call should write its record (trace active, outermost call).
      bool     record() const { return outer; }
      // Wall time since construction.
      uint64_t ns() const;

    private:
      bool                                  counted = false;
      bool                                  outer   = false;
      std::chrono::steady_clock::time_point start;
    };

  // Game code called back from inside a physics call (bullet/bbox callbacks, the item hit
  // sound): its own physics calls are outermost again, bracketed by CALLBACK_BEGIN/END
  // when the call that runs the callback is itself traced.
  class Callback final {
    public:
      Callback(uint32_t world, uint8_t kind);
      ~Callback();
      Callback(const Callback&)=delete;
      Callback& operator=(const Callback&)=delete;

    private:
      int      saved  = 0;
      bool     marked = false;
      bool     counted = false;
      uint32_t world  = 0;
    };

  // One record: the body is built in memory and written whole by the destructor; a body
  // that couldn't be encoded is dropped and counted as a failed write.
  class Rec final {
    public:
      Rec(uint8_t kind, uint32_t world);
      ~Rec();
      Rec(const Rec&)=delete;
      Rec& operator=(const Rec&)=delete;

      Rec& u8 (uint8_t v);
      Rec& u16(uint16_t v);
      Rec& u32(uint32_t v);
      Rec& i32(int32_t v);
      Rec& u64(uint64_t v);
      Rec& f32(float v);
      Rec& vec3(const Tempest::Vec3& v);
      Rec& mat4(const Tempest::Matrix4x4& m);
      Rec& str(const char* s);
      Rec& str(std::string_view s);

    private:
      uint8_t              kind = 0;
      bool                 broken = false;
      std::vector<uint8_t> body;
    };

  // Trace numbers of worlds and objects, keyed by the engine's pointers (never
  // dereferenced). addWorld/addObject hand out the next number; a pointer the engine
  // reuses after a delete gets a fresh one.
  uint32_t addWorld(const void* world);
  uint32_t world(const void* world);
  void     removeWorld(const void* world);

  struct Object {
    uint32_t world = 0;
    uint32_t id    = 0;
    };
  Object   addObject(const void* obj, uint32_t world);
  // The object's numbers; {0,0} (logged as an error) if the pointer was never added.
  Object   object(const void* obj);
  // As object(), and forgets the pointer.
  Object   removeObject(const void* obj);

  // Trace number of a collision shape: the same for the same pointer and hash (shapes are
  // cached for the process); `first` tells the caller to write the SHAPE record.
  uint32_t shape(const void* shp, uint64_t hash, bool& first);
  // Trace number of what a physics object points at (0 for null), see og_phys_trace.h.
  uint32_t payload(const void* p);
  }
