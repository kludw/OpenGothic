// opengothic-rs (focus-fix): og_set_background off macOS; the macOS version is ogbackground.mm.
#if !defined(__APPLE__)
#include "../../game/capi/og_capi.h"

extern "C" int og_set_background(int) { return 0; }
#endif
