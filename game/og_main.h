#pragma once

// opengothic-rs: the former body of main(), callable from a host program.
// Call once per process, on the main thread (macOS Application). It installs
// process-wide log/crash handlers and may throw C++ exceptions: a non-C++ host
// must call it through a wrapper that catches them (C seam, task 1.4).
#ifdef __cplusplus
extern "C" {
#endif

int og_main(int argc, const char** argv);

#ifdef __cplusplus
}

class MainWindow;

// opengothic-rs (task 5.2): og_main in three parts, for a host that owns the loop.
// init does what og_main does before its event loop (same process-wide setup, same
// objects, same order) and may throw like og_main; shutdown destroys the objects in
// the order og_main's stack unwound. Main thread only.
namespace OgMain {
  struct Engine;
  Engine*     init(int argc, const char** argv);
  MainWindow& window(Engine& e);
  void        shutdown(Engine* e);
  }
#endif
