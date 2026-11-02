#pragma once

#include <cstdint>

class DialogMenu;
class MainWindow;
class PlayerControl;
class VideoWidget;

// opengothic-rs: engine-side hooks used by the C seam (game/capi/og_capi.h).
// Lives in the OpenGothic library so engine code never depends on opengothic_core.
namespace OgHooks {
  // Called once per rendered frame from MainWindow::render, also while loading or paused.
  extern void (*onFrame)();
  // The main window's widgets while the window exists (main thread only).
  extern DialogMenu*    dialogMenu;
  extern PlayerControl* playerControl;
  extern VideoWidget*   video;
  extern MainWindow*    mainWindow;
  // Bumped when a world load starts (not on saves), so the seam can drop per-world ids.
  extern uint64_t worldGeneration;
  }
