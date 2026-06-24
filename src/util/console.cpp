#include "console.h"
#include "esk8os.h"
#include "app/App.h"
#include "version.h"
#include "logging/ridelog.h"
#include "services/webexport.h"
#include <LittleFS.h>
#include <esp_system.h>

// Forward-declared (defined in App.cpp, external linkage) — re-applies the
// backlight from gBrightnessPct after the `bright` command changes it.
void applyBrightness();

static const char* RIDES_DIR = "/rides";
static char   g_line[96];
static size_t g_len = 0;

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
    Serial.printf("%s -- confirm? [y/N]\n", what);
    return false;
}

// Resolve a user-typed name to a full path: bare names go under /rides.
static String resolvePath(const char* arg) {
    String a = arg; a.trim();
    if (a.startsWith("/")) return a;
    return String(RIDES_DIR) + "/" + a;
}

static void cmdList() {
    File dir = LittleFS.open(RIDES_DIR);
    if (!dir || !dir.isDirectory()) { Serial.println("(no rides yet)"); return; }
    int n = 0; size_t total = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        Serial.printf("  %-14s %8u B\n", f.name(), (unsigned)f.size());
        total += f.size(); n++;
        f.close();
    }
    dir.close();
    Serial.printf("%d file(s), %u B in /rides | FS %u/%u B used\n",
                  n, (unsigned)total,
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

static void cmdCat(const char* arg) {
    String path = resolvePath(arg);
    File f = LittleFS.open(path, "r");
    if (!f) { Serial.println("not found: " + path); return; }
    Serial.printf("----- %s (%u B) -----\n", path.c_str(), (unsigned)f.size());
    uint8_t buf[128];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, n);
    f.close();
    Serial.println("----- end -----");
}

static void cmdRm(const char* arg) {
    String a = arg; a.trim();
    if (a == "") { Serial.println("usage: rm <file|all>"); return; }
    if (a == "all") {
        // Goes through ridelog so the actively-logged file is closed first
        // (you can't unlink a file with an open handle), then a fresh ride starts.
        ridelogDeleteAll();
        Serial.println("cleared all ride files");
        return;
    }
    String path = resolvePath(arg);
    Serial.println(LittleFS.remove(path) ? "removed " + path : "failed: " + path);
}

static void cmdLog(const char* arg) {
    String a = arg; a.trim();
    if (a == "on")  { ridelogSetEnabled(true);  Serial.println("logging ON"); return; }
    if (a == "off") { ridelogSetEnabled(false); Serial.println("logging OFF"); return; }
    Serial.printf("logging %s%s | %u KB free\n",
                  ridelogEnabled() ? "ON" : "OFF",
                  ridelogFull() ? " (auto-stopped: low space)" : "",
                  (unsigned)(ridelogFreeBytes() / 1024));
}

// Toggle the standalone log/OTA web service from USB serial — a test path for
// the unbridged "hybrid" flow (otherwise only reachable via the BLE command).
static void cmdWifi(const char* arg) {
    String a = arg; a.trim();
    if (a == "on") {
        if (systemMode != MODE_DASHBOARD) { Serial.println("can't: exit bridge mode first"); return; }
        if (webServiceActive()) { Serial.println("wifi export already ON"); return; }
        webServiceStart();
        Serial.println("wifi export ON - join ESK8-BRIDGE / esk8bridge, then http://192.168.4.1");
        return;
    }
    if (a == "off") { webServiceStop(); Serial.println("wifi export OFF"); return; }
    Serial.printf("wifi export %s\n", webServiceActive() ? "ON" : "OFF");
}

// ---- debug / status dumps ---------------------------------------------------
static void cmdStat() {
    const char* su = useMph ? "mph" : "kmh";
    float spd = useMph ? currentSpeedMph : currentSpeedKmh;
    Serial.printf("spd %.1f %s | batt %d%% | %.1fV | link %s | fault %d\n",
        spd, su, currentBatteryPercent, currentVoltage, vescLinkOk ? "OK" : "DOWN", vescFault);
    Serial.printf("pwr %dW (peak %dW) | batA %.1f | motA %.1f | duty %d%%\n",
        (int)currentWatts, (int)peakWatts, currentAmps, currentMotorAmps, (int)currentDuty);
    Serial.printf("temp motor %dC | esc %dC | batt %dC\n",
        (int)currentMotorTemp, (int)currentEscTemp, (int)currentBatteryTemp);
    Serial.printf("energy %dWh used | %dWh regen\n", (int)currentWattHours, (int)currentWhRegen);
}

static void cmdDiag() {
    Serial.printf("remote: %s | throttle %+.2f (%s) | pulse %.2f ms\n",
        gPpmConnected ? "CONNECTED" : "no signal", gPpmDecoded,
        gPpmDecoded > 0.02f ? "accel" : (gPpmDecoded < -0.02f ? "brake" : "center"), gPpmPulseMs);
    Serial.printf("vesc fw %u.%u | slave(CAN) %s | fault %d | last-fault %d\n",
        gVescFwMajor, gVescFwMinor, gSlaveOnline ? "online" : "offline", vescFault, gLastFault);
    Serial.printf("motor A: master %.1f | slave %.1f\n", gMasterMotorAmps, gSlaveMotorAmps);
    Serial.printf("temps C: motor m %.0f/s %.0f | esc m %.0f/s %.0f\n",
        gMasterMotorTemp, gSlaveMotorTemp, gMasterEscTemp, gSlaveEscTemp);
}

