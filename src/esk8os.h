#pragma once
// ============================================================================
// ESK8OS shared core.
//
// Types, enums, macros, and the cross-module globals/prototypes shared between
// the firmware's translation units (main.cpp, bridge.cpp, ble_bridge.cpp, and
// future modules). Plain globals keep their single DEFINITION in main.cpp; this
// header only DECLARES them (extern) so other modules can use them.
//
// As main.cpp is split further (telemetry, pages, ...), move the symbols those
// modules need into this header.
// ============================================================================
#include <Arduino.h>
#include "LGFX_Config.h"   // LovyanGFX device/sprite types + fonts

// ---- layout -----------------------------------------------------------------
static constexpr int UI_W = 170;   // logical UI width (the panel is 170 px wide)
extern int X0;                     // X offset to center the UI on wider screens

// ---- pages / top-level mode -------------------------------------------------
enum PageId {
    PAGE_HUD = 0, PAGE_DASH, PAGE_POWER, PAGE_TRIP,
    PAGE_SETTINGS, PAGE_SYSTEM, PAGE_GRAPHS, PAGE_LOGS, PAGE_COUNT
};
enum SystemMode { MODE_DASHBOARD, MODE_VESC_BRIDGE };
extern int        currentPage;
extern SystemMode systemMode;

// ---- demo flag --------------------------------------------------------------
extern bool gDemoMode;             // runtime demo toggle (Settings page)
#if defined(WOKWI_SIMULATION)
  #define DEMO_DATA true
#else
  #define DEMO_DATA gDemoMode
#endif

// ---- display target + NZXT-CAM palette --------------------------------------
extern lgfx::LovyanGFX* GFX;       // active draw target (canvas, or panel)
extern uint16_t COL_BG, COL_BORDER, COL_DIM, COL_LABEL, COL_WHITE,
                COL_GREEN, COL_RED, COL_ACCENT, COL_YELLOW, COL_ORANGE;

// ---- shared state used across modules ---------------------------------------
extern float         currentSpeedKmh;
extern unsigned long lastVescOkMs;
extern bool          gRedrawAll;

// ---- shared helpers (defined in main.cpp) -----------------------------------
void pushCanvasFull();
void drawStaticFrame();
void saveOdo();
