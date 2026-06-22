#include "console.h"
#include "esk8os.h"
#include "logging/ridelog.h"
#include "services/webexport.h"
#include <LittleFS.h>

static const char* RIDES_DIR = "/rides";
static char   g_line[96];
static size_t g_len = 0;

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
}

static void dispatch(char* line) {
    char* sp = strchr(line, ' ');
    const char* arg = "";
    if (sp) { *sp = '\0'; arg = sp + 1; }

    if      (!strcmp(line, "help") || !strcmp(line, "?")) cmdHelp();
    else if (!strcmp(line, "logs") || !strcmp(line, "ls")) cmdList();
    else if (!strcmp(line, "cat"))  cmdCat(arg);
    else if (!strcmp(line, "rm"))   cmdRm(arg);
    else if (!strcmp(line, "log"))  cmdLog(arg);
    else if (!strcmp(line, "wifi")) cmdWifi(arg);
    else if (!strcmp(line, "free")) Serial.printf("FS %u/%u B used\n",
                 (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    else if (!strcmp(line, "odo")) {
        if (!strcmp(arg, "reset")) {
            totalDistanceKm = 0.0f;
            saveOdo();                          // persist the cleared lifetime total
            Serial.println("odo reset -> 0");
        } else if (!strncmp(arg, "set ", 4)) {
            float v = atof(arg + 4);            // value in the active display unit
            if (v >= 0) {
                totalDistanceKm = useMph ? v / 0.621371f : v;
                saveOdo();
                Serial.printf("odo set -> %.2f %s\n", v, useMph ? "mi" : "km");
            } else {
                Serial.println("odo set: need a value >= 0");
            }
        } else if (useMph) {
            Serial.printf("odo %.2f mi | trip %.2f mi\n",
                 totalDistanceKm * 0.621371f, tripDistanceKm * 0.621371f);
        } else {
            Serial.printf("odo %.2f km | trip %.2f km\n",
                 totalDistanceKm, tripDistanceKm);
        }
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
                dispatch(g_line);
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
