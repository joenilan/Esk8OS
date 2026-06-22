#include "companion_ble.h"

#if defined(BLE_BRIDGE_ENABLED) && !defined(WOKWI_SIMULATION)
// ---------------------------------------------------------------------------
// Real implementation. Owns the NimBLE stack; co-hosts the VESC-Tool NUS bridge.
// ---------------------------------------------------------------------------
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "esk8os.h"
#include "ble_bridge.h"
#include "bridge.h"
#include "webexport.h"
#include "ui/ui.h"
#include "app/App.h"
#include "telemetry/telemetry.h"
#include "board/BoardLilyGoTDisplayS3.h"

// Custom companion service + characteristics (docs/companion_api_spec.md §2).
static const char* DEVICE_NAME   = "ESK8-BLE";   // same name VESC Tool scans for
static const char* SVC_COMPANION = "5043697A-0000-4682-93CB-33BB0A149F7E";
static const char* CH_TELEMETRY  = "5043697A-0001-4682-93CB-33BB0A149F7E"; // NOTIFY
static const char* CH_SETTINGS   = "5043697A-0002-4682-93CB-33BB0A149F7E"; // READ | WRITE
static const char* CH_COMMAND    = "5043697A-0003-4682-93CB-33BB0A149F7E"; // WRITE

static const float KM2MI = 0.621371f;

static NimBLEServer*         g_server = nullptr;
static NimBLECharacteristic* g_tel    = nullptr;

// Payloads handed from the BLE task to the UI loop (see THREADING note in .h).
static portMUX_TYPE  g_mux        = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_cmdPending = false;
static char          g_cmdBuf[40];
static volatile bool g_setPending = false;
static char          g_setBuf[256];

static inline float r1(float v) { return roundf(v * 10.0f) / 10.0f; }

// Case-insensitive equality (theme names are stored uppercase; the spec sends
// lowercase like "cyber").
static bool ieq(const char* a, const char* b) {
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; a++; b++; }
    return *a == *b;
}

// ---- Settings characteristic (0002) ---------------------------------------

static void buildSettingsJson(char* out, size_t cap) {
    JsonDocument doc;
    doc["mph"]     = useMph;
    doc["theme"]   = THEMES[gThemeIdx].name;
    doc["poles"]   = (int)lroundf(profilePolePairs() * 2.0f);                                   // pole pairs -> poles
    doc["wheel"]   = (int)lroundf(wheelProfiles[activeWheelProfile].wheelDiameterM * 1000.0f);  // mm
    doc["gear"]    = r1(profileGearRatio());      // motor:wheel pulley ratio the firmware uses
    doc["bat_s"]   = BATTERY_CELLS_COUNT;
    doc["profile"] = activeWheelProfile;          // index; poles/wheel/gear are derived from this preset
    // Battery / range tuning (writable; mirror the board's SETTINGS page).
    doc["packAh"]   = r1(BATTERY_EFFECTIVE_CAPACITY_AH);
    doc["stopCell"] = roundf(BATTERY_STOP_CELL_V * 100.0f) / 100.0f;
    doc["whmi"]     = (int)lroundf(RANGE_DEFAULT_WH_PER_MILE);
    doc["bright"]   = gBrightnessPct;
    doc["demo"]     = gDemoMode;
    doc["rider"]    = RIDER_NAME;
    serializeJson(doc, out, cap);
}

