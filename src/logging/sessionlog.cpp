#include "sessionlog.h"
#include "esk8os.h"
#include "telemetry/BatteryView.h"
#include "transports/DalyBms.h"
#include <LittleFS.h>

// Session files live under /sessions as sNNNN.csv. The board has no RTC, so
// filenames use a persistent sequence rather than wall-clock time.
static const char* SESSIONS_DIR = "/sessions";
// Retention is byte-budgeted: BMS-schema rows run ~200 B and one commute
// writes ~0.5 MB, so the old count cap (10 files, sized for ~90 B rows) could
// want more than the partition holds and space-pressure pruning did the
// evicting instead. Prune until a fresh ride always fits, then keep a file-
// count ceiling only as a directory-size bound (stub-first, then oldest —
// bench boots can never evict a real ride while a real ride remains).
static const size_t TARGET_FREE_BYTES = 704 * 1024;  // prune while free is below this
static const int    MAX_SESSION_FILES = 48;          // directory bound, not storage
static const size_t MIN_FREE_BYTES = 96 * 1024;      // hard stop only (nothing left to prune)

static bool          g_mounted = false;
static File          g_file;
static bool          g_active = false;
static char          g_path[40] = "";
static unsigned long g_lastSampleMs = 0;
static int           g_sinceFlush = 0;
static bool          g_logMph = false;   // units locked at session start
static bool          g_enabled = true;
static bool          g_full = false;

#if ESK8OS_BMS_DALY
static const uint32_t BMS_CELL_DETAIL_MS = 10000;
static const uint16_t BMS_DELTA_CHANGE_MV = 25;
static uint32_t g_lastBmsCellDetailMs = 0;
static bool     g_bmsStateSeen = false;
static bool     g_prevBmsLink = false;
static bool     g_prevBmsFault = false;
static bool     g_prevBmsChargeMos = false;
static bool     g_prevBmsDischargeMos = false;
static bool     g_bmsSettingsNoted = false;
static Esk8OS::Transports::BmsAlarmLevel g_prevBmsAlarm =
    Esk8OS::Transports::BMS_ALARM_STALE;
static uint16_t g_prevBmsDeltaMv = 0;
#endif

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

// A session file below this size holds no real ride (header + bench-idle rows
// only; even a ~1-minute ride writes several KB). Eviction removes these
// FIRST so a string of bench boots can never rotate actual ride data off the
// flash — that exact failure destroyed the only board-side copy of a ride that
// was needed for a pack-cutoff investigation (2026-07-03).
static const size_t STUB_MAX_BYTES = 4096;

// Pick the file eviction should remove next: the oldest stub if any exist,
// otherwise the oldest file. Never the active session file.
static String victimSessionFile() {
    File dir = LittleFS.open(SESSIONS_DIR);
    if (!dir || !dir.isDirectory()) return "";
    String active = "";
    if (g_path[0]) {
        active = g_path;
        int slash = active.lastIndexOf('/');
        if (slash >= 0) active = active.substring(slash + 1);
    }
    String oldest = "", oldestStub = "";
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        size_t size = f.size();
        f.close();
        if (!name.endsWith(".csv") || name == active) continue;
        if (oldest == "" || name < oldest) oldest = name;
        if (size < STUB_MAX_BYTES && (oldestStub == "" || name < oldestStub)) oldestStub = name;
    }
    dir.close();
    return oldestStub != "" ? oldestStub : oldest;
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
    while (LittleFS.totalBytes() - LittleFS.usedBytes() < TARGET_FREE_BYTES && guard++ < 64) {
        String victim = victimSessionFile();
        if (victim == "") break;
        String path = String(SESSIONS_DIR) + "/" + victim;
        if (!LittleFS.remove(path)) break;
        Serial.println("[sessionlog] pruned " + path);
    }
}

