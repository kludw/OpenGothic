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
#endif
