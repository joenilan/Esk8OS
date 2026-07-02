#include "console.h"
#include "esk8os.h"
#include "app/App.h"
#include "telemetry/telemetry.h"
#include "version.h"
#include "logging/sessionlog.h"
#include "services/webexport.h"
#include "services/wifi_bridge.h"
#include "ui/UiRenderer.h"
#include <LittleFS.h>
#include <esp_system.h>
#if ESK8OS_DISPLAY_OLED
#include <Wire.h>
#endif
#ifndef ESK8OS_STATUS_RGB
#define ESK8OS_STATUS_RGB 0
#endif
#ifndef ESK8OS_STATUS_RGB_PIN
#define ESK8OS_STATUS_RGB_PIN 48
#endif

// Forward-declared (defined in App.cpp, external linkage) — re-applies the
// backlight from gBrightnessPct after the `bright` command changes it.
void applyBrightness();

static const char* SESSIONS_DIR = "/sessions";
static char   g_line[96];
static size_t g_len = 0;
#if ESK8OS_DUAL_CONSOLE
static char   g_line0[96];
static size_t g_len0 = 0;
#endif

#if ESK8OS_DUAL_CONSOLE
class ConsoleOutput : public Print {
public:
    size_t write(uint8_t c) override {
        size_t n = Serial.write(c);
        Serial0.write(c);
        return n;
    }
    size_t write(const uint8_t* buffer, size_t size) override {
        size_t n = Serial.write(buffer, size);
        Serial0.write(buffer, size);
        return n;
    }
    void flush() override {
        Serial.flush();
        Serial0.flush();
    }
};

static ConsoleOutput g_consoleOut;
static Print& consoleOut() { return g_consoleOut; }
#else
static Print& consoleOut() { return Serial; }
#endif

// ---- confirmation guard for destructive commands ----------------------------
// A dangerous command stashes its original line in g_pending and prints a "[y/N]"
// prompt; the next input line confirms (y/yes) and re-dispatches, or cancels.
static char g_pending[96] = "";
static bool g_confirmed   = false;

// Returns true to proceed (this exec was already confirmed); otherwise stashes
// the full command line, prints the prompt, and returns false so the caller aborts.
static bool needConfirm(const char* what, const char* fullLine) {
    if (g_confirmed) return true;
    strlcpy(g_pending, fullLine, sizeof(g_pending));
    consoleOut().printf("%s -- confirm? [y/N]\n", what);
    return false;
}

// Resolve a user-typed name to a full path: bare names go under /sessions.
static String resolvePath(const char* arg) {
    String a = arg; a.trim();
    if (a.startsWith("/")) return a;
    return String(SESSIONS_DIR) + "/" + a;
}

static void cmdList() {
    File dir = LittleFS.open(SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) { consoleOut().println("(no sessions yet)"); return; }
    int n = 0; size_t total = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        consoleOut().printf("  %-14s %8u B\n", f.name(), (unsigned)f.size());
        total += f.size(); n++;
        f.close();
    }
    dir.close();
    consoleOut().printf("%d file(s), %u B in /sessions | FS %u/%u B used\n",
                  n, (unsigned)total,
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

static void cmdCat(const char* arg) {
    String path = resolvePath(arg);
    File f = LittleFS.open(path, "r");
    if (!f) { consoleOut().println("not found: " + path); return; }
    consoleOut().printf("----- %s (%u B) -----\n", path.c_str(), (unsigned)f.size());
    uint8_t buf[128];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) consoleOut().write(buf, n);
    f.close();
    consoleOut().println("----- end -----");
}

static void cmdRm(const char* arg) {
    String a = arg; a.trim();
    if (a == "") { consoleOut().println("usage: rm <file|all>"); return; }
    if (a == "all") {
        // Goes through sessionLog so the active file handle is closed first,
        // then a fresh board session file starts.
        sessionLogDeleteAll();
        consoleOut().println("cleared all session files");
        return;
    }
    String path = resolvePath(arg);
    consoleOut().println(LittleFS.remove(path) ? "removed " + path : "failed: " + path);
}

