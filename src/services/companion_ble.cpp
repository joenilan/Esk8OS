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
#include "ui/UiRenderer.h"
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
static inline float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

// Case-insensitive equality (theme names are stored uppercase; the spec sends
// lowercase like "cyber").
static bool ieq(const char* a, const char* b) {
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; a++; b++; }
    return *a == *b;
}

static const char* hudFaceName(int face) {
    switch (face) {
        case HUD_FACE_BATTERY: return "battery";
        case HUD_FACE_WATTS:   return "watts";
        case HUD_FACE_SAFETY:  return "safety";
        default:               return "speed";
    }
}

static const char* hudSettingName() {
    if (gHudFace == HUD_FACE_BATTERY && gBatteryFocus == BATTERY_FOCUS_VOLTS) return "volts";
    return hudFaceName(gHudFace);
}

static const char* batteryFocusName(int focus) {
    return focus == BATTERY_FOCUS_VOLTS ? "volts" : "pct";
}

static const char* hardwareModelName() {
#if ESK8OS_DISPLAY_TFT
    return "tdisplay-s3";
#elif ESK8OS_DISPLAY_OLED
    return "esp32s3-oled";
#elif ESK8OS_HEADLESS
    return "esp32s3-headless";
#else
    return "esp32s3";
#endif
}

static const char* displayKindName() {
#if ESK8OS_DISPLAY_TFT
    return "tft";
#elif ESK8OS_DISPLAY_OLED
    return "oled";
#else
    return "none";
#endif
}

static const char* uiKindName() {
#if ESK8OS_FULL_UI
    return "full";
#elif ESK8OS_DISPLAY_OLED
    return "mini";
#else
    return "headless";
#endif
}

// ---- Settings characteristic (0002) ---------------------------------------

