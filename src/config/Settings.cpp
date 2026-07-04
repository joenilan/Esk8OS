#include "config/Settings.h"
#include "esk8os.h"

// Variables previously in main.cpp
const char* PRODUCT_NAME = "ESK8OS";
char RIDER_NAME[16]      = "";   // empty = no rider line; settable from the app, NVS-persisted
char gDeviceName[20]     = "ESK8-BLE";   // BLE advertised name (settable; tells nearby boards apart)
char gPairCode[5]        = "";           // BLE MAC tail hex; filled once at BLE init (companionBleBegin)
int  gVehicleType        = VT_SKATE;     // VehicleType; drives the app's vehicle icon
char gVehicleLabel[20]   = "";           // custom vehicle name (VT_CUSTOM)
int  gVehicleCustomIcon  = 0;            // app icon index chosen for VT_CUSTOM

const char* vehicleTypeName(int t) {
    switch (t) {
        case VT_SKATE:    return "SKATE";
        case VT_EBIKE:    return "E-BIKE";
        case VT_ESCOOTER: return "SCOOTER";
        case VT_EMOPED:   return "MOPED";
        case VT_CAR:      return "CAR";
        case VT_EUC:      return "EUC";
        case VT_ONEWHEEL: return "ONEWHEEL";
        case VT_CUSTOM:   return gVehicleLabel[0] ? gVehicleLabel : "CUSTOM";
        default:          return "CUSTOM";
    }
}
const bool  USE_MPH_DEFAULT = true;
const int   STORAGE_SCHEMA_VERSION = 2;
const bool  DEMO_MODE_DEFAULT = true;

bool gDemoMode = DEMO_MODE_DEFAULT;

// Neutral generic defaults — the LAST config tier, used only before a VESC has
// ever been read AND the rider hasn't set a value. Nothing here may describe
// any specific board or pack (rule: base truth comes from the connected ESC
// via applyVescBase; rider changes become explicit NVS overrides on top).
float BATTERY_EFFECTIVE_CAPACITY_AH = 10.0;
float BATTERY_HOME_CELL_V = 3.40;
float BATTERY_STOP_CELL_V = 3.10;
float RANGE_DEFAULT_WH_PER_MILE = 20.0;

int BATTERY_CELLS_COUNT = 10;
float BATTERY_MAX_V = 42.0;
float BATTERY_MIN_V = 31.0;

void recalcBatteryBounds() {
    BATTERY_MAX_V = BATTERY_CELLS_COUNT * BATTERY_FULL_CELL_V;
    BATTERY_MIN_V = BATTERY_CELLS_COUNT * BATTERY_STOP_CELL_V;
}

// Generic starting presets — deliberately typical-of-the-category numbers, NOT
// any specific board's gearing. Presets only matter until the first VESC read:
// the ESC's own wheel/gearing/poles (applyVescBase) beat them from then on.
WheelProfile wheelProfiles[] = {
    { "STREET 90", 0.090f, 15, 40, 7.0f },
    { "PNEU 200",  0.200f, 15, 60, 7.0f },
};
int activeWheelProfile = 0;

int gWheelDiameterMm = 0;   // 0 = use VESC/preset; >0 = rider override (measured rolling dia)

// The VESC-read base tier. Filled from the NVS cache at boot and refreshed by
// applyVescBase() on every live capture. valid=false until either happens.
static Esk8OS::Transports::VescBaseConfig gBase;

// The diameter (mm) actually used for the display speed/distance math:
// rider override > VESC config > active preset.
int effectiveWheelDiameterMm() {
    if (gWheelDiameterMm > 0) return gWheelDiameterMm;
    if (gBase.valid) return (int)lroundf(gBase.wheelDiameterM * 1000.0f);
    return (int)lroundf(wheelProfiles[activeWheelProfile].wheelDiameterM * 1000.0f);
}

