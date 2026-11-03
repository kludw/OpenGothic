#include "oghooks.h"

void (*OgHooks::onFrame)() = nullptr;
DialogMenu*    OgHooks::dialogMenu    = nullptr;
InventoryMenu* OgHooks::inventoryMenu = nullptr;
PlayerControl* OgHooks::playerControl = nullptr;
VideoWidget*   OgHooks::video         = nullptr;
MainWindow*    OgHooks::mainWindow    = nullptr;
uint64_t OgHooks::worldGeneration = 0;
uint64_t OgHooks::saveFailures    = 0;
bool     OgHooks::hostDrivesPlayer    = false;
bool     OgHooks::hostFightsForPlayer = false;
int32_t  OgHooks::playerFightTactic   = 0;
