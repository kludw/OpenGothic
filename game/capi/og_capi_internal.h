#pragma once


#include <cstddef>
#include <cstdint>
#include <string_view>

// Per-frame context behind the opaque og_ctx of og_capi.h.
struct og_ctx {
  uint64_t frame   = 0;
  bool     inFrame = false;  // true only while on_frame runs
  };

// opengothic-rs: C++-side access to the host's log sink (og_callbacks.on_log),
// for og_main's logger setup. Thread-safe.
namespace OgCapi {
  // True while og_run runs with an on_log callback.
  bool hasLogSink();
  // Forwards one line (level = OG_LOG_*); false if there is no sink (nothing done).
  bool log(int level, const char* source, const char* msg);
  }

// FNV-1a 64: stable within a process, enough to notice a change between two reads
// (og_read_watch, og_read_world_trace).
struct OgFnv {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) {
    auto b = static_cast<const uint8_t*>(p);
    for(size_t i=0; i<n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
      }
    }
  void text(std::string_view s) { bytes(s.data(),s.size()); uint8_t z=0; bytes(&z,1); }
  void u64 (uint64_t v)         { bytes(&v,sizeof(v)); }
  };