// Gearing/poles: the ESC's configured drivetrain is physical truth — riders
// override wheel DIAMETER (loaded rolling size differs from nominal) but not
// pulley teeth or magnet counts, so the VESC tier wins whenever it exists.
float profileGearRatio() {
    if (gBase.valid && gBase.gearRatio > 0.01f) return 1.0f / gBase.gearRatio;  // motor:wheel
    return (float)wheelProfiles[activeWheelProfile].motorPulley / (float)wheelProfiles[activeWheelProfile].wheelPulley;
}
float profileCircumfM()     { return (effectiveWheelDiameterMm() / 1000.0f) * PI; }
float profilePolePairs() {
    if (gBase.valid && gBase.motorPoles >= 2) return gBase.motorPoles / 2.0f;
    return wheelProfiles[activeWheelProfile].polePairs;
}

bool useMph = USE_MPH_DEFAULT;
int gBrightnessPct = 100;
bool gStatusRgbEnabled = true;
bool gOledInvert = false;
int gThemeIdx = 0;
int gHudFace = HUD_FACE_SPEED;
int gBatteryFocus = BATTERY_FOCUS_PERCENT;
const int BRIGHTNESS_STEPS[] = { 25, 50, 75, 100 };
const int BRIGHTNESS_STEP_COUNT = sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]);

Preferences prefs;

