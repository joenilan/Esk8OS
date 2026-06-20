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
#include <Preferences.h>
#include "LGFX_Config.h"   // LovyanGFX device/sprite types + fonts
#ifndef WOKWI_SIMULATION
#include <VescUart.h>
#endif

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

// ---- configuration constants shared with the telemetry module ---------------
const float BATTERY_NOMINAL_CELL_V = 3.7;
const float BATTERY_FULL_CELL_V    = 4.20;
const float RANGE_LEARN_MIN_DISTANCE_KM = 1.6;
const float RANGE_LEARN_MIN_WH = 20.0;
const unsigned long VESC_LINK_TIMEOUT_MS = 3000;

// ---- runtime battery/range config (mutable; defined in main, edited in Settings)
extern float BATTERY_EFFECTIVE_CAPACITY_AH;
extern float BATTERY_STOP_CELL_V;
extern float RANGE_DEFAULT_WH_PER_MILE;
extern int   BATTERY_CELLS_COUNT;
extern float BATTERY_MAX_V;
extern float BATTERY_MIN_V;
void recalcBatteryBounds();

// ---- wheel profile accessors (defined in main) ------------------------------
float profileGearRatio();
float profileCircumfM();
float profilePolePairs();

// ---- telemetry state (defined in main; currentSpeedKmh/lastVescOkMs above) ---
extern float currentSpeedMph, currentVoltage;
extern int   currentBatteryPercent;
extern float currentMotorTemp, currentBatteryTemp, currentEscTemp;
extern float currentAmps, currentMotorAmps, currentDuty, currentWatts, peakWatts;
extern float currentWattHours, currentWhRegen;
extern float maxSpeedKmh, avgSpeedKmh;
extern float maxWattsSession, minVoltageSession, maxMotorAmpsSession;
extern int   vescFault;
extern bool  vescLinkOk;
extern unsigned long rideStartMs;
extern float sessionTripStartKm, tripDistanceKm, totalDistanceKm;
extern float estimatedRangeKm, remainingRangeKm, avgWhPerKm;
extern float rideStartVescWh, rideStartVescWhRegen;
extern bool  rideEnergyBaselineSet, rangeEstimateReady;

// ---- telemetry history + ride logs (shared structs; data defined in main) ----
struct TelemetrySample {
    float speedKmh, volts, watts, batteryAmps, motorAmps, duty, motorTemp, escTemp;
    int   batteryPct;
};
struct RideLog {
    uint32_t durationSec;
    float distanceKm, maxSpeedKmh, avgSpeedKmh, whUsed, whRegen, minVoltage, maxWatts;
};
const int HIST_N = 180;       // 3 minutes at 1 sample/sec
const int RIDE_LOG_MAX = 10;
extern TelemetrySample history[];
extern int histHead, histCount;

// ---- persistence + VESC link (defined in main) ------------------------------
extern Preferences prefs;
#ifndef WOKWI_SIMULATION
extern VescUart UART;
#endif