static void cmdLog(const char* arg) {
    String a = arg; a.trim();
    if (a == "on")  { sessionLogSetEnabled(true);  consoleOut().println("session logging ON"); return; }
    if (a == "off") { sessionLogSetEnabled(false); consoleOut().println("session logging OFF"); return; }
    consoleOut().printf("logging %s%s | %u KB free\n",
                  sessionLogEnabled() ? "ON" : "OFF",
                  sessionLogFull() ? " (auto-stopped: low space)" : "",
                  (unsigned)(sessionLogFreeBytes() / 1024));
}

// Toggle the standalone log/OTA web service from USB serial — a test path for
// the unbridged "hybrid" flow (otherwise only reachable via the BLE command).
static void cmdWifi(const char* arg) {
    String a = arg; a.trim();
    if (a == "on") {
        if (systemMode != MODE_DASHBOARD) { consoleOut().println("can't: exit bridge mode first"); return; }
        if (webServiceActive()) { consoleOut().println("wifi export already ON"); return; }
        webServiceStart();
        consoleOut().printf("wifi export ON - join %s / %s, then http://192.168.4.1\n",
                            wifiBridgeSsid(), wifiBridgePass());
        return;
    }
    if (a == "off") { webServiceStop(); consoleOut().println("wifi export OFF"); return; }
    consoleOut().printf("wifi export %s\n", webServiceActive() ? "ON" : "OFF");
}

// ---- debug / status dumps ---------------------------------------------------
static bool liveTelemetryAvailable() {
    return telemetryLive && (gDemoMode || vescLinkOk);
}

static void cmdStat() {
    if (!liveTelemetryAvailable()) {
        consoleOut().printf("telemetry unavailable | demo %s | VESC link %s | fault %d\n",
            gDemoMode ? "ON" : "OFF", vescLinkOk ? "OK" : "DOWN", vescFault);
        consoleOut().println("live fields masked: speed/battery/volts/power/amps/temps/range");
        return;
    }
    const char* su = useMph ? "mph" : "kmh";
    float spd = useMph ? currentSpeedMph : currentSpeedKmh;
    consoleOut().printf("spd %.1f %s | batt %d%% | %.1fV | link %s | fault %d\n",
        spd, su, currentBatteryPercent, currentVoltage, vescLinkOk ? "OK" : "DOWN", vescFault);
    consoleOut().printf("pwr %dW (peak %dW) | batA %.1f | motA %.1f | duty %d%%\n",
        (int)currentWatts, (int)peakWatts, currentAmps, currentMotorAmps, (int)currentDuty);
    consoleOut().printf("safety cell %.2fV | min loaded %.1fV | max batA %.1f | sag %d (%us home, %us limp)\n",
        loadedCellVoltage, minVoltageUnderLoadSession, maxBatteryAmpsSession,
        sagEventsSession, (unsigned)homeVoltageSecondsSession, (unsigned)limpVoltageSecondsSession);
    consoleOut().printf("temp motor %dC | esc %dC | batt %dC\n",
        (int)currentMotorTemp, (int)currentEscTemp, (int)currentBatteryTemp);
    consoleOut().printf("energy %dWh used | %dWh regen\n", (int)currentWattHours, (int)currentWhRegen);
}

static void cmdDiag() {
    if (!liveTelemetryAvailable()) {
        consoleOut().printf("VESC telemetry unavailable | demo %s | link %s | last-fault %d\n",
            gDemoMode ? "ON" : "OFF", vescLinkOk ? "OK" : "DOWN", gLastFault);
        consoleOut().printf("remote: %s | throttle %+.3f | pulse %.4f ms\n",
            gPpmConnected ? "CONNECTED" : "no signal", gPpmDecoded, gPpmPulseMs);
        return;
    }
    consoleOut().printf("remote: %s | throttle %+.3f (%s) | pulse %.4f ms\n",
        gPpmConnected ? "CONNECTED" : "no signal", gPpmDecoded,
        gPpmDecoded > 0.02f ? "accel" : (gPpmDecoded < -0.02f ? "brake" : "center"), gPpmPulseMs);
    consoleOut().printf("vesc fw %u.%u%s%s | proto %s | fault %d | last-fault %d\n",
        gVescFwMajor, gVescFwMinor,
        gVescHwName[0] ? " " : "", gVescHwName,
        gVescModernProto ? "setup-values" : "legacy", vescFault, gLastFault);
    consoleOut().printf("can bus: %u vesc(s) | slave id %u %s\n",
        gVescNumVescs, gSlaveCanId, gSlaveOnline ? "online" : (gSlaveCanId ? "offline" : "(none found)"));
    consoleOut().printf("motor A: master %.1f | slave %.1f\n", gMasterMotorAmps, gSlaveMotorAmps);
    consoleOut().printf("temps C: motor m %.0f/s %.0f | esc m %.0f/s %.0f\n",
        gMasterMotorTemp, gSlaveMotorTemp, gMasterEscTemp, gSlaveEscTemp);
    if (gVescModernProto) {
        float cv = useMph ? 0.621371f : 1.0f;
        consoleOut().printf("esc-side: spd %.1f %s | batt %d%% | %.0f Wh left | odo %.1f %s\n",
            gVescSpeedKmh * cv, useMph ? "mph" : "kmh", gVescBattPct, gVescWhLeft,
            gVescOdoKm * cv, useMph ? "mi" : "km");
    }
}

