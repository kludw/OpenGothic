#pragma once

#include <cstdint>

class DialogMenu;

// opengothic-rs: engine-side hooks used by the C seam (game/capi/og_capi.h).
// Lives in the OpenGothic library so engine code never depends on opengothic_core.
namespace OgHooks {
  // Called once per rendered frame from MainWindow::render, also while loading or paused.
  extern void (*onFrame)();
  // The main window's dialog menu while the window exists (main thread only).
  extern const DialogMenu* dialogMenu;
  // Bumped when a world load starts (not on saves), so the seam can drop per-world ids.
  extern uint64_t worldGeneration;
  }
