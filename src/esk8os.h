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

#ifndef ESK8OS_DISPLAY_TFT
#define ESK8OS_DISPLAY_TFT 0
#endif
#ifndef ESK8OS_DISPLAY_OLED
#define ESK8OS_DISPLAY_OLED 0
#endif
#ifndef ESK8OS_HEADLESS
#define ESK8OS_HEADLESS 0
#endif
#if defined(WOKWI_SIMULATION)
#undef ESK8OS_DISPLAY_TFT
#define ESK8OS_DISPLAY_TFT 1
#endif
#define ESK8OS_FULL_UI ESK8OS_DISPLAY_TFT

#if ESK8OS_DISPLAY_TFT
#include "LGFX_Config.h"   // LovyanGFX device/sprite types + fonts
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
    MODE_TRIP_RESET_CONFIRM,  // hold-L trip reset waits here for L=yes / R=no
    MODE_WIFI_CONFIRM         // app asked for the log/OTA AP; rider must OK on-board
};
enum HudFace {
    HUD_FACE_SPEED = 0,
    HUD_FACE_BATTERY,
    HUD_FACE_WATTS,
    HUD_FACE_SAFETY,
    HUD_FACE_COUNT
};
enum BatteryFocus {
    BATTERY_FOCUS_PERCENT = 0,
    BATTERY_FOCUS_VOLTS,
    BATTERY_FOCUS_COUNT
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
#if ESK8OS_DISPLAY_TFT
extern lgfx::LovyanGFX* GFX;       // active draw target (canvas, or panel)
#endif
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
extern char gDeviceName[20];  // BLE advertised name (settable; tells nearby boards apart)
extern char gPairCode[5];     // BLE MAC tail in hex (e.g. "EEFF") — shown on board + app to confirm pairing
// Vehicle kind — drives the icon + label the companion app shows for this
// board. Values are a wire contract (BLE `vtype`), so NEW types are APPENDED
// (never reordered) to keep already-configured boards reading correctly.
// VT_CUSTOM is the "anything else" type: the rider gives it a free-text name
// (gVehicleLabel) and picks an icon (gVehicleCustomIcon) in the app.
enum VehicleType {
    VT_SKATE = 0, VT_EBIKE, VT_ESCOOTER, VT_EMOPED, VT_CAR,
    VT_CUSTOM,          // was VT_OTHER (5) — same slot, now rider-nameable
    VT_EUC,             // 6: electric unicycle
    VT_ONEWHEEL,        // 7: one-wheel board
    VT_COUNT
};
extern int  gVehicleType;
extern char gVehicleLabel[20];   // custom vehicle name (VT_CUSTOM); NVS "vlabel"
extern int  gVehicleCustomIcon;  // app icon index for VT_CUSTOM; NVS "vicon"
const char* vehicleTypeName(int t);

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
enum { SET_PROFILE, SET_UNITS, SET_DEMO, SET_BRIGHT, SET_THEME, SET_HUD_FACE,
       SET_BATT_FOCUS, SET_CELLS, SET_PACK_AH, SET_STOP_CELL, SET_WHMI,
       SETTINGS_COUNT };

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
extern bool gStatusRgbEnabled;
extern bool gOledInvert;
extern int  gHudFace;
extern int  gBatteryFocus;
extern int  settingsCursor;
extern bool settingsEditing;
extern int  motorHealthPct, batteryHealthPct, escHealthPct;
extern unsigned long gToastUntil;
extern char gToastMsg[];
extern unsigned long gLastInteractionMs;  // last rider interaction (buttons OR app command) — keeps the screensaver awake

// ---- canvas / diagnostics (defined in main.cpp) -----------------------------
extern int           gCanvasH;       // canvas height (dirty-band clamp)
extern uint16_t      gFps;           // frames pushed in the last second
extern unsigned long gLastPushUs;    // duration of the last blit
extern bool          gCanvasPsram;   // true if the frame buffer landed in PSRAM

// ---- shared helpers (defined in main.cpp) -----------------------------------
void pushCanvasFull();
void pushCanvas();          // blit only the accumulated dirty regions (partial)
void markDirty(int y, int h);
void drawStaticFrame();
void saveOdo();

// ---- configuration constants shared with the telemetry module ---------------
const float BATTERY_NOMINAL_CELL_V = 3.7;
const float BATTERY_FULL_CELL_V    = 4.20;
const float RANGE_LEARN_MIN_DISTANCE_KM = 3.2;   // about 2 mi; enough distance for stable Wh/mi
const float RANGE_LEARN_MIN_WH = 20.0;
const float RANGE_TURN_HOME_KM = 3.2;     // about 2 mi; conservative reserve prompt
const float RANGE_LIMP_HOME_KM = 1.6;     // about 1 mi; emergency reserve prompt
const unsigned long VESC_LINK_TIMEOUT_MS = 3000;
// A VESC whose logic is alive over UART but whose power stage is off (battery
// disconnected, or its 5V back-fed from the board on the bench) reports garbage
// telemetry and a spurious DRV fault. No real esk8 pack reads anywhere near this
// low, so a read below this means "VESC not actually powered" — ignore it so the
// link goes LINK LOST instead of surfacing a meaningless FAULT.
const float VESC_MIN_OPERATIONAL_V = 6.0f;

// ---- runtime battery/range config (mutable; defined in main, edited in Settings)
extern float BATTERY_EFFECTIVE_CAPACITY_AH;
extern float BATTERY_STOP_CELL_V;
extern float BATTERY_HOME_CELL_V;
extern float RANGE_DEFAULT_WH_PER_MILE;
extern int   BATTERY_CELLS_COUNT;
extern float BATTERY_MAX_V;
extern float BATTERY_MIN_V;
// The supported series-cell range. ONE definition, shared by every layer that
// clamps or validates a cell count (NVS load, BLE settings write, and the VESC
// base-tier adoption) so they can never disagree — see Settings::applyBaseTier
// (F-8). Widen these (and re-test the SoC/sag/range chain) to support bigger
// packs; today the whole battery-math stack is validated for 6..14S.
static const int BATTERY_CELLS_MIN = 6;
static const int BATTERY_CELLS_MAX = 14;
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
// Rider wheel-size calibration (mm). 0 = use the active preset's diameter;
// >0 overrides it, so a worn/soft pneumatic can be dialled in like an e-bike
// computer. NVS "wheelmm"; cleared when the wheel preset is switched.
extern int          gWheelDiameterMm;
int  effectiveWheelDiameterMm();          // the diameter actually used for speed/distance

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
extern float maxWattsSession, minVoltageSession, minVoltageUnderLoadSession;
extern float maxMotorAmpsSession, maxBatteryAmpsSession;
extern float loadedCellVoltage;
extern uint32_t homeVoltageSecondsSession, limpVoltageSecondsSession;
extern int rangeAlertState, sagEventsSession;
extern int   vescFault;
extern bool  vescLinkOk;
extern bool  telemetryLive;
// remote input + diagnostics (see VescUartTransport / telemetry)
extern float gPpmDecoded;     // -1..1 throttle (brake..accel)
extern float gPpmPulseMs;     // last PPM pulse length, ms
extern bool  gPpmConnected;   // valid remote signal present
extern uint8_t gVescFwMajor, gVescFwMinor;
extern bool  gSlaveOnline;
extern float gMasterMotorAmps, gSlaveMotorAmps;
extern float gMasterMotorTemp, gSlaveMotorTemp;
extern float gMasterEscTemp, gSlaveEscTemp;
extern int   gLastFault;
// modern-protocol extras (aggregated setup values; zero on legacy ESC firmware)
extern bool  gVescModernProto;
extern uint8_t gVescNumVescs, gSlaveCanId;
extern float gVescSpeedKmh;   // ESC-computed speed (its own gearing config)
extern int   gVescBattPct;    // ESC's own battery estimate, 0-100
extern float gVescWhLeft, gVescOdoKm;
extern char  gVescHwName[17];
// adaptive battery calibration — learned on real rides, NVS-persisted (see
// telemetry.cpp "adaptive calibration" section; `cal` console command)
extern float gPackROhm;         // pack internal resistance (sag compensation)
extern float gTypicalRideAmps;  // EMA of battery draw while rolling (sag-floor input)
extern float gLearnedPackWh;    // measured deliverable pack energy (0 = not learned)
extern float gLearnedWhPerKm;   // cross-ride consumption EMA (0 = not learned)
extern unsigned long rideStartMs;
extern float sessionTripStartKm, tripDistanceKm, totalDistanceKm;
extern uint32_t tripMovingSec;       // trip moving time (seconds rolling); persisted, board-authoritative
extern uint32_t sessionMovingStartSec; // tripMovingSec baseline at session start (for the moving AVG)
extern unsigned long lastMovedMs;    // millis() of last rolling sample; drives the parked auto-reset
extern float estimatedRangeKm, remainingRangeKm, estimatedLimpRangeKm, remainingLimpRangeKm, avgWhPerKm;
extern float rideStartVescWh, rideStartVescWhRegen;
extern bool  rideEnergyBaselineSet, rangeEstimateReady;

// ---- telemetry history + compact ride summaries (shared structs) ------------
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

// ---- persistence (defined in main) -------------------------------------------
extern Preferences prefs;