static void applySettings(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;       // parse error -> ignore
    bool repaint = false, themeChanged = false;

    if (doc["mph"].is<bool>()) {
        useMph = doc["mph"];
        prefs.putBool("mph", useMph);
        repaint = true;
    }
    if (doc["bat_s"].is<int>()) {
        BATTERY_CELLS_COUNT = constrain((int)doc["bat_s"], 6, 14);
        prefs.putInt("cells", BATTERY_CELLS_COUNT);
        recalcBatteryBounds();
        currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
        minVoltageSession = BATTERY_MAX_V;
        updateRangeEstimate();
        repaint = true;
    }
    if (doc["profile"].is<int>()) {
        activeWheelProfile = constrain((int)doc["profile"], 0, 1);
        prefs.putInt("wheelprof", activeWheelProfile);
        updateRangeEstimate();
        repaint = true;
    }
    // poles / wheel / gear are read-only here: they come from the selected wheel
    // preset, not independently settable fields. Use "profile" to switch presets.
    if (doc["packAh"].is<float>()) {
        BATTERY_EFFECTIVE_CAPACITY_AH = constrain((float)doc["packAh"], 4.0f, 40.0f);
        prefs.putFloat("packAh", BATTERY_EFFECTIVE_CAPACITY_AH);
        updateRangeEstimate();
        repaint = true;
    }
    if (doc["stopCell"].is<float>()) {
        BATTERY_STOP_CELL_V = constrain((float)doc["stopCell"], 3.00f, 3.60f);
        prefs.putFloat("stopCell", BATTERY_STOP_CELL_V);
        recalcBatteryBounds();
        currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
        updateRangeEstimate();
        repaint = true;
    }
    if (doc["whmi"].is<float>()) {
        RANGE_DEFAULT_WH_PER_MILE = constrain((float)doc["whmi"], 14.0f, 40.0f);
        prefs.putFloat("whmi", RANGE_DEFAULT_WH_PER_MILE);
        updateRangeEstimate();
        repaint = true;
    }
    if (doc["bright"].is<int>()) {
        gBrightnessPct = constrain((int)doc["bright"], 10, 100);
        prefs.putInt("bright", gBrightnessPct);
        Esk8OS::Board::setBacklight((uint8_t)(gBrightnessPct * 255 / 100));
        repaint = true;
    }
    if (doc["demo"].is<bool>()) {
        gDemoMode = doc["demo"];
        prefs.putBool("demo", gDemoMode);
        repaint = true;
    }
    if (doc["rider"].is<const char*>()) {
        strlcpy(RIDER_NAME, doc["rider"], sizeof(RIDER_NAME));
        prefs.putString("rider", RIDER_NAME);
        repaint = true;
    }
    if (doc["theme"].is<const char*>()) {
        const char* tn = doc["theme"];
        for (int i = 0; i < THEME_COUNT; i++) {
            if (ieq(THEMES[i].name, tn)) { gThemeIdx = i; themeChanged = true; break; }
        }
    }
    if (themeChanged) {
        prefs.putInt("theme", gThemeIdx);
        applyTheme(gThemeIdx);
        repaint = true;
    }
    if (repaint) { drawStaticFrame(); gRedrawAll = true; }
}

class SettingsCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        char buf[256];
        buildSettingsJson(buf, sizeof(buf));
        c->setValue((uint8_t*)buf, strlen(buf));
    }
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        portENTER_CRITICAL(&g_mux);
        size_t n = v.size(); if (n >= sizeof(g_setBuf)) n = sizeof(g_setBuf) - 1;
        memcpy(g_setBuf, v.data(), n); g_setBuf[n] = 0;
        g_setPending = true;
        portEXIT_CRITICAL(&g_mux);
    }
};

// ---- Command characteristic (0003) ----------------------------------------

static void dispatchCommand(const char* cmd) {
    if      (!strcmp(cmd, "TRIP_RESET"))        Esk8OS::App::resetTrip();
    else if (!strcmp(cmd, "PAGE_NEXT"))       { settingsCursor = 0; currentPage = (currentPage + 1) % PAGE_COUNT; drawStaticFrame(); gRedrawAll = true; }
    else if (!strcmp(cmd, "PAGE_PREV"))       { settingsCursor = 0; currentPage = (currentPage + PAGE_COUNT - 1) % PAGE_COUNT; drawStaticFrame(); gRedrawAll = true; }
    else if (!strncmp(cmd, "PAGE_SET:", 9))   { int p = atoi(cmd + 9); if (p >= 0 && p < PAGE_COUNT) { settingsCursor = 0; currentPage = p; drawStaticFrame(); gRedrawAll = true; } }  // absolute page (app pages don't 1:1 the board's count)
    else if (!strcmp(cmd, "BRIDGE_MODE"))       enterBridgeMode();         // safety-checks speed internally
    else if (!strcmp(cmd, "WIFI_EXPORT_START")) { if (!webServiceActive()) { webServiceStart(); showToast("WIFI EXPORT"); } }  // standalone AP + http (logs/OTA); telemetry stays live
    else if (!strcmp(cmd, "WIFI_EXPORT_STOP"))  { if (webServiceActive())  { webServiceStop();  showToast("WIFI OFF"); } }
    else if (!strcmp(cmd, "REBOOT"))          { delay(100); ESP.restart(); }
}

class CommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        portENTER_CRITICAL(&g_mux);
        size_t n = v.size(); if (n >= sizeof(g_cmdBuf)) n = sizeof(g_cmdBuf) - 1;
        memcpy(g_cmdBuf, v.data(), n); g_cmdBuf[n] = 0;
        g_cmdPending = true;
        portEXIT_CRITICAL(&g_mux);
    }
};

// ---- Server lifecycle ------------------------------------------------------

class CompanionServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {}
    void onDisconnect(NimBLEServer*) override {
        NimBLEDevice::startAdvertising();   // keep the board discoverable after any client drops
    }
};