static void enforceSessionCap() {
    if (!g_mounted) return;
    int guard = 0;
    while (sessionFileCount() > MAX_SESSION_FILES && guard++ < 64) {
        String victim = victimSessionFile();
        if (victim == "") break;
        if (!LittleFS.remove(String(SESSIONS_DIR) + "/" + victim)) break;
        Serial.println("[sessionlog] capped " + victim);
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
#if ESK8OS_BMS_DALY
    g_file.println("# esk8os_session_schema=4;bms_rollup=1s_active+10s_parked+30s_down;bms_cells_cadence=10s+change;bms_oc_settings=comment_once");
    g_file.println(g_logMph
        ? "uptime_s,trip_elapsed_s,trip_moving_s,event,speed_mph,trip_mi,volts,cell_v,watts,batt_a,max_batt_a,motor_a,m1a,m2a,duty,motor_c,esc_c,batt_pct,home_s,limp_s,sag_events,range_warn,fault,vesc_ok,bms_link,bms_pack_v,bms_current_a,bms_soc_pct,bms_remaining_ah,bms_delta_mv,bms_min_cell,bms_min_mv,bms_max_cell,bms_max_mv,bms_max_temp_c,bms_charge_mos,bms_discharge_mos,bms_fault,bms_stale_mask,bms_fault_hex,bms_cells_mv,bms_valid_mask,bms_alarm_level,battery_view_v,battery_view_soc,battery_view_source,battery_soc_provisional,battery_range_home_km"
        : "uptime_s,trip_elapsed_s,trip_moving_s,event,speed_kmh,trip_km,volts,cell_v,watts,batt_a,max_batt_a,motor_a,m1a,m2a,duty,motor_c,esc_c,batt_pct,home_s,limp_s,sag_events,range_warn,fault,vesc_ok,bms_link,bms_pack_v,bms_current_a,bms_soc_pct,bms_remaining_ah,bms_delta_mv,bms_min_cell,bms_min_mv,bms_max_cell,bms_max_mv,bms_max_temp_c,bms_charge_mos,bms_discharge_mos,bms_fault,bms_stale_mask,bms_fault_hex,bms_cells_mv,bms_valid_mask,bms_alarm_level,battery_view_v,battery_view_soc,battery_view_source,battery_soc_provisional,battery_range_home_km");
    g_lastBmsCellDetailMs = 0;
    g_bmsStateSeen = false;
    g_prevBmsLink = false;
    g_prevBmsFault = false;
    g_prevBmsChargeMos = false;
    g_prevBmsDischargeMos = false;
    g_bmsSettingsNoted = false;
    g_prevBmsAlarm = Esk8OS::Transports::BMS_ALARM_STALE;
    g_prevBmsDeltaMv = 0;
#else
    g_file.println("# esk8os_session_schema=1");
    g_file.println(g_logMph
        ? "uptime_s,trip_elapsed_s,trip_moving_s,event,speed_mph,trip_mi,volts,cell_v,watts,batt_a,max_batt_a,motor_a,m1a,m2a,duty,motor_c,esc_c,batt_pct,home_s,limp_s,sag_events,range_warn,fault,vesc_ok"
        : "uptime_s,trip_elapsed_s,trip_moving_s,event,speed_kmh,trip_km,volts,cell_v,watts,batt_a,max_batt_a,motor_a,m1a,m2a,duty,motor_c,esc_c,batt_pct,home_s,limp_s,sag_events,range_warn,fault,vesc_ok");
#endif
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

#if ESK8OS_BMS_DALY
    Esk8OS::Transports::BmsData bms;
    Esk8OS::Transports::getBmsData(&bms);
    if (bms.dischargeOcSettingsValid && !g_bmsSettingsNoted) {
        g_file.printf(
            "# bms_discharge_oc_settings=warning_a:%.1f,"
            "protection1_a:%.1f,protection1_delay_ms:%u,"
            "protection2_a:%.1f,protection2_delay_ms:%u,"
            "source:read_only_modbus_0x145_0x149\n",
            bms.dischargeOcWarningA,
            bms.dischargeOcProtection1A,
            (unsigned)bms.dischargeOcProtection1DelayMs,
            bms.dischargeOcProtection2A,
            (unsigned)bms.dischargeOcProtection2DelayMs);
        g_file.flush();
        g_bmsSettingsNoted = true;
    }
    const Esk8OS::Telemetry::BatteryView battery =
        Esk8OS::Telemetry::getBatteryView();
    uint32_t staleMask = 0;
    const int cells = min((int)bms.cellCount, Esk8OS::Transports::BMS_MAX_CELLS);
    for (int c = 0; c < cells; c++) {
        if (!Esk8OS::Transports::cellFresh(bms, c)) staleMask |= (1UL << c);
    }

    const uint32_t now = millis();
    const Esk8OS::Transports::BmsAlarmLevel alarm =
        Esk8OS::Transports::bmsAlarmLevel(bms);
    const bool linkChanged = g_bmsStateSeen && bms.linkOk != g_prevBmsLink;
    const bool faultChanged = g_bmsStateSeen && bms.hasFault != g_prevBmsFault;
    const bool alarmChanged = g_bmsStateSeen && alarm != g_prevBmsAlarm;
    const bool mosChanged = g_bmsStateSeen &&
                            (bms.chargeMos != g_prevBmsChargeMos ||
                             bms.dischargeMos != g_prevBmsDischargeMos);
    const bool deltaChanged = g_bmsStateSeen && bms.linkOk && g_prevBmsLink &&
                              abs((int)bms.cellDeltamV - (int)g_prevBmsDeltaMv) >= BMS_DELTA_CHANGE_MV;
    const bool cellDetailDue = !g_bmsStateSeen || linkChanged || faultChanged ||
                               alarmChanged || mosChanged || deltaChanged ||
                               now - g_lastBmsCellDetailMs >= BMS_CELL_DETAIL_MS;

    const char* rowEvent = event ? event : "";
    if (!rowEvent[0]) {
        if (linkChanged)       rowEvent = bms.linkOk ? "bms_link_up" : "bms_link_lost";
        else if (alarmChanged) {
            const Esk8OS::Transports::BmsFaultKind faultKind =
                Esk8OS::Transports::bmsFaultKind(bms);
            rowEvent =
                alarm == Esk8OS::Transports::BMS_ALARM_NONE ? "bms_alarm_clear" :
                faultKind == Esk8OS::Transports::BMS_FAULT_DISCHARGE_OVERCURRENT_PROTECTION
                    ? "bms_discharge_oc_protect" :
                faultKind == Esk8OS::Transports::BMS_FAULT_DISCHARGE_OVERCURRENT_WARNING
                    ? "bms_discharge_oc_warn" :
                alarm == Esk8OS::Transports::BMS_ALARM_WARNING ? "bms_warning" :
                alarm == Esk8OS::Transports::BMS_ALARM_ALERT ? "bms_alert" :
                "bms_alarm_stale";
        }
        else if (mosChanged)   rowEvent = "bms_mos";
        else if (faultChanged) rowEvent = bms.hasFault ? "bms_fault_on" : "bms_fault_off";
        else if (deltaChanged) rowEvent = "bms_delta";
    }

    char faultHex[15];
    for (int i = 0; i < 7; i++) snprintf(&faultHex[i * 2], 3, "%02x", bms.faultBytes[i]);

    char cellDetail[112] = "";
    if (cellDetailDue && cells > 0) {
        size_t used = 0;
        for (int c = 0; c < cells && used < sizeof(cellDetail); c++) {
            const int n = snprintf(cellDetail + used, sizeof(cellDetail) - used,
                                   "%s%u", c ? "|" : "", (unsigned)bms.cellmV[c]);
            if (n < 0 || (size_t)n >= sizeof(cellDetail) - used) break;
            used += (size_t)n;
        }
        g_lastBmsCellDetailMs = now;
    }
    g_bmsStateSeen = true;
    g_prevBmsLink = bms.linkOk;
    g_prevBmsFault = bms.hasFault;
    g_prevBmsChargeMos = bms.chargeMos;
    g_prevBmsDischargeMos = bms.dischargeMos;
    g_prevBmsAlarm = alarm;
    g_prevBmsDeltaMv = bms.cellDeltamV;
#else
    const char* rowEvent = event ? event : "";
#endif

    g_file.printf("%u,%u,%u,%s,%.1f,%.2f,%.2f,%.2f,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%d,%u,%u,%d,%d,%d,%d",
                  (unsigned)uptime, (unsigned)tripElapsed, (unsigned)tripMovingSec,
                  rowEvent, spd, dist, currentVoltage, loadedCellVoltage, currentWatts,
                  currentAmps, maxBatteryAmpsSession, currentMotorAmps, gMasterMotorAmps, gSlaveMotorAmps, currentDuty,
                  currentMotorTemp, currentEscTemp, currentBatteryPercent,
                  (unsigned)homeVoltageSecondsSession, (unsigned)limpVoltageSecondsSession,
                  sagEventsSession, rangeAlertState,
                  vescFault, vescLinkOk ? 1 : 0);
#if ESK8OS_BMS_DALY
    g_file.printf(",%d,%.2f,%.1f,%.1f,%.3f,%u,%u,%u,%u,%u,%d,%d,%d,%d,0x%08lx,%s,%s,0x%03x,%u,%.2f,%d,%s,%d,%.2f",
                  bms.linkOk ? 1 : 0, bms.packVoltage, bms.current, bms.soc,
                  bms.remainingAh, (unsigned)bms.cellDeltamV,
                  (unsigned)bms.minCellNo, (unsigned)bms.minCellmV,
                  (unsigned)bms.maxCellNo, (unsigned)bms.maxCellmV,
                  (int)bms.tempMax, bms.chargeMos ? 1 : 0,
                  bms.dischargeMos ? 1 : 0, bms.hasFault ? 1 : 0,
                  (unsigned long)staleMask, faultHex, cellDetail,
                  (unsigned)Esk8OS::Transports::bmsFreshMask(bms),
                  (unsigned)Esk8OS::Transports::bmsAlarmLevel(bms),
                  battery.voltageValid ? battery.packVoltage : 0.0f,
                  battery.socValid ? battery.socPct : 0,
                  Esk8OS::Telemetry::batterySourceName(battery.source),
                  battery.socProvisional ? 1 : 0,
                  battery.rangeValid ? battery.homeRangeKm : 0.0f);
#endif
    g_file.print("\n");
}

void sessionLogMark(const char* event) {
    if (!g_enabled) return;
    if (!g_active) sessionLogStart();
    appendRow(event);
    sessionLogFlush();
}

// Free-text forensics inline with the CSV rows (e.g. the ESC's own fault log,
// captured the moment a fault appears). '#'-prefixed so CSV parsers skip it.
// Flushed immediately — these exist precisely for the moments power might die.
void sessionLogNote(const char* text) {
    if (!g_enabled || !text || !text[0]) return;
    if (!g_active) sessionLogStart();
    if (!g_active || !g_file) return;
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        g_file.print("# ");
        g_file.write((const uint8_t*)p, len);
        g_file.print("\n");
        p += len + (nl ? 1 : 0);
    }
    g_file.flush();
}

