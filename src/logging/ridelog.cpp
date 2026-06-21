#include "ridelog.h"
#include "esk8os.h"
#include <LittleFS.h>

// Ride files live under /rides as rNNNN.csv, named by a persistent sequence
// number (the board has no RTC, so we can't timestamp them by wall-clock).
static const char* RIDES_DIR = "/rides";
// Keep at most this many ride CSVs (matches the on-device LOGS view; oldest
// pruned as new rides start). The partition holds far more, but there's no point
// keeping detailed logs we can't surface.
static const int    MAX_RIDE_FILES = 10;
// Safety net: also prune oldest rides if free space drops below this (a single
// multi-hour ride could be large), independent of the file-count cap.
static const size_t MIN_FREE_BYTES = 96 * 1024;

static bool          g_mounted = false;
static File          g_file;
static bool          g_active = false;
static bool          g_pending = false;  // ride armed but file not created yet (lazy)
static unsigned long g_lastSampleMs = 0;
static unsigned long g_t0 = 0;           // ms at first movement -> CSV t_s baseline
static int           g_sinceFlush = 0;
static bool          g_logMph = false;   // units locked at ride start (file stays consistent)
static bool          g_enabled = true;   // runtime master switch (debug/space control)
static bool          g_full = false;     // true once auto-stopped on low space

void ridelogBegin() {
    // format-on-fail so a blank/garbage partition is made usable on first boot.
    g_mounted = LittleFS.begin(true);
    if (!g_mounted) {
        Serial.println("[ridelog] LittleFS mount failed - logging disabled");
        return;
    }
    if (!LittleFS.exists(RIDES_DIR)) LittleFS.mkdir(RIDES_DIR);
}

bool ridelogReady() { return g_mounted; }

// Lowest-numbered (oldest) ride file currently on disk, "" if none.
static String oldestRideFile() {
    File dir = LittleFS.open(RIDES_DIR);
    if (!dir || !dir.isDirectory()) return "";
    String oldest = "";
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();   // bare filename, e.g. "r0007.csv"
        if (name.endsWith(".csv")) {
            if (oldest == "" || name < oldest) oldest = name;
        }
        f.close();
    }
    dir.close();
    return oldest;
}

static int rideFileCount() {
    File dir = LittleFS.open(RIDES_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (String(f.name()).endsWith(".csv")) n++;
        f.close();
    }
    dir.close();
    return n;
}

// Delete oldest ride files until at least MIN_FREE_BYTES is free (or none left).
static void pruneIfNeeded() {
    if (!g_mounted) return;
    int guard = 0;   // never loop forever, even if deletes don't free space
    while (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_BYTES && guard++ < 64) {
        String oldest = oldestRideFile();
        if (oldest == "") break;
        String path = String(RIDES_DIR) + "/" + oldest;
        if (!LittleFS.remove(path)) break;
        Serial.println("[ridelog] pruned " + path);
    }
}

// Keep only the MAX_RIDE_FILES newest CSVs. The active (newest) file is never the
// oldest, so its open handle is safe. Call after a new ride file is created.
static void enforceRideCap() {
    if (!g_mounted) return;
    int guard = 0;
    while (rideFileCount() > MAX_RIDE_FILES && guard++ < 64) {
        String oldest = oldestRideFile();
        if (oldest == "") break;
        if (!LittleFS.remove(String(RIDES_DIR) + "/" + oldest)) break;
        Serial.println("[ridelog] capped " + oldest);
    }
}

void ridelogDeleteAll() {
    if (!g_mounted) return;
    ridelogEndRide();          // release the open FD so the active file can go too
    File dir = LittleFS.open(RIDES_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String p = String(RIDES_DIR) + "/" + f.name();
            f.close();
            LittleFS.remove(p);
        }
        dir.close();
    }
    ridelogStartRide();        // begin a fresh ride immediately
}

void ridelogEndRide() {
    if (g_file) {
        g_file.flush();
        g_file.close();
    }
    g_active = false;
    g_pending = false;
}

