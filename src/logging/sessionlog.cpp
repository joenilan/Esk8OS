#include "sessionlog.h"
#include "esk8os.h"
#include <LittleFS.h>

// Session files live under /sessions as sNNNN.csv. The board has no RTC, so
// filenames use a persistent sequence rather than wall-clock time.
static const char* SESSIONS_DIR = "/sessions";
static const int    MAX_SESSION_FILES = 10;
static const size_t MIN_FREE_BYTES = 96 * 1024;

static bool          g_mounted = false;
static File          g_file;
static bool          g_active = false;
static char          g_path[40] = "";
static unsigned long g_lastSampleMs = 0;
static int           g_sinceFlush = 0;
static bool          g_logMph = false;   // units locked at session start
static bool          g_enabled = true;
static bool          g_full = false;

void sessionLogBegin() {
    // format-on-fail so a blank/garbage partition is made usable on first boot.
    g_mounted = LittleFS.begin(true);
    if (!g_mounted) {
        Serial.println("[sessionlog] LittleFS mount failed - logging disabled");
        return;
    }
    if (!LittleFS.exists(SESSIONS_DIR)) LittleFS.mkdir(SESSIONS_DIR);
}

bool sessionLogReady() { return g_mounted; }

static String oldestSessionFile() {
    File dir = LittleFS.open(SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return "";
    String oldest = "";
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        if (name.endsWith(".csv")) {
            if (oldest == "" || name < oldest) oldest = name;
        }
        f.close();
    }
    dir.close();
    return oldest;
}

static int sessionFileCount() {
    File dir = LittleFS.open(SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (String(f.name()).endsWith(".csv")) n++;
        f.close();
    }
    dir.close();
    return n;
}

static void pruneIfNeeded() {
    if (!g_mounted) return;
    int guard = 0;
    while (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_BYTES && guard++ < 64) {
        String oldest = oldestSessionFile();
        if (oldest == "") break;
        String path = String(SESSIONS_DIR) + "/" + oldest;
        if (!LittleFS.remove(path)) break;
        Serial.println("[sessionlog] pruned " + path);
    }
}

static void enforceSessionCap() {
    if (!g_mounted) return;
    int guard = 0;
    while (sessionFileCount() > MAX_SESSION_FILES && guard++ < 64) {
        String oldest = oldestSessionFile();
        if (oldest == "") break;
        if (!LittleFS.remove(String(SESSIONS_DIR) + "/" + oldest)) break;
        Serial.println("[sessionlog] capped " + oldest);
    }
}

void sessionLogFlush() {
    if (g_file) g_file.flush();
}

void sessionLogEnd() {
    if (g_file) {
        g_file.flush();
        g_file.close();
    }
    g_active = false;
    g_path[0] = '\0';
}

void sessionLogStart() {
    if (!g_mounted || !g_enabled || g_active) return;
    // Storage previously found full: don't re-walk /sessions + spam serial on
    // every tick — retry at most every 30s (files may have been deleted via web).
    static unsigned long lastFullRetryMs = 0;
    if (g_full) {
        if (millis() - lastFullRetryMs < 30000UL) return;
        lastFullRetryMs = millis();
    }
    pruneIfNeeded();
    if (sessionLogFreeBytes() < MIN_FREE_BYTES) {
        Serial.println("[sessionlog] storage low - logging STOPPED");
        g_full = true;
        return;
    }

    uint32_t seq = prefs.getUInt("sessionSeq", 1);
    snprintf(g_path, sizeof(g_path), "%s/s%04u.csv", SESSIONS_DIR, (unsigned)(seq % 10000));

    g_file = LittleFS.open(g_path, "w");
    if (!g_file) {
        Serial.printf("[sessionlog] open failed: %s\n", g_path);
        g_path[0] = '\0';
        return;
    }

    g_logMph = useMph;
    g_file.println(g_logMph
        ? "uptime_s,trip_elapsed_s,trip_moving_s,event,speed_mph,trip_mi,volts,cell_v,watts,batt_a,max_batt_a,motor_a,m1a,m2a,duty,motor_c,esc_c,batt_pct,home_s,limp_s,sag_events,range_warn,fault,vesc_ok"
        : "uptime_s,trip_elapsed_s,trip_moving_s,event,speed_kmh,trip_km,volts,cell_v,watts,batt_a,max_batt_a,motor_a,m1a,m2a,duty,motor_c,esc_c,batt_pct,home_s,limp_s,sag_events,range_warn,fault,vesc_ok");
    g_file.flush();
    g_active = true;
    g_full = false;
    g_lastSampleMs = 0;
    g_sinceFlush = 0;
    prefs.putUInt("sessionSeq", seq + 1);
    Serial.printf("[sessionlog] logging to %s\n", g_path);
    enforceSessionCap();
    sessionLogMark("boot");
}

