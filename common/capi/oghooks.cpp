#include "oghooks.h"

void (*OgHooks::onFrame)() = nullptr;
DialogMenu*    OgHooks::dialogMenu    = nullptr;
PlayerControl* OgHooks::playerControl = nullptr;
VideoWidget*   OgHooks::video         = nullptr;
uint64_t OgHooks::worldGeneration = 0;