void sessionLogTick() {
    if (!g_enabled) return;
    if (!g_active) sessionLogStart();

    if (!g_active || !g_file) return;
    uint32_t sampleIntervalMs = 1000;
    bool urgent = false;
#if ESK8OS_BMS_DALY
    Esk8OS::Transports::BmsData bms;
    Esk8OS::Transports::getBmsData(&bms);
    const Esk8OS::Transports::BmsAlarmLevel alarm =
        Esk8OS::Transports::bmsAlarmLevel(bms);
    urgent = !g_bmsStateSeen ||
             (bms.dischargeOcSettingsValid && !g_bmsSettingsNoted) ||
             bms.linkOk != g_prevBmsLink ||
             bms.hasFault != g_prevBmsFault ||
             alarm != g_prevBmsAlarm ||
             bms.chargeMos != g_prevBmsChargeMos ||
             bms.dischargeMos != g_prevBmsDischargeMos ||
             (bms.linkOk && g_prevBmsLink &&
              abs((int)bms.cellDeltamV - (int)g_prevBmsDeltaMv) >=
                  BMS_DELTA_CHANGE_MV);
    if (!telemetryLive) {
        if (!bms.linkOk) sampleIntervalMs = 30000;          // down-state heartbeat
        else {
            // Charging/balancing used to hold the 1 s "active" cadence here
            // (|current| > 0.5 A) and could write ~900 KB/h into a partition
            // sized for a week of commutes. Parked pack state moves slowly, so
            // only genuine alert conditions keep 1 s; state changes (link,
            // alarm, MOS, delta) still flush an urgent row immediately below.
            const bool alertPack = alarm == Esk8OS::Transports::BMS_ALARM_ALERT ||
                                   bms.cellDeltamV > 100;
            sampleIntervalMs = alertPack ? 1000 : 10000;    // alert vs stable parked
        }
    }
    if (!urgent && millis() - g_lastSampleMs < sampleIntervalMs) return;
#else
    // The original build remains VESC-only: no idle zero rows and no behavior
    // change for riders without a smart BMS.
    if (!telemetryLive) return;
#endif
    g_lastSampleMs = millis();
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
    // Active 1 Hz rides retain the original ten-row flush cadence. Parked/down
    // heartbeats are already sparse, so flush every one; link/alarm/MOS/delta
    // transitions are forensic evidence and must survive an immediate power cut.
    if (urgent || sampleIntervalMs >= 10000 || ++g_sinceFlush >= 10) {
        g_file.flush();
        g_sinceFlush = 0;
    }
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
