#pragma once

// opengothic-rs: engine-side hooks used by the C seam (game/capi/og_capi.h).
// Lives in the OpenGothic library so engine code never depends on opengothic_core.
namespace OgHooks {
  // Called once per rendered frame from MainWindow::render, also while loading or paused.
  extern void (*onFrame)();
  }
