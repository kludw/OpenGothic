#pragma once

#include <cstdint>

class DialogMenu;
class DocumentMenu;
class InventoryMenu;
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
  extern DocumentMenu*  documentMenu;
  extern InventoryMenu* inventoryMenu;
  extern PlayerControl* playerControl;
  extern VideoWidget*   video;
  extern MainWindow*    mainWindow;
  // Bumped when a world load starts (not on saves), so the seam can drop per-world ids.
  extern uint64_t worldGeneration;
  // Bumped when a save ends in LoadState::FailedSave (Gothic::finishLoading), the only
  // trace a failed save leaves once the state is back to Idle.
  extern uint64_t saveFailures;
  // Seam attack/flee (task 2.11). hostDrivesPlayer: PlayerControl leaves the player's
  // movement alone. hostFightsForPlayer: the NPC fight AI (Npc::implAttack) drives the
  // player, with playerFightTactic if the hero has none of its own (fight_tactic 0).
  extern bool    hostDrivesPlayer;
  // A seam action walks the player through the AI queue (go_to, pick_up, talk_to, ...):
  // MoveAlgo then gives the player the NPCs' bounce off obstacles (task 5.3, UB-31).
  extern bool    hostWalksPlayer;
  extern bool    hostFightsForPlayer;
  extern int32_t playerFightTactic;
  // Exact replay (task 5.6). onPlayerInput: called by PlayerControl for every key press
  // (kind 1), release (2) and mouse rotation (3) it receives. inputReplay: the window
  // drops real keyboard and mouse input.
  extern void  (*onPlayerInput)(int kind, int action, int key, int mapping, float dx, float dy);
  extern bool    inputReplay;
  }