// Arm a new ride. The CSV file is NOT created here — it's created lazily on the
// first movement (see ridelogTick), so powering the board on/off without riding
// (picking it up, setting it down) never burns a ride slot with an empty file.
void ridelogStartRide() {
    if (!g_mounted || !g_enabled) return;
    ridelogEndRide();          // close any ride still open
    g_pending = true;          // wait for movement before opening a file
    g_full = false;            // a fresh ride armed -> clear any prior low-space stop
    g_lastSampleMs = 0;
    g_sinceFlush = 0;
}

// Actually create the ride file (called from the first moving sample). Locks the
// CSV units + the t_s=0 baseline to this moment.
static void openRideFile() {
    pruneIfNeeded();
    uint32_t seq = prefs.getUInt("rideSeq", 1);
    char path[40];
    snprintf(path, sizeof(path), "%s/r%04u.csv", RIDES_DIR, (unsigned)(seq % 10000));

    g_file = LittleFS.open(path, "w");
    if (!g_file) {
        Serial.printf("[ridelog] open failed: %s\n", path);
        g_pending = false;
        return;
    }
    // Speed + distance follow the display's chosen units, locked here so the file
    // stays consistent even if units are toggled mid-ride.
    g_logMph = useMph;
    g_file.println(g_logMph
        ? "t_s,speed_mph,dist_mi,volts,watts,batt_a,motor_a,duty,motor_c,esc_c,batt_pct"
        : "t_s,speed_kmh,dist_km,volts,watts,batt_a,motor_a,duty,motor_c,esc_c,batt_pct");
    g_file.flush();
    g_active = true;
    g_pending = false;
    g_t0 = millis();           // t_s is seconds since the ride actually started moving
    prefs.putUInt("rideSeq", seq + 1);
    Serial.printf("[ridelog] logging to %s\n", path);
    enforceRideCap();          // keep only the newest MAX_RIDE_FILES rides
}

void ridelogTick() {
    if (!g_enabled) return;
    if (millis() - g_lastSampleMs < 1000) return;       // 1 Hz
    if (currentSpeedKmh < 0.5f) return;                  // only log while moving
    g_lastSampleMs = millis();

    if (g_pending) openRideFile();   // lazy: create the file on the first movement
    if (!g_active || !g_file) return;

    // Low-space failsafe: try to reclaim space from old rides; if still low (the
    // active file is the bulk), stop logging and flag it so the UI can warn.
    if (ridelogFreeBytes() < MIN_FREE_BYTES) {
        pruneIfNeeded();
        if (ridelogFreeBytes() < MIN_FREE_BYTES) {
            Serial.println("[ridelog] storage low - logging STOPPED");
            ridelogEndRide();
            g_full = true;
            return;
        }
    }

    uint32_t t = (millis() - g_t0) / 1000;
    float spd  = g_logMph ? currentSpeedMph : currentSpeedKmh;
    float dist = g_logMph ? tripDistanceKm * 0.621371f : tripDistanceKm;
    g_file.printf("%u,%.1f,%.2f,%.2f,%.0f,%.1f,%.1f,%.0f,%.0f,%.0f,%d\n",
                  (unsigned)t, spd, dist, currentVoltage, currentWatts,
                  currentAmps, currentMotorAmps, currentDuty,
                  currentMotorTemp, currentEscTemp, currentBatteryPercent);

    // Commit roughly every 10s of riding so a sudden power-off loses little.
    if (++g_sinceFlush >= 10) { g_file.flush(); g_sinceFlush = 0; }
}

void ridelogSetEnabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    if (on) ridelogStartRide();   // resume on a fresh file (also clears g_full)
    else    ridelogEndRide();     // stop + close the current file
}

bool ridelogEnabled() { return g_enabled; }
bool ridelogFull() { return g_full; }
size_t ridelogFreeBytes() {
    return g_mounted ? (LittleFS.totalBytes() - LittleFS.usedBytes()) : 0;
}
