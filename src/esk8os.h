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
enum SystemMode {
    MODE_DASHBOARD,
    MODE_VESC_BRIDGE,
    MODE_BRIDGE_CONFIRM,
    MODE_TRIP_RESET_CONFIRM   // hold-L trip reset waits here for L=yes / R=no
};
extern int        currentPage;
extern SystemMode systemMode;

// ---- demo flag --------------------------------------------------------------
extern bool gDemoMode;
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

// OTA Update State
extern bool       gOtaInProgress;
extern int        gOtaProgressPct;

// ---- branding (defined in main.cpp; external linkage const char*) ------------
extern const char* PRODUCT_NAME;
extern char RIDER_NAME[16];   // mutable, NVS-persisted (settable from the app)

// ---- shared compile-time UI constants ---------------------------------------
// Battery warning thresholds over the configured usable pack window (display
// treats 0% as "stop riding", not absolute empty).
const int   BATT_WARN_PCT = 50;          // below -> yellow
const int   BATT_LOW_PCT  = 30;          // below -> orange
const int   BATT_CRIT_PCT = 15;          // below -> red (stop / charge now)
// Over-temp alert thresholds (deg C): past these the temp turns red + banner.
const float MOTOR_TEMP_LIMIT = 80.0;
const float ESC_TEMP_LIMIT   = 80.0;
// Degree symbol in the GLCD font (LovyanGFX Font0, CP437): 0xF8.
const char  DEG = (char)0xF8;

// ---- settings page rows (cursor index -> SET_*; used by ui + checkButtons) ---
enum { SET_PROFILE, SET_UNITS, SET_DEMO, SET_BRIGHT, SET_THEME, SET_CELLS,
       SET_PACK_AH, SET_STOP_CELL, SET_WHMI, SETTINGS_COUNT };

// ---- color themes (palette table; applied to the COL_* globals in main) ------
struct RGB { uint8_t r, g, b; };
struct Theme {
    const char* name;
    RGB bg, border, dim, label, white, green, red, accent, yellow, orange;
};
extern const Theme THEMES[];
extern const int   THEME_COUNT;
extern int         gThemeIdx;     // selected theme (NVS-persisted; Settings > THEME)
void applyTheme(int idx);         // recompute COL_* from THEMES[idx] (defined in main)

// ---- UI runtime state (defined in main.cpp) ---------------------------------
extern bool useMph;
extern int  gBrightnessPct;
extern int  settingsCursor;
extern int  motorHealthPct, batteryHealthPct, escHealthPct;
extern unsigned long gToastUntil;
extern char gToastMsg[];

// ---- canvas / diagnostics (defined in main.cpp) -----------------------------
extern int           gCanvasH;       // canvas height (dirty-band clamp)
extern uint16_t      gFps;           // frames pushed in the last second
extern unsigned long gLastPushUs;    // duration of the last blit
extern bool          gCanvasPsram;   // true if the frame buffer landed in PSRAM

// ---- shared helpers (defined in main.cpp) -----------------------------------
void pushCanvasFull();
void markDirty(int y, int h);
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

// ---- wheel/gearing profiles (display speed/distance math only; NOT VESC) -----
struct WheelProfile {
    const char* name;
    float wheelDiameterM;
    int   motorPulley;
    int   wheelPulley;
    float polePairs;
};
extern WheelProfile wheelProfiles[];
extern int          activeWheelProfile;   // loaded from NVS in setup()

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
extern uint32_t tripMovingSec;       // trip moving time (seconds rolling); persisted, board-authoritative
extern uint32_t sessionMovingStartSec; // tripMovingSec baseline at session start (for the moving AVG)
extern unsigned long lastMovedMs;    // millis() of last rolling sample; drives the parked auto-reset
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