void companionBleBegin() {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setMTU(517);              // request a large MTU; JSON exceeds the 20-byte default

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new CompanionServerCallbacks());

    NimBLEService* svc = g_server->createService(SVC_COMPANION);
    g_tel = svc->createCharacteristic(CH_TELEMETRY, NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* set = svc->createCharacteristic(
        CH_SETTINGS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    set->setCallbacks(new SettingsCallbacks());

    NimBLECharacteristic* cmd = svc->createCharacteristic(
        CH_COMMAND, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    cmd->setCallbacks(new CommandCallbacks());
    svc->start();

    // Build the VESC-Tool NUS service onto the SAME server now, so the whole GATT
    // table exists before advertising. It stays idle until VESC Bridge mode flips
    // its forwarding flag.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    bleBridgeRegister(g_server, adv);

    // Primary adv packet carries the companion service (broadcasting 100% of the
    // time). The scan response carries the name + NUS UUID so VESC Tool still
    // discovers it without overflowing the 31-byte adv packet.
    adv->addServiceUUID(SVC_COMPANION);
    NimBLEAdvertisementData scanResp;
    scanResp.setName(DEVICE_NAME);
    scanResp.setCompleteServices(NimBLEUUID(bleBridgeServiceUuid()));
    adv->setScanResponseData(scanResp);
    adv->setScanResponse(true);
    adv->start();
}

void companionBleTick() {
    // Only act while the dashboard owns the screen/UART. In VESC Bridge mode the
    // UI loop calls bridgeLoop() instead, and telemetry pauses (spec §6); queued
    // writes stay pending and apply once the dashboard resumes.
    if (systemMode != MODE_DASHBOARD) return;

    // Apply queued writes from the BLE task on this (UI) thread — GFX/flash-safe.
    if (g_setPending) {
        char local[256];
        portENTER_CRITICAL(&g_mux);
        memcpy(local, g_setBuf, sizeof(local)); g_setPending = false;
        portEXIT_CRITICAL(&g_mux);
        applySettings(local);
    }
    if (g_cmdPending) {
        char local[40];
        portENTER_CRITICAL(&g_mux);
        memcpy(local, g_cmdBuf, sizeof(local)); g_cmdPending = false;
        portEXIT_CRITICAL(&g_mux);
        dispatchCommand(local);
    }

    // 5 Hz telemetry notify (spec §3).
    static unsigned long lastTel = 0;
    unsigned long now = millis();
    if (now - lastTel < 200) return;
    lastTel = now;
    if (!g_tel || !g_server || g_server->getConnectedCount() == 0) return;

    // Distance/speed convert to the board's display unit (mi/mph) so they line up
    // with spd/rng/max_s. Efficiency is Wh/mi when mph (divide by KM2MI). These
    // match the conversions ui.cpp uses on the board's own pages.
    const float dCv = useMph ? KM2MI : 1.0f;     // km -> display distance
    JsonDocument doc;
    doc["spd"]   = r1(useMph ? currentSpeedMph : currentSpeedKmh);
    doc["bat"]   = currentBatteryPercent;
    doc["v"]     = r1(currentVoltage);
    doc["w"]     = (int)currentWatts;
    doc["mtr_t"] = (int)currentMotorTemp;
    doc["esc_t"] = (int)currentEscTemp;
    doc["btemp"] = (int)currentBatteryTemp;
    doc["rng"]   = r1(useMph ? remainingRangeKm * KM2MI : remainingRangeKm);
    doc["max_s"] = r1(useMph ? maxSpeedKmh * KM2MI : maxSpeedKmh);
    doc["wh"]    = (int)currentWattHours;
    // Power detail
    doc["bata"]  = r1(currentAmps);              // battery A
    doc["mota"]  = r1(currentMotorAmps);         // motor A
    doc["duty"]  = (int)currentDuty;             // already %
    doc["pkw"]   = (int)peakWatts;               // session peak W
    // Energy / session
    doc["whr"]   = (int)currentWhRegen;          // regen Wh
    doc["minv"]  = r1(minVoltageSession);        // session min volt
    doc["avs"]   = r1(useMph ? avgSpeedKmh * KM2MI : avgSpeedKmh);
    // Trip / odometer / range / efficiency (display units)
    doc["trip"]  = r1(tripDistanceKm * dCv);
    doc["odo"]   = r1(totalDistanceKm * dCv);
    doc["est"]   = r1(estimatedRangeKm * dCv);
    doc["eff"]   = (int)(useMph ? avgWhPerKm / KM2MI : avgWhPerKm);   // Wh/mi (mph) or Wh/km
    // System / fault
    doc["fault"] = vescFault;
    doc["rtime"] = (uint32_t)((millis() - rideStartMs) / 1000UL);     // ride seconds

    char buf[320];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    g_tel->setValue((uint8_t*)buf, n);
    g_tel->notify();
}

#else
// ---------------------------------------------------------------------------
// No-op stubs (BLE disabled, or Wokwi build).
// ---------------------------------------------------------------------------
void companionBleBegin() {}
void companionBleTick() {}
#endif