// Burst-sample the decoded PPM for ~2s and report the spread, to compare what
// the receiver feeds the VESC with the handheld remote OFF vs ON (idle vs active).
static void cmdPpmScan() {
    float pMin = 9, pMax = -9, dMin = 9, dMax = -9;
    float pPrev = gPpmPulseMs; int changes = 0;
    for (int i = 0; i < 80; i++) {            // ~2.4s at 30ms
        float p = gPpmPulseMs, d = gPpmDecoded;
        if (p < pMin) pMin = p; if (p > pMax) pMax = p;
        if (d < dMin) dMin = d; if (d > dMax) dMax = d;
        if (fabsf(p - pPrev) > 0.0001f) { changes++; pPrev = p; }
        delay(30);
    }
    consoleOut().printf("ppm scan (2.4s): pulse %.4f..%.4f spread %.4f ms, %d changes | throttle %.3f..%.3f | conn=%s\n",
        pMin, pMax, pMax - pMin, changes, dMin, dMax, gPpmConnected ? "Y" : "N");
}

static void cmdTrip(const char* arg) {
    if (!strcmp(arg, "reset")) {
        Esk8OS::App::resetTrip();
        consoleOut().println("trip reset (distance + moving-time + session metrics)");
        return;
    }
    float cv = useMph ? 0.621371f : 1.0f;
    const char* u  = useMph ? "mi"  : "km";
    const char* su = useMph ? "mph" : "kmh";
    consoleOut().printf("trip %.2f %s | tmov %02u:%02u:%02u | odo %.2f %s\n",
        tripDistanceKm * cv, u, tripMovingSec / 3600, (tripMovingSec / 60) % 60, tripMovingSec % 60,
        totalDistanceKm * cv, u);
    consoleOut().printf("avg %.1f %s | max %.1f %s | home %.1f %s (rem %.1f) | limp %.1f %s (rem %.1f) %s\n",
        avgSpeedKmh * cv, su, maxSpeedKmh * cv, su,
        estimatedRangeKm * cv, u, remainingRangeKm * cv,
        estimatedLimpRangeKm * cv, u, remainingLimpRangeKm * cv,
        rangeEstimateReady ? "[learned]" : "[default]");
}

