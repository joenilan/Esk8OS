#pragma once
#include "esk8os.h"

// UI / pages layer: the boot splash, per-page static chrome + live value
// updates, the page-aware static-frame composer, the common status bars, and
// the safety overlay banners. Draws into the shared `GFX` target and tracks
// changed regions via markDirty() (both owned by main.cpp). Only the entry
// points main.cpp drives directly are declared here; everything else is
// internal to ui.cpp. drawStaticFrame() is declared in esk8os.h (shared).

void waitForBootReady();          // boot splash + progress until ready to run
void showToast(const char* msg);  // brief centered confirmation banner (~1.2 s)

void updateClock();               // top-right ride timer
void updateCurrentPageContent();  // dispatch to the active page's value updates
void updateBatteryCells();        // shared bottom battery strip
void updateBottomBar();           // shared bottom status bar (%, trip, odo)

int  alertState();                // highest-priority alert (0 = none)
void updateOverlays(int state);   // draw/clear the alert banner for `state`