namespace Esk8OS {
namespace Settings {

// getFloat() goes through getBytes(), which logs an ESP "nvs_get_blob" error for
// a key that doesn't exist yet. Guard with isKey() so first-boot/default reads
// stay quiet and just return the default.
static float prefFloat(const char* key, float def) {
    return prefs.isKey(key) ? prefs.getFloat(key, def) : def;
}

static void applyBaseTier();   // defined below begin(); begin() uses it for the boot-time cache

void begin() {
    prefs.begin("esk8os", false);
    
    // The lifetime odometer, trip, and moving-time are plain scalars — never
    // format-versioned — so ALWAYS preserve them, even across a schema bump.
    // (A firmware update used to wipe them to 0 here, silently losing real
    // mileage. Load them unconditionally; on fresh flash the keys are absent
    // and prefFloat returns the 0.0 default, which is the correct new-board start.)
    totalDistanceKm = prefFloat("odo", 0.0);
    tripDistanceKm  = prefFloat("trip", 0.0);
    tripMovingSec   = prefs.getUInt("tripsec", 0);

    int storedSchema = prefs.getInt("schema", 0);
    if (storedSchema != STORAGE_SCHEMA_VERSION) {
        // Future format-versioned keys migrate here; odo/trip are kept as-is.
        prefs.putInt("schema", STORAGE_SCHEMA_VERSION);
    }

    activeWheelProfile = prefs.getInt("wheelprof", 0);
    if (activeWheelProfile < 0 || activeWheelProfile >= 2) activeWheelProfile = 0;
    gWheelDiameterMm = constrain(prefs.getInt("wheelmm", 0), 0, 400);   // 0 = use preset

    gDemoMode      = prefs.getBool("demo", DEMO_MODE_DEFAULT);
    useMph         = prefs.getBool("mph", USE_MPH_DEFAULT);
    { String r = prefs.getString("rider", ""); strlcpy(RIDER_NAME, r.c_str(), sizeof(RIDER_NAME)); }
    { String n = prefs.getString("devname", "ESK8-BLE"); strlcpy(gDeviceName, n.c_str(), sizeof(gDeviceName)); }
    gVehicleType = constrain(prefs.getInt("vtype", VT_SKATE), 0, VT_COUNT - 1);
    { String vl = prefs.getString("vlabel", ""); strlcpy(gVehicleLabel, vl.c_str(), sizeof(gVehicleLabel)); }
    gVehicleCustomIcon = constrain(prefs.getInt("vicon", 0), 0, 15);
    gBrightnessPct = constrain(prefs.getInt("bright", 100), 10, 100);
    gStatusRgbEnabled = prefs.getBool("rgb", true);
    gOledInvert = prefs.getBool("oledInv", false);
    gThemeIdx      = constrain(prefs.getInt("theme", 0), 0, THEME_COUNT - 1);
    gHudFace       = constrain(prefs.getInt("hudFace", HUD_FACE_SPEED), 0, HUD_FACE_COUNT - 1);
    gBatteryFocus  = constrain(prefs.getInt("batFocus", BATTERY_FOCUS_PERCENT), 0, BATTERY_FOCUS_COUNT - 1);

    BATTERY_CELLS_COUNT = constrain(prefs.getInt("cells", BATTERY_CELLS_COUNT), 6, 14);
    BATTERY_EFFECTIVE_CAPACITY_AH = constrain(prefFloat("packAh", BATTERY_EFFECTIVE_CAPACITY_AH), 4.0f, 40.0f);
    BATTERY_STOP_CELL_V = constrain(prefFloat("stopCell", BATTERY_STOP_CELL_V), 3.00f, 3.60f);
    BATTERY_HOME_CELL_V = constrain(prefFloat("homeCell", BATTERY_HOME_CELL_V), BATTERY_STOP_CELL_V, BATTERY_FULL_CELL_V);
    RANGE_DEFAULT_WH_PER_MILE = constrain(prefFloat("whmi", RANGE_DEFAULT_WH_PER_MILE), 14.0f, 40.0f);

    // Adaptive battery calibration learned on rides (telemetry.cpp; `cal` cmd).
    // 0 = "not learned yet" for the two learned-energy values; any stored number
    // outside a physically sane band is treated as corrupt and reset to that
    // sentinel, so a bad NVS read can't drive the range math to absurd values
    // (a near-zero Wh/km would otherwise blow the range estimate up to ~infinity).
    gPackROhm        = constrain(prefFloat("packR", gPackROhm), 0.01f, 0.50f);
    gTypicalRideAmps = constrain(prefFloat("typA", gTypicalRideAmps), 2.0f, 60.0f);
    gLearnedPackWh   = prefFloat("packWhL", 0.0f);
    if (gLearnedPackWh < 50.0f || gLearnedPackWh > 5000.0f) gLearnedPackWh = 0.0f;   // Wh
    gLearnedWhPerKm  = prefFloat("whkmL", 0.0f);
    if (gLearnedWhPerKm < 5.0f || gLearnedWhPerKm > 35.0f) gLearnedWhPerKm = 0.0f;   // Wh/km


    // VESC base tier: restore the last parsed ESC config from its NVS cache so
    // bench/demo sessions (pack off) keep the real drivetrain/pack numbers, then
    // let it fill everything the rider hasn't explicitly overridden.
    if (prefs.getBool("vbValid", false)) {
        gBase.cutStartV      = prefFloat("vbCutS", 0);
        gBase.cutEndV        = prefFloat("vbCutE", 0);
        gBase.motorAmpMax    = prefFloat("vbMotA", 0);
        gBase.battAmpMax     = prefFloat("vbBatA", 0);
        gBase.battAmpRegen   = prefFloat("vbRegA", 0);
        gBase.motorPoles     = (uint8_t)prefs.getInt("vbPoles", 0);
        gBase.gearRatio      = prefFloat("vbGear", 0);
        gBase.wheelDiameterM = prefFloat("vbWheel", 0);
        gBase.cells          = (uint8_t)prefs.getInt("vbCells", 0);
        gBase.packAh         = prefFloat("vbAh", 0);
        gBase.valid = true;
        applyBaseTier();
    }

    recalcBatteryBounds();
    currentVoltage = 0;                       // show 0 (no reading) until the VESC is polled, not a fake 42V
    minVoltageSession = BATTERY_MAX_V;         // min-tracking must start high (see telemetry.cpp:336)
    minVoltageUnderLoadSession = BATTERY_MAX_V;
    loadedCellVoltage = BATTERY_FULL_CELL_V;
}

// Re-derive every battery value the rider hasn't explicitly set (NVS key
// absent = no override) from the VESC base. Wheel/gearing/poles are handled
// live in the profile functions above; these are the stored-global ones.
static void applyBaseTier() {
    if (!gBase.valid) return;
    if (!prefs.isKey("cells") && gBase.cells >= 6 && gBase.cells <= 14)
        BATTERY_CELLS_COUNT = gBase.cells;
    if (!prefs.isKey("packAh"))
        BATTERY_EFFECTIVE_CAPACITY_AH = constrain(gBase.packAh, 4.0f, 40.0f);
    if (!prefs.isKey("stopCell") && gBase.cells > 0)
        BATTERY_STOP_CELL_V = constrain(gBase.cutEndV / gBase.cells, 3.00f, 3.60f);
    if (!prefs.isKey("homeCell") && gBase.cells > 0)
        BATTERY_HOME_CELL_V = constrain(gBase.cutStartV / gBase.cells,
                                        BATTERY_STOP_CELL_V, BATTERY_FULL_CELL_V);
    recalcBatteryBounds();
}

void applyVescBase(const Esk8OS::Transports::VescBaseConfig& b) {
    if (!b.valid) return;
    // Persist the cache change-guarded: a capture parses on every boot, and the
    // config rarely changes — don't rewrite NVS for identical values.
    bool changed = !gBase.valid ||
        fabsf(gBase.cutStartV - b.cutStartV) > 0.05f ||
        fabsf(gBase.cutEndV - b.cutEndV) > 0.05f ||
        fabsf(gBase.packAh - b.packAh) > 0.05f ||
        fabsf(gBase.gearRatio - b.gearRatio) > 0.005f ||
        fabsf(gBase.wheelDiameterM - b.wheelDiameterM) > 0.0005f ||
        fabsf(gBase.motorAmpMax - b.motorAmpMax) > 0.5f ||
        fabsf(gBase.battAmpMax - b.battAmpMax) > 0.5f ||
        fabsf(gBase.battAmpRegen - b.battAmpRegen) > 0.5f ||
        gBase.motorPoles != b.motorPoles || gBase.cells != b.cells;
    gBase = b;
    if (changed) {
        prefs.putFloat("vbCutS", b.cutStartV);
        prefs.putFloat("vbCutE", b.cutEndV);
        prefs.putFloat("vbMotA", b.motorAmpMax);
        prefs.putFloat("vbBatA", b.battAmpMax);
        prefs.putFloat("vbRegA", b.battAmpRegen);
        prefs.putInt("vbPoles", b.motorPoles);
        prefs.putFloat("vbGear", b.gearRatio);
        prefs.putFloat("vbWheel", b.wheelDiameterM);
        prefs.putInt("vbCells", b.cells);
        prefs.putFloat("vbAh", b.packAh);
        prefs.putBool("vbValid", true);
        applyBaseTier();
        gRedrawAll = true;
    }
}

bool vescBase(Esk8OS::Transports::VescBaseConfig* out) {
    *out = gBase;
    return gBase.valid;
}

const char* sourceTag(const char* nvsKey, bool vescProvides) {
    if (prefs.isKey(nvsKey)) return "rider";
    if (gBase.valid && vescProvides) return "vesc";
    return "default";
}

bool removeOverride(const char* nvsKey) {
    static const char* KEYS[] = { "cells", "packAh", "stopCell", "homeCell", "whmi", "wheelmm" };
    bool known = false;
    for (const char* k : KEYS) if (!strcmp(k, nvsKey)) { known = true; break; }
    if (!known) return false;
    prefs.remove(nvsKey);
    // Reset to the compiled generic, then let the VESC base win where valid.
    if      (!strcmp(nvsKey, "cells"))    BATTERY_CELLS_COUNT = 10;
    else if (!strcmp(nvsKey, "packAh"))   BATTERY_EFFECTIVE_CAPACITY_AH = 10.0f;
    else if (!strcmp(nvsKey, "stopCell")) BATTERY_STOP_CELL_V = 3.10f;
    else if (!strcmp(nvsKey, "homeCell")) BATTERY_HOME_CELL_V = 3.40f;
    else if (!strcmp(nvsKey, "whmi"))     RANGE_DEFAULT_WH_PER_MILE = 20.0f;
    else if (!strcmp(nvsKey, "wheelmm"))  gWheelDiameterMm = 0;
    applyBaseTier();
    recalcBatteryBounds();
    gRedrawAll = true;
    return true;
}

} // namespace Settings
} // namespace Esk8OS
