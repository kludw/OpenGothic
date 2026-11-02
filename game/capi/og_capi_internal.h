#pragma once

// opengothic-rs: C++-side access to the host's log sink (og_callbacks.on_log),
// for og_main's logger setup. Thread-safe.
namespace OgCapi {
  // True while og_run runs with an on_log callback.
  bool hasLogSink();
  // Forwards one line (level = OG_LOG_*); false if there is no sink (nothing done).
  bool log(int level, const char* source, const char* msg);
  }
