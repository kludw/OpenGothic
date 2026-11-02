#include "oghooks.h"

void (*OgHooks::onFrame)() = nullptr;
const DialogMenu* OgHooks::dialogMenu = nullptr;
uint64_t OgHooks::worldGeneration = 0;