static void cmdSys() {
    unsigned long up = millis() / 1000;
    consoleOut().printf("fw %s\n", FW_VERSION_FULL);
    consoleOut().printf("uptime %02lu:%02lu:%02lu | reset-reason %d | fps %u\n",
        up / 3600, (up / 60) % 60, up % 60, (int)esp_reset_reason(), gFps);
    consoleOut().printf("heap %u B free (min %u) | psram %u/%u B free\n",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
        (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
}

static void cmdI2c() {
#if ESK8OS_DISPLAY_OLED
    consoleOut().println("i2c scan:");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            consoleOut().printf("  0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0) consoleOut().println("  none");
#else
    consoleOut().println("i2c scan unavailable in this build");
#endif
}

static void cmdCfg() {
    consoleOut().printf("units %s | demo %s | bright %d%% | rgb %s | oled %s | theme %d | rider %s\n",
        useMph ? "mph" : "kmh", gDemoMode ? "ON" : "OFF", gBrightnessPct,
        gStatusRgbEnabled ? "ON" : "OFF", gOledInvert ? "INVERT" : "NORMAL", gThemeIdx, RIDER_NAME);
    consoleOut().printf("name %s | vtype %d (%s)\n", gDeviceName, gVehicleType, vehicleTypeName(gVehicleType));
    consoleOut().printf("battery %d cells | pack %.1f Ah | home %.2f V/cell | limp %.2f V/cell | %.1f Wh/mi\n",
        BATTERY_CELLS_COUNT, BATTERY_EFFECTIVE_CAPACITY_AH, BATTERY_HOME_CELL_V, BATTERY_STOP_CELL_V, RANGE_DEFAULT_WH_PER_MILE);
    WheelProfile& w = wheelProfiles[activeWheelProfile];
    consoleOut().printf("wheel prof %d: %s (%.0f mm, %d/%d, %.1f pp)\n",
        activeWheelProfile, w.name, w.wheelDiameterM * 1000.0f, w.motorPulley, w.wheelPulley, w.polePairs);
    const char* hud = "speed";
    if (gHudFace == HUD_FACE_BATTERY) hud = (gBatteryFocus == BATTERY_FOCUS_VOLTS) ? "volts" : "battery";
    else if (gHudFace == HUD_FACE_WATTS) hud = "watts";
    else if (gHudFace == HUD_FACE_SAFETY) hud = "safety";
    consoleOut().printf("hud %s\n", hud);
}

// ---- actions ----------------------------------------------------------------
static void saveHudFace() {
    prefs.putInt("hudFace", gHudFace);
    prefs.putInt("batFocus", gBatteryFocus);
    gRedrawAll = true;
}

static void cmdHud(const char* arg) {
    String a = arg; a.trim(); a.toLowerCase();
    if (a == "next") {
        if (gHudFace == HUD_FACE_SPEED) {
            gHudFace = HUD_FACE_BATTERY;
            gBatteryFocus = BATTERY_FOCUS_PERCENT;
        } else if (gHudFace == HUD_FACE_BATTERY && gBatteryFocus == BATTERY_FOCUS_PERCENT) {
            gBatteryFocus = BATTERY_FOCUS_VOLTS;
        } else if (gHudFace == HUD_FACE_BATTERY) {
            gHudFace = HUD_FACE_WATTS;
        } else if (gHudFace == HUD_FACE_WATTS) {
            gHudFace = HUD_FACE_SAFETY;
        } else {
            gHudFace = HUD_FACE_SPEED;
            gBatteryFocus = BATTERY_FOCUS_PERCENT;
        }
        saveHudFace();
    } else if (a == "speed") {
        gHudFace = HUD_FACE_SPEED;
        saveHudFace();
    } else if (a == "battery" || a == "pct" || a == "percent") {
        gHudFace = HUD_FACE_BATTERY;
        gBatteryFocus = BATTERY_FOCUS_PERCENT;
        saveHudFace();
    } else if (a == "volts" || a == "voltage") {
        gHudFace = HUD_FACE_BATTERY;
        gBatteryFocus = BATTERY_FOCUS_VOLTS;
        saveHudFace();
    } else if (a == "watts" || a == "power") {
        gHudFace = HUD_FACE_WATTS;
        saveHudFace();
    } else if (a == "safety" || a == "range") {
        gHudFace = HUD_FACE_SAFETY;
        saveHudFace();
    } else if (a.length()) {
        consoleOut().println("usage: hud [speed|battery|volts|watts|safety|next]");
        return;
    }

    const char* hud = "speed";
    if (gHudFace == HUD_FACE_BATTERY) hud = (gBatteryFocus == BATTERY_FOCUS_VOLTS) ? "volts" : "battery";
    else if (gHudFace == HUD_FACE_WATTS) hud = "watts";
    else if (gHudFace == HUD_FACE_SAFETY) hud = "safety";
    consoleOut().printf("hud %s\n", hud);
}

static void cmdDemo(const char* arg) {
    String a = arg; a.trim();
    if (a == "on" || a == "off") {
        gDemoMode = (a == "on");
        prefs.putBool("demo", gDemoMode);
        rideEnergyBaselineSet = false;     // re-baseline VESC energy on the next real poll
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
        drawStaticFrame();   // the DEMO MODE badge lives in the static top bar
        gRedrawAll = true;
    }
    consoleOut().printf("demo %s\n", gDemoMode ? "ON" : "OFF");
}

static void cmdUnits(const char* arg) {
    String a = arg; a.trim();
    if (a == "mph" || a == "kmh" || a == "km") {
        useMph = (a == "mph");
        prefs.putBool("mph", useMph);
        gRedrawAll = true;
    }
    consoleOut().printf("units %s\n", useMph ? "mph" : "kmh");
}

static void cmdBright(const char* arg) {
    String a = arg; a.trim();
    if (a.length()) {
        gBrightnessPct = constrain(a.toInt(), 10, 100);
        prefs.putInt("bright", gBrightnessPct);
        applyBrightness();
    }
    consoleOut().printf("bright %d%%\n", gBrightnessPct);
}

static void cmdRgb(const char* arg) {
    String a = arg; a.trim(); a.toLowerCase();
    if (a == "on" || a == "1" || a == "yes") {
        gStatusRgbEnabled = true;
        prefs.putBool("rgb", true);
    } else if (a == "off" || a == "0" || a == "no") {
        gStatusRgbEnabled = false;
        prefs.putBool("rgb", false);
#if ESK8OS_STATUS_RGB
        neopixelWrite(ESK8OS_STATUS_RGB_PIN, 0, 0, 0);
#endif
    } else if (a.length()) {
        consoleOut().println("usage: rgb [on|off]");
        return;
    }
    consoleOut().printf("rgb %s\n", gStatusRgbEnabled ? "ON" : "OFF");
}

static void cmdOled(const char* arg) {
    String a = arg; a.trim(); a.toLowerCase();
    if (a == "invert" || a == "inverted" || a == "light" || a == "white") {
        gOledInvert = true;
        prefs.putBool("oledInv", true);
        Esk8OS::UiRenderer::applyOledInvert();
        gRedrawAll = true;
    } else if (a == "normal" || a == "dark" || a == "black") {
        gOledInvert = false;
        prefs.putBool("oledInv", false);
        Esk8OS::UiRenderer::applyOledInvert();
        gRedrawAll = true;
    } else if (a == "saver" || a == "screensaver" || a == "preview") {
        Esk8OS::UiRenderer::previewOledScreensaver(20000UL);
    } else if (a == "boot" || a == "splash") {
        Esk8OS::UiRenderer::showBootSplash(12, "DISPLAY"); delay(160);
        Esk8OS::UiRenderer::showBootSplash(35, "BOARD"); delay(160);
        Esk8OS::UiRenderer::showBootSplash(60, "UART"); delay(160);
        Esk8OS::UiRenderer::showBootSplash(82, "PHONE"); delay(160);
        Esk8OS::UiRenderer::showBootSplash(100, "READY"); delay(300);
        gRedrawAll = true;
    } else if (a.length()) {
        consoleOut().println("usage: oled [normal|invert|saver|boot]");
        return;
    }
    consoleOut().printf("oled %s\n", gOledInvert ? "invert" : "normal");
}

static void cmdRider(const char* arg) {
    String a = arg; a.trim();
    if (a.length()) {
        strlcpy(RIDER_NAME, a.c_str(), sizeof(RIDER_NAME));
        prefs.putString("rider", RIDER_NAME);
        gRedrawAll = true;
    }
    consoleOut().printf("rider %s\n", RIDER_NAME);
}

static void cmdName(const char* arg) {
    String a = arg; a.trim();
    if (a.length()) {
        strlcpy(gDeviceName, a.c_str(), sizeof(gDeviceName));
        prefs.putString("devname", gDeviceName);
    }
    consoleOut().printf("name %s (reboot to re-advertise)\n", gDeviceName);
}

static void cmdVtype(const char* arg) {
    String a = arg; a.trim();
    if (a.length()) {
        gVehicleType = constrain((int)a.toInt(), 0, VT_COUNT - 1);
        prefs.putInt("vtype", gVehicleType);
    }
    consoleOut().printf("vtype %d (%s)  [0=skate 1=ebike 2=scooter 3=moped 4=car 5=other]\n",
                        gVehicleType, vehicleTypeName(gVehicleType));
}

static void cmdHelp() {
    consoleOut().println(F("commands:"));
    consoleOut().println(F("  help            this list"));
    consoleOut().println(F("  logs            list board session files + storage use"));
    consoleOut().println(F("  cat <file>      dump a session CSV (e.g. cat s0001.csv)"));
    consoleOut().println(F("  rm <file|all>   delete one session file, or all"));
    consoleOut().println(F("  log [on|off]    logging switch / status"));
    consoleOut().println(F("  wifi [on|off]   standalone log/OTA web service (http://192.168.4.1)"));
    consoleOut().println(F("  free            partition usage"));
    consoleOut().println(F("  odo [reset|set <v>]  odometer + trip (reset=0, set <v> in display unit)"));
    consoleOut().println(F("  stat            live telemetry (speed/power/temps/energy)"));
    consoleOut().println(F("  diag            remote/PPM throttle + VESC diagnostics"));
    consoleOut().println(F("  trip [reset]    trip distance/time/avg/max/range, or full reset"));
    consoleOut().println(F("  cal [reset]     learned battery calibration (pack R/energy/Wh-mi)"));
    consoleOut().println(F("  sys             fw, uptime, heap/psram, reset reason, fps"));
    consoleOut().println(F("  cfg             units/demo/brightness/battery/wheel config"));
    consoleOut().println(F("  i2c             scan I2C bus (OLED builds)"));
    consoleOut().println(F("  hud [face|next] speed/battery/volts/watts/safety"));
    consoleOut().println(F("  demo [on|off]   simulate telemetry (persisted)"));
    consoleOut().println(F("  units [mph|kmh] display units (persisted)"));
    consoleOut().println(F("  bright <10-100> backlight % (persisted)"));
    consoleOut().println(F("  rgb [on|off]    status RGB LED (persisted)"));
    consoleOut().println(F("  oled [normal|invert|saver|boot] OLED pixels / previews"));
    consoleOut().println(F("  rider [name]    rider name (persisted)"));
    consoleOut().println(F("  reboot          restart the board"));
}

static void dispatch(char* line) {
    char orig[96];
    strlcpy(orig, line, sizeof(orig));   // keep the un-split line to re-dispatch on confirm
    char* sp = strchr(line, ' ');
    const char* arg = "";
    if (sp) { *sp = '\0'; arg = sp + 1; }

    if      (!strcmp(line, "help") || !strcmp(line, "?")) cmdHelp();
    else if (!strcmp(line, "logs") || !strcmp(line, "ls")) cmdList();
    else if (!strcmp(line, "cat"))  cmdCat(arg);
    else if (!strcmp(line, "rm")) {
        if (arg[0]) {
            char msg[80];
            if (!strcmp(arg, "all")) strlcpy(msg, "delete ALL session files", sizeof(msg));
            else snprintf(msg, sizeof(msg), "delete session file '%s'", arg);
            if (!needConfirm(msg, orig)) return;
        }
        cmdRm(arg);
    }
    else if (!strcmp(line, "log"))  cmdLog(arg);
    else if (!strcmp(line, "wifi")) cmdWifi(arg);
    else if (!strcmp(line, "free")) consoleOut().printf("FS %u/%u B used\n",
                 (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    else if (!strcmp(line, "odo")) {
        if (!strcmp(arg, "reset")) {
            if (!needConfirm("reset the LIFETIME odometer to 0", orig)) return;
            totalDistanceKm = 0.0f;
            saveOdo();                          // persist the cleared lifetime total
            consoleOut().println("odo reset -> 0");
        } else if (!strncmp(arg, "set ", 4)) {
            float v = atof(arg + 4);            // value in the active display unit
            if (v >= 0) {
                if (!needConfirm("overwrite the lifetime odometer", orig)) return;
                totalDistanceKm = useMph ? v / 0.621371f : v;
                saveOdo();
                consoleOut().printf("odo set -> %.2f %s\n", v, useMph ? "mi" : "km");
            } else {
                consoleOut().println("odo set: need a value >= 0");
            }
        } else {
            float cv = useMph ? 0.621371f : 1.0f;
            const char* u = useMph ? "mi" : "km";
            consoleOut().printf("odo %.2f %s | trip %.2f %s | tmov %02u:%02u:%02u\n",
                 totalDistanceKm * cv, u, tripDistanceKm * cv, u,
                 tripMovingSec / 3600, (tripMovingSec / 60) % 60, tripMovingSec % 60);
        }
    }
    else if (!strcmp(line, "stat") || !strcmp(line, "tel")) cmdStat();
    else if (!strcmp(line, "diag")) cmdDiag();
    else if (!strcmp(line, "ppmscan")) cmdPpmScan();
    else if (!strcmp(line, "trip")) {
        if (!strcmp(arg, "reset") && !needConfirm("reset the trip (distance + moving-time)", orig)) return;
        cmdTrip(arg);
    }
    else if (!strcmp(line, "cal")) {
        if (!strcmp(arg, "reset")) {
            if (!needConfirm("clear the learned battery calibration (pack R / energy / Wh-per-mi)", orig)) return;
            telemetryResetCal();
            consoleOut().println("calibration reset to seeds; relearns on the next rides");
        } else {
            telemetryPrintCal(consoleOut());
        }
    }
    else if (!strcmp(line, "sys"))  cmdSys();
    else if (!strcmp(line, "i2c"))  cmdI2c();
    else if (!strcmp(line, "cfg") || !strcmp(line, "config")) cmdCfg();
    else if (!strcmp(line, "hud")) cmdHud(arg);
    else if (!strcmp(line, "demo")) cmdDemo(arg);
    else if (!strcmp(line, "units")) cmdUnits(arg);
    else if (!strcmp(line, "bright")) cmdBright(arg);
    else if (!strcmp(line, "rgb")) cmdRgb(arg);
    else if (!strcmp(line, "oled")) cmdOled(arg);
    else if (!strcmp(line, "rider")) cmdRider(arg);
    else if (!strcmp(line, "name")) cmdName(arg);
    else if (!strcmp(line, "vtype")) cmdVtype(arg);
    else if (!strcmp(line, "reboot")) {
        if (!needConfirm("reboot the board", orig)) return;
        consoleOut().println("rebooting...");
        consoleOut().flush();
        delay(100);
        ESP.restart();
    }
    else if (line[0]) consoleOut().println("? unknown - try 'help'");
}

static void pollConsoleInput(Stream& in, char* line, size_t& len, int& esc) {
    // Skip ANSI escape sequences (arrow keys etc. send ESC [ <final>), so cursor
    // movement in the terminal doesn't leak control bytes into a command.
    while (in.available()) {
        char c = in.read();

        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) { if (c >= 0x40 && c <= 0x7e) esc = 0; continue; }  // CSI final byte
        if (c == 0x1b) { esc = 1; continue; }                            // ESC

        if (c == '\r' || c == '\n') {
            // End of line on any terminator (\r, \n, or \r\n). The trailing
            // terminator of a \r\n pair sees an empty buffer and is ignored,
            // so a command never dispatches twice.
            if (len > 0) {
                line[len] = '\0';
                if (g_pending[0]) {
                    // Awaiting a [y/N] answer to a stashed destructive command.
                    // Anything but y/yes cancels (safe default).
                    bool yes = (line[0] == 'y' || line[0] == 'Y');
                    char cmd[96];
                    strlcpy(cmd, g_pending, sizeof(cmd));
                    g_pending[0] = '\0';
                    if (yes) { g_confirmed = true; dispatch(cmd); g_confirmed = false; }
                    else consoleOut().println("cancelled");
                } else {
                    dispatch(line);
                }
                len = 0;
            }
        } else if (c == 0x08 || c == 0x7f) {     // backspace / delete
            if (len > 0) len--;
        } else if (c >= 0x20 && (uint8_t)c < 0x7f) {   // printable ASCII only
            if (len < sizeof(g_line) - 1) line[len++] = c;
        }
        // other control bytes ignored
    }
}

void consolePoll() {
    static int esc = 0;   // 0 = normal, 1 = saw ESC, 2 = inside CSI "ESC ["
    pollConsoleInput(Serial, g_line, g_len, esc);
#if ESK8OS_DUAL_CONSOLE
    static int esc0 = 0;
    pollConsoleInput(Serial0, g_line0, g_len0, esc0);
#endif
}
