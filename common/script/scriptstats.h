#pragma once

#include "../../game/capi/og_script.h"

#include <atomic>
#include <chrono>
#include <cstdint>

// opengothic-rs (task 4.19): script time accounting behind og_script_stats_enable/take. Off by
// default: then every hook is one relaxed load. On, the outermost OgVm entry on each thread
// (call, init, opaque, allocate) is timed with steady_clock, nested entries (an external that
// calls back into a script) are inside it, and the og_script host functions count the Rust
// backend's crossings. Entries run on the main thread and the world loader thread, hence the
// atomics; the depth is per thread.
class ScriptStats final {
  public:
    static bool on() { return enabled.load(std::memory_order_relaxed); }
    static void enable(bool v);
    // One og_script_call_external: counted; the outermost per thread is timed (an external
    // may run a script that bridges again).
    class Bridge final {
      public:
        Bridge() {
          if(!on())
            return;
          counted = true;
          bridges.fetch_add(1, std::memory_order_relaxed);
          if(bridgeDepth++==0)
            start = std::chrono::steady_clock::now();
          }
        ~Bridge() {
          if(!counted || --bridgeDepth!=0)
            return;
          const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
          bridgeNanos.fetch_add(uint64_t(ns), std::memory_order_relaxed);
          }
        Bridge(const Bridge&) = delete;
        Bridge& operator=(const Bridge&) = delete;

      private:
        bool                                  counted = false;
        std::chrono::steady_clock::time_point start;
      };
    // og_script_member_get/set, og_script_global_get/set.
    static void accessor() { if(on()) accessors.fetch_add(1, std::memory_order_relaxed); }
    // The totals since the last take (or enable), then zero.
    static og_script_stats take();

    // One OgVm entry; only the outermost one on its thread counts.
    class Top final {
      public:
        Top() {
          if(!on())
            return;
          counted = true;
          if(depth++==0)
            start = std::chrono::steady_clock::now();
          }
        ~Top() {
          if(!counted || --depth!=0)
            return;
          const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
          calls.fetch_add(1, std::memory_order_relaxed);
          nanos.fetch_add(uint64_t(ns), std::memory_order_relaxed);
          }
        Top(const Top&) = delete;
        Top& operator=(const Top&) = delete;

      private:
        bool                                  counted = false;
        std::chrono::steady_clock::time_point start;
      };

  private:
    static inline std::atomic<bool>     enabled   {false};
    static inline std::atomic<uint64_t> calls     {0};
    static inline std::atomic<uint64_t> nanos     {0};
    static inline std::atomic<uint64_t> bridges   {0};
    static inline std::atomic<uint64_t> accessors {0};
    static inline std::atomic<uint64_t> bridgeNanos {0};
    static inline thread_local uint32_t depth     = 0;
    static inline thread_local uint32_t bridgeDepth = 0;
  };
