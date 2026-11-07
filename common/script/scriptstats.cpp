#include "scriptstats.h"

void ScriptStats::enable(bool v) {
  take();
  enabled.store(v, std::memory_order_relaxed);
  }

og_script_stats ScriptStats::take() {
  og_script_stats s = {};
  s.calls     = calls    .exchange(0, std::memory_order_relaxed);
  s.ns        = nanos    .exchange(0, std::memory_order_relaxed);
  s.bridge    = bridges  .exchange(0, std::memory_order_relaxed);
  s.accessors = accessors.exchange(0, std::memory_order_relaxed);
  return s;
  }

extern "C" void og_script_stats_enable(int32_t on) {
  ScriptStats::enable(on!=0);
  }

extern "C" void og_script_stats_take(og_script_stats* out) {
  if(out!=nullptr)
    *out = ScriptStats::take();
  }
