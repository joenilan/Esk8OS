#include "config/Settings.h"
#include "esk8os.h"

// Variables previously in main.cpp
const char* PRODUCT_NAME = "ESK8OS";
char RIDER_NAME[16]      = "JOE";   // mutable: settable from the app, persisted to NVS
const bool  USE_MPH_DEFAULT = true;
const int   STORAGE_SCHEMA_VERSION = 2;
const bool  DEMO_MODE_DEFAULT = true;

bool gDemoMode = DEMO_MODE_DEFAULT;

const int BATTERY_PARALLEL_COUNT = 6;
const int BATTERY_CELL_CAPACITY_MAH = 2800;
float BATTERY_EFFECTIVE_CAPACITY_AH = 16.5;
float BATTERY_STOP_CELL_V = 3.30;
float RANGE_DEFAULT_WH_PER_MILE = 22.0;

int BATTERY_CELLS_COUNT = 10;
float BATTERY_MAX_V = 42.0;
float BATTERY_MIN_V = 33.0;

void recalcBatteryBounds() {
    BATTERY_MAX_V = BATTERY_CELLS_COUNT * BATTERY_FULL_CELL_V;
    BATTERY_MIN_V = BATTERY_CELLS_COUNT * BATTERY_STOP_CELL_V;
}

WheelProfile wheelProfiles[] = {
    { "8IN PNEU", 0.203f, 16, 72, 7.0f },
    { "100MM",    0.100f, 16, 48, 7.0f },
};
int activeWheelProfile = 0;

float profileGearRatio()    { return (float)wheelProfiles[activeWheelProfile].motorPulley / (float)wheelProfiles[activeWheelProfile].wheelPulley; }
float profileCircumfM()     { return wheelProfiles[activeWheelProfile].wheelDiameterM * PI; }
float profilePolePairs()    { return wheelProfiles[activeWheelProfile].polePairs; }

bool useMph = USE_MPH_DEFAULT;
int gBrightnessPct = 100;
int gThemeIdx = 0;
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

void begin() {
    prefs.begin("esk8os", false);
    
    int storedSchema = prefs.getInt("schema", 0);
    if (storedSchema != STORAGE_SCHEMA_VERSION) {
        totalDistanceKm = 0.0;
        tripDistanceKm = 0.0;
        tripMovingSec = 0;
        prefs.putFloat("odo", totalDistanceKm);
        prefs.putFloat("trip", tripDistanceKm);
        prefs.putUInt("tripsec", tripMovingSec);
        prefs.putInt("schema", STORAGE_SCHEMA_VERSION);
    } else {
        totalDistanceKm = prefFloat("odo", 0.0);
        tripDistanceKm  = prefFloat("trip", 0.0);
        // Reload trip moving-time so a quick power-cycle mid-ride continues the
        // same trip (board is authoritative; cold boot favours continue-from-NVS).
        tripMovingSec   = prefs.getUInt("tripsec", 0);
    }

    activeWheelProfile = prefs.getInt("wheelprof", 0);
    if (activeWheelProfile < 0 || activeWheelProfile >= 2) activeWheelProfile = 0;

    gDemoMode      = prefs.getBool("demo", DEMO_MODE_DEFAULT);
    useMph         = prefs.getBool("mph", USE_MPH_DEFAULT);
    { String r = prefs.getString("rider", "JOE"); strlcpy(RIDER_NAME, r.c_str(), sizeof(RIDER_NAME)); }
    gBrightnessPct = constrain(prefs.getInt("bright", 100), 10, 100);
    gThemeIdx      = constrain(prefs.getInt("theme", 0), 0, THEME_COUNT - 1);

    BATTERY_CELLS_COUNT = constrain(prefs.getInt("cells", BATTERY_CELLS_COUNT), 6, 14);
    BATTERY_EFFECTIVE_CAPACITY_AH = constrain(prefFloat("packAh", BATTERY_EFFECTIVE_CAPACITY_AH), 4.0f, 40.0f);
    BATTERY_STOP_CELL_V = constrain(prefFloat("stopCell", BATTERY_STOP_CELL_V), 3.00f, 3.60f);
    RANGE_DEFAULT_WH_PER_MILE = constrain(prefFloat("whmi", RANGE_DEFAULT_WH_PER_MILE), 14.0f, 40.0f);
    
    recalcBatteryBounds();
    currentVoltage = BATTERY_MAX_V;
    minVoltageSession = BATTERY_MAX_V;
}

} // namespace Settings
} // namespace Esk8OS