static void appendRow(const char* event) {
    if (!g_active || !g_file) return;

    uint32_t uptime = millis() / 1000UL;
    uint32_t tripElapsed = (millis() - rideStartMs) / 1000UL;
    float spd  = g_logMph ? currentSpeedMph : currentSpeedKmh;
    float dist = g_logMph ? tripDistanceKm * 0.621371f : tripDistanceKm;
    g_file.printf("%u,%u,%u,%s,%.1f,%.2f,%.2f,%.2f,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%d,%u,%u,%d,%d,%d,%d\n",
                  (unsigned)uptime, (unsigned)tripElapsed, (unsigned)tripMovingSec,
                  event ? event : "", spd, dist, currentVoltage, loadedCellVoltage, currentWatts,
                  currentAmps, maxBatteryAmpsSession, currentMotorAmps, gMasterMotorAmps, gSlaveMotorAmps, currentDuty,
                  currentMotorTemp, currentEscTemp, currentBatteryPercent,
                  (unsigned)homeVoltageSecondsSession, (unsigned)limpVoltageSecondsSession,
                  sagEventsSession, rangeAlertState,
                  vescFault, vescLinkOk ? 1 : 0);
}

void sessionLogMark(const char* event) {
    if (!g_enabled) return;
    if (!g_active) sessionLogStart();
    appendRow(event);
    sessionLogFlush();
}

void sessionLogTick() {
    if (!g_enabled) return;
    if (!g_active) sessionLogStart();
    if (millis() - g_lastSampleMs < 1000) return;
    g_lastSampleMs = millis();

    if (!g_active || !g_file) return;
    // Skip periodic rows when telemetry isn't live: idle/no-VESC samples are just
    // masked zeros, and the resulting flash writes stall the UI core (Core 1),
    // seen as screensaver/render hiccups. Resumes automatically when the VESC link
    // returns. Explicit events (sessionLogMark) still log regardless.
    if (!telemetryLive) return;
    if (sessionLogFreeBytes() < MIN_FREE_BYTES) {
        pruneIfNeeded();
        if (sessionLogFreeBytes() < MIN_FREE_BYTES) {
            Serial.println("[sessionlog] storage low - logging STOPPED");
            sessionLogEnd();
            g_full = true;
            return;
        }
    }

    appendRow("");
    if (++g_sinceFlush >= 10) { g_file.flush(); g_sinceFlush = 0; }
}

void sessionLogDeleteAll() {
    if (!g_mounted) return;
    sessionLogEnd();
    File dir = LittleFS.open(SESSIONS_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String p = String(SESSIONS_DIR) + "/" + f.name();
            f.close();
            LittleFS.remove(p);
        }
        dir.close();
    }
    g_full = false;   // space was just reclaimed — let sessionLogStart retry immediately
    sessionLogStart();
}

void sessionLogSetEnabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    if (on) sessionLogStart();
    else    sessionLogEnd();
}

bool sessionLogEnabled() { return g_enabled; }
bool sessionLogFull() { return g_full; }
size_t sessionLogFreeBytes() {
    return g_mounted ? (LittleFS.totalBytes() - LittleFS.usedBytes()) : 0;
}