static void buildSettingsJson(char* out, size_t cap) {
    JsonDocument doc;
    doc["hw"]      = hardwareModelName();
    doc["display"] = displayKindName();
    doc["ui"]      = uiKindName();
    doc["buttons"] =
#if ESK8OS_DISPLAY_TFT
        true;
#else
        false;
#endif
    doc["mph"]     = useMph;
    doc["theme"]   = THEMES[gThemeIdx].name;
    doc["poles"]   = (int)lroundf(profilePolePairs() * 2.0f);                                   // pole pairs -> poles
    doc["wheel"]   = (int)lroundf(wheelProfiles[activeWheelProfile].wheelDiameterM * 1000.0f);  // mm
    doc["gear"]    = r1(profileGearRatio());      // motor:wheel pulley ratio the firmware uses
    doc["bat_s"]   = BATTERY_CELLS_COUNT;
    doc["profile"] = activeWheelProfile;          // index; poles/wheel/gear are derived from this preset
    // Battery / range tuning (writable; mirror the board's SETTINGS page).
    doc["packAh"]   = r1(BATTERY_EFFECTIVE_CAPACITY_AH);
    doc["homeCell"] = roundf(BATTERY_HOME_CELL_V * 100.0f) / 100.0f;
    doc["stopCell"] = roundf(BATTERY_STOP_CELL_V * 100.0f) / 100.0f;
    doc["whmi"]     = r1(RANGE_DEFAULT_WH_PER_MILE);
    doc["bright"]   = gBrightnessPct;
    doc["rgb"]      = gStatusRgbEnabled;
    doc["oled_inv"] = gOledInvert;
    doc["demo"]     = gDemoMode;
    doc["rider"]    = RIDER_NAME;
    doc["hud"]      = hudSettingName();
    doc["bfocus"]   = batteryFocusName(gBatteryFocus);
    doc["name"]     = gDeviceName;
    doc["vtype"]    = gVehicleType;
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
        minVoltageUnderLoadSession = BATTERY_MAX_V;
        loadedCellVoltage = currentVoltage / max(1, BATTERY_CELLS_COUNT);
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
        if (BATTERY_HOME_CELL_V < BATTERY_STOP_CELL_V) {
            BATTERY_HOME_CELL_V = BATTERY_STOP_CELL_V;
            prefs.putFloat("homeCell", BATTERY_HOME_CELL_V);
        }
        recalcBatteryBounds();
        currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
        loadedCellVoltage = currentVoltage / max(1, BATTERY_CELLS_COUNT);
        updateRangeEstimate();
        repaint = true;
    }
    if (doc["homeCell"].is<float>()) {
        BATTERY_HOME_CELL_V = constrain((float)doc["homeCell"], BATTERY_STOP_CELL_V, BATTERY_FULL_CELL_V);
        prefs.putFloat("homeCell", BATTERY_HOME_CELL_V);
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
        Esk8OS::UiRenderer::applyDisplayBrightness();
        repaint = true;
    }
    if (doc["rgb"].is<bool>()) {
        gStatusRgbEnabled = doc["rgb"];
        prefs.putBool("rgb", gStatusRgbEnabled);
    }
    if (doc["oled_inv"].is<bool>()) {
        gOledInvert = doc["oled_inv"];
        prefs.putBool("oledInv", gOledInvert);
        Esk8OS::UiRenderer::applyOledInvert();
        repaint = true;
    }
    if (doc["demo"].is<bool>()) {
        gDemoMode = doc["demo"];
        prefs.putBool("demo", gDemoMode);
        if (!gDemoMode) {
            lastVescOkMs = 0;
            vescLinkOk = false;
            telemetryLive = false;
            currentSpeedKmh = 0.0f;
            currentSpeedMph = 0.0f;
            currentAmps = 0.0f;
            currentMotorAmps = 0.0f;
            currentDuty = 0.0f;
            currentWatts = 0.0f;
            peakWatts = 0.0f;
            gPpmConnected = false;
            gPpmDecoded = 0.0f;
            gPpmPulseMs = 0.0f;
        }
        repaint = true;
    }
    if (doc["rider"].is<const char*>()) {
        strlcpy(RIDER_NAME, doc["rider"], sizeof(RIDER_NAME));
        prefs.putString("rider", RIDER_NAME);
        repaint = true;
    }
    if (doc["name"].is<const char*>()) {
        strlcpy(gDeviceName, doc["name"], sizeof(gDeviceName));
        prefs.putString("devname", gDeviceName);   // new BLE name applies on next reboot
    }
    if (doc["vtype"].is<int>()) {
        gVehicleType = constrain((int)doc["vtype"], 0, VT_COUNT - 1);
        prefs.putInt("vtype", gVehicleType);
    }
    if (doc["hud"].is<const char*>()) {
        const char* h = doc["hud"];
        if (ieq(h, "speed")) gHudFace = HUD_FACE_SPEED;
        else if (ieq(h, "battery") || ieq(h, "pct") || ieq(h, "percent")) {
            gHudFace = HUD_FACE_BATTERY;
            gBatteryFocus = BATTERY_FOCUS_PERCENT;
        } else if (ieq(h, "volts") || ieq(h, "voltage")) {
            gHudFace = HUD_FACE_BATTERY;
            gBatteryFocus = BATTERY_FOCUS_VOLTS;
        } else if (ieq(h, "watts") || ieq(h, "power")) gHudFace = HUD_FACE_WATTS;
        else if (ieq(h, "safety") || ieq(h, "range")) gHudFace = HUD_FACE_SAFETY;
        prefs.putInt("hudFace", gHudFace);
        prefs.putInt("batFocus", gBatteryFocus);
        repaint = true;
    } else if (doc["hud"].is<int>()) {
        gHudFace = constrain((int)doc["hud"], 0, HUD_FACE_COUNT - 1);
        prefs.putInt("hudFace", gHudFace);
        repaint = true;
    }
    if (doc["bfocus"].is<const char*>()) {
        const char* f = doc["bfocus"];
        gBatteryFocus = ieq(f, "volts") ? BATTERY_FOCUS_VOLTS : BATTERY_FOCUS_PERCENT;
        prefs.putInt("batFocus", gBatteryFocus);
        repaint = true;
    } else if (doc["bfocus"].is<int>()) {
        gBatteryFocus = constrain((int)doc["bfocus"], 0, BATTERY_FOCUS_COUNT - 1);
        prefs.putInt("batFocus", gBatteryFocus);
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
        char buf[512];
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
    else if (!strcmp(cmd, "BRIDGE_EXIT"))       { if (systemMode == MODE_VESC_BRIDGE) exitBridgeMode(); }
    else if (!strcmp(cmd, "BRIDGE_TOGGLE"))     { if (systemMode == MODE_VESC_BRIDGE) exitBridgeMode(); else enterBridgeMode(); }
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

// Pair code = the BLE MAC tail (last two octets, e.g. "EEFF"). It matches the
// address tail the app sees, so the rider can confirm they're pairing the right
// board. Also advertised (with vtype) in manufacturer data — see below.
static uint8_t g_macHi = 0, g_macLo = 0;
static void computePairCode() {
    std::string mac = NimBLEDevice::getAddress().toString();   // "aa:bb:cc:dd:ee:ff"
    if (mac.size() >= 17) {
        g_macHi = (uint8_t)strtol(mac.substr(12, 2).c_str(), nullptr, 16);   // 5th octet
        g_macLo = (uint8_t)strtol(mac.substr(15, 2).c_str(), nullptr, 16);   // 6th octet
    }
    snprintf(gPairCode, sizeof(gPairCode), "%02X%02X", g_macHi, g_macLo);
}

void companionBleBegin() {
    NimBLEDevice::init(gDeviceName);
    NimBLEDevice::setMTU(517);              // request a large MTU; JSON exceeds the 20-byte default
    computePairCode();

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
    // Manufacturer data (company 0xFFFF = local/testing): [vtype, macHi, macLo].
    // Lets the app show the right vehicle icon + the pair code BEFORE connecting.
    // Fits the 31-byte adv packet alongside the 128-bit service UUID (~28 used).
    uint8_t mfg[] = { 0xFF, 0xFF, (uint8_t)gVehicleType, g_macHi, g_macLo };
    adv->setManufacturerData(std::string((const char*)mfg, sizeof(mfg)));
    NimBLEAdvertisementData scanResp;
    scanResp.setName(gDeviceName);
    scanResp.setCompleteServices(NimBLEUUID(bleBridgeServiceUuid()));
    adv->setScanResponseData(scanResp);
    adv->setScanResponse(true);
    adv->start();
}

void companionBleTick() {
    // Apply queued command writes even in bridge mode so the phone can exit the
    // bridge without local buttons. Settings/telemetry remain dashboard-owned.
    if (g_cmdPending) {
        char local[40];
        portENTER_CRITICAL(&g_mux);
        memcpy(local, g_cmdBuf, sizeof(local)); g_cmdPending = false;
        portEXIT_CRITICAL(&g_mux);
        dispatchCommand(local);
    }

    if (systemMode != MODE_DASHBOARD) return;

    // Apply queued settings from the BLE task on this (UI) thread — GFX/flash-safe.
    if (g_setPending) {
        char local[256];
        portENTER_CRITICAL(&g_mux);
        memcpy(local, g_setBuf, sizeof(local)); g_setPending = false;
        portEXIT_CRITICAL(&g_mux);
        applySettings(local);
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
    const bool live = telemetryLive && (gDemoMode || vescLinkOk);
    JsonDocument doc;
    doc["live"]  = live;
    doc["vesc"]  = vescLinkOk;
    doc["mph"]   = useMph;
    doc["spd"]   = live ? r1(useMph ? currentSpeedMph : currentSpeedKmh) : 0.0f;
    doc["bat"]   = live ? currentBatteryPercent : 0;
    doc["v"]     = live ? r1(currentVoltage) : 0.0f;
    doc["w"]     = live ? (int)currentWatts : 0;            // truncate (drop decimal), never round
    doc["mtr_t"] = live ? (int)currentMotorTemp : 0;        // truncate (hide decimal)
    doc["esc_t"] = live ? (int)currentEscTemp : 0;
    doc["btemp"] = live ? (int)currentBatteryTemp : 0;
    doc["rng"]   = live ? r1(useMph ? remainingRangeKm * KM2MI : remainingRangeKm) : 0.0f;
    doc["max_s"] = r1(useMph ? maxSpeedKmh * KM2MI : maxSpeedKmh);
    doc["wh"]    = r1(currentWattHours);
    // Power detail
    doc["bata"]  = live ? r1(currentAmps) : 0.0f;              // battery A
    doc["mota"]  = live ? r1(currentMotorAmps) : 0.0f;         // motor A
    doc["duty"]  = live ? (int)currentDuty : 0;                // already %
    doc["pkw"]   = live ? (int)peakWatts : 0;                  // live peak-hold W ("peak now")
    doc["mpw"]   = (int)maxWattsSession;         // session max W ("max ride")
    // Energy / session
    doc["whr"]   = r1(currentWhRegen);           // regen Wh
    doc["minv"]  = r1(minVoltageSession);        // session min volt
    doc["minvl"] = r1(minVoltageUnderLoadSession); // lowest loaded/discharge V
    doc["mba"]   = r1(maxBatteryAmpsSession);    // max session battery A
    doc["cellv"] = live ? roundf(loadedCellVoltage * 100.0f) / 100.0f : 0.0f;
    doc["rwarn"] = live ? rangeAlertState : 0;   // 0 ok, 1 turn-home, 2 sag, 3 limp
    doc["sagc"]  = sagEventsSession;
    doc["thome"] = homeVoltageSecondsSession;    // seconds below ride-home floor under load
    doc["tlimp"] = limpVoltageSecondsSession;    // seconds near limp floor under load
    doc["avs"]   = r1(useMph ? avgSpeedKmh * KM2MI : avgSpeedKmh);
    // Trip / odometer / range / efficiency (display units)
    doc["trip"]  = r1(tripDistanceKm * dCv);
    doc["odo"]   = r1(totalDistanceKm * dCv);
    doc["est"]   = r1(estimatedRangeKm * dCv);
    doc["lrng"]  = live ? r1(remainingLimpRangeKm * dCv) : 0.0f;
    doc["lest"]  = r1(estimatedLimpRangeKm * dCv);
    doc["eff"]   = r1(useMph ? avgWhPerKm / KM2MI : avgWhPerKm);      // Wh/mi (mph) or Wh/km
    // System / fault
    doc["fault"] = live ? vescFault : 0;
    doc["rtime"] = (uint32_t)(millis() / 1000UL);                     // board uptime this boot (seconds)
    doc["tmov"]  = tripMovingSec;                                     // trip moving-time (seconds rolling) — board-authoritative
    // Remote input + diagnostics
    doc["ppm"]   = live ? r2(gPpmDecoded) : 0.0f; // throttle -1..1 (brake..accel)
    doc["ppmok"] = live && gPpmConnected;         // remote signal present
    doc["lfault"]= gLastFault;                   // most recent fault (latched)
    doc["slave"] = live && gSlaveOnline;         // 2nd motor online over CAN
    doc["m1a"]   = live ? r1(gMasterMotorAmps) : 0.0f; // master motor current
    doc["m2a"]   = live ? r1(gSlaveMotorAmps) : 0.0f;  // slave motor current
    { char fw[8]; snprintf(fw, sizeof(fw), "%u.%u", gVescFwMajor, gVescFwMinor); doc["fw"] = fw; }

    char buf[768];
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