static void cmdTrip(const char* arg) {
    if (!strcmp(arg, "reset")) {
        Esk8OS::App::resetTrip();
        Serial.println("trip reset (distance + moving-time + session metrics)");
        return;
    }
    float cv = useMph ? 0.621371f : 1.0f;
    const char* u  = useMph ? "mi"  : "km";
    const char* su = useMph ? "mph" : "kmh";
    Serial.printf("trip %.2f %s | tmov %02u:%02u:%02u | odo %.2f %s\n",
        tripDistanceKm * cv, u, tripMovingSec / 3600, (tripMovingSec / 60) % 60, tripMovingSec % 60,
        totalDistanceKm * cv, u);
    Serial.printf("avg %.1f %s | max %.1f %s | range est %.1f %s (rem %.1f) %s\n",
        avgSpeedKmh * cv, su, maxSpeedKmh * cv, su, estimatedRangeKm * cv, u, remainingRangeKm * cv,
        rangeEstimateReady ? "[learned]" : "[default]");
}

static void cmdSys() {
    unsigned long up = millis() / 1000;
    Serial.printf("fw %s\n", FW_VERSION_FULL);
    Serial.printf("uptime %02lu:%02lu:%02lu | reset-reason %d | fps %u\n",
        up / 3600, (up / 60) % 60, up % 60, (int)esp_reset_reason(), gFps);
    Serial.printf("heap %u B free (min %u) | psram %u/%u B free\n",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
        (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
}

static void cmdCfg() {
    Serial.printf("units %s | demo %s | bright %d%% | theme %d | rider %s\n",
        useMph ? "mph" : "kmh", gDemoMode ? "ON" : "OFF", gBrightnessPct, gThemeIdx, RIDER_NAME);
    Serial.printf("battery %d cells | pack %.1f Ah | stop %.2f V/cell | %.0f Wh/mi\n",
        BATTERY_CELLS_COUNT, BATTERY_EFFECTIVE_CAPACITY_AH, BATTERY_STOP_CELL_V, RANGE_DEFAULT_WH_PER_MILE);
    WheelProfile& w = wheelProfiles[activeWheelProfile];
    Serial.printf("wheel prof %d: %s (%.0f mm, %d/%d, %.1f pp)\n",
        activeWheelProfile, w.name, w.wheelDiameterM * 1000.0f, w.motorPulley, w.wheelPulley, w.polePairs);
}

// ---- actions ----------------------------------------------------------------
static void cmdDemo(const char* arg) {
    String a = arg; a.trim();
    if (a == "on" || a == "off") {
        gDemoMode = (a == "on");
        prefs.putBool("demo", gDemoMode);
        rideEnergyBaselineSet = false;     // re-baseline VESC energy on the next real poll
    }
    Serial.printf("demo %s\n", gDemoMode ? "ON" : "OFF");
}

static void cmdUnits(const char* arg) {
    String a = arg; a.trim();
    if (a == "mph" || a == "kmh" || a == "km") {
        useMph = (a == "mph");
        prefs.putBool("mph", useMph);
        gRedrawAll = true;
    }
    Serial.printf("units %s\n", useMph ? "mph" : "kmh");
}

static void cmdBright(const char* arg) {
    String a = arg; a.trim();
    if (a.length()) {
        gBrightnessPct = constrain(a.toInt(), 10, 100);
        prefs.putInt("bright", gBrightnessPct);
        applyBrightness();
    }
    Serial.printf("bright %d%%\n", gBrightnessPct);
}

static void cmdRider(const char* arg) {
    String a = arg; a.trim();
    if (a.length()) {
        strlcpy(RIDER_NAME, a.c_str(), sizeof(RIDER_NAME));
        prefs.putString("rider", RIDER_NAME);
        gRedrawAll = true;
    }
    Serial.printf("rider %s\n", RIDER_NAME);
}

static void cmdHelp() {
    Serial.println(F("commands:"));
    Serial.println(F("  help            this list"));
    Serial.println(F("  logs            list ride files + storage use"));
    Serial.println(F("  cat <file>      dump a ride CSV (e.g. cat r0001.csv)"));
    Serial.println(F("  rm <file|all>   delete one ride file, or all"));
    Serial.println(F("  log [on|off]    logging switch / status"));
    Serial.println(F("  wifi [on|off]   standalone log/OTA web service (http://192.168.4.1)"));
    Serial.println(F("  free            partition usage"));
    Serial.println(F("  odo [reset|set <v>]  odometer + trip (reset=0, set <v> in display unit)"));
    Serial.println(F("  stat            live telemetry (speed/power/temps/energy)"));
    Serial.println(F("  diag            remote/PPM throttle + VESC diagnostics"));
    Serial.println(F("  trip [reset]    trip distance/time/avg/max/range, or full reset"));
    Serial.println(F("  sys             fw, uptime, heap/psram, reset reason, fps"));
    Serial.println(F("  cfg             units/demo/brightness/battery/wheel config"));
    Serial.println(F("  demo [on|off]   simulate telemetry (persisted)"));
    Serial.println(F("  units [mph|kmh] display units (persisted)"));
    Serial.println(F("  bright <10-100> backlight % (persisted)"));
    Serial.println(F("  rider [name]    rider name (persisted)"));
    Serial.println(F("  reboot          restart the board"));
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
            if (!strcmp(arg, "all")) strlcpy(msg, "delete ALL ride files", sizeof(msg));
            else snprintf(msg, sizeof(msg), "delete ride file '%s'", arg);
            if (!needConfirm(msg, orig)) return;
        }
        cmdRm(arg);
    }
    else if (!strcmp(line, "log"))  cmdLog(arg);
    else if (!strcmp(line, "wifi")) cmdWifi(arg);
    else if (!strcmp(line, "free")) Serial.printf("FS %u/%u B used\n",
                 (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    else if (!strcmp(line, "odo")) {
        if (!strcmp(arg, "reset")) {
            if (!needConfirm("reset the LIFETIME odometer to 0", orig)) return;
            totalDistanceKm = 0.0f;
            saveOdo();                          // persist the cleared lifetime total
            Serial.println("odo reset -> 0");
        } else if (!strncmp(arg, "set ", 4)) {
            float v = atof(arg + 4);            // value in the active display unit
            if (v >= 0) {
                if (!needConfirm("overwrite the lifetime odometer", orig)) return;
                totalDistanceKm = useMph ? v / 0.621371f : v;
                saveOdo();
                Serial.printf("odo set -> %.2f %s\n", v, useMph ? "mi" : "km");
            } else {
                Serial.println("odo set: need a value >= 0");
            }
        } else {
            float cv = useMph ? 0.621371f : 1.0f;
            const char* u = useMph ? "mi" : "km";
            Serial.printf("odo %.2f %s | trip %.2f %s | tmov %02u:%02u:%02u\n",
                 totalDistanceKm * cv, u, tripDistanceKm * cv, u,
                 tripMovingSec / 3600, (tripMovingSec / 60) % 60, tripMovingSec % 60);
        }
    }
    else if (!strcmp(line, "stat") || !strcmp(line, "tel")) cmdStat();
    else if (!strcmp(line, "diag")) cmdDiag();
    else if (!strcmp(line, "trip")) {
        if (!strcmp(arg, "reset") && !needConfirm("reset the trip (distance + moving-time)", orig)) return;
        cmdTrip(arg);
    }
    else if (!strcmp(line, "sys"))  cmdSys();
    else if (!strcmp(line, "cfg") || !strcmp(line, "config")) cmdCfg();
    else if (!strcmp(line, "demo")) cmdDemo(arg);
    else if (!strcmp(line, "units")) cmdUnits(arg);
    else if (!strcmp(line, "bright")) cmdBright(arg);
    else if (!strcmp(line, "rider")) cmdRider(arg);
    else if (!strcmp(line, "reboot")) {
        if (!needConfirm("reboot the board", orig)) return;
        Serial.println("rebooting...");
        Serial.flush();
        delay(100);
        ESP.restart();
    }
    else if (line[0]) Serial.println("? unknown - try 'help'");
}

void consolePoll() {
    // Skip ANSI escape sequences (arrow keys etc. send ESC [ <final>), so cursor
    // movement in the terminal doesn't leak control bytes into a command.
    static int esc = 0;   // 0 = normal, 1 = saw ESC, 2 = inside CSI "ESC ["

    while (Serial.available()) {
        char c = Serial.read();

        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) { if (c >= 0x40 && c <= 0x7e) esc = 0; continue; }  // CSI final byte
        if (c == 0x1b) { esc = 1; continue; }                            // ESC

        if (c == '\r' || c == '\n') {
            // End of line on any terminator (\r, \n, or \r\n). The trailing
            // terminator of a \r\n pair sees an empty buffer and is ignored,
            // so a command never dispatches twice.
            if (g_len > 0) {
                g_line[g_len] = '\0';
                if (g_pending[0]) {
                    // Awaiting a [y/N] answer to a stashed destructive command.
                    // Anything but y/yes cancels (safe default).
                    bool yes = (g_line[0] == 'y' || g_line[0] == 'Y');
                    char cmd[96];
                    strlcpy(cmd, g_pending, sizeof(cmd));
                    g_pending[0] = '\0';
                    if (yes) { g_confirmed = true; dispatch(cmd); g_confirmed = false; }
                    else Serial.println("cancelled");
                } else {
                    dispatch(g_line);
                }
                g_len = 0;
            }
        } else if (c == 0x08 || c == 0x7f) {     // backspace / delete
            if (g_len > 0) g_len--;
        } else if (c >= 0x20 && (uint8_t)c < 0x7f) {   // printable ASCII only
            if (g_len < sizeof(g_line) - 1) g_line[g_len++] = c;
        }
        // other control bytes ignored
    }
}
