#include "transports/VescUartTransport.h"
#include <Arduino.h>

#ifndef WOKWI_SIMULATION
#include "transports/VescProtocol.h"
#include "services/remote_link.h"
#include <LittleFS.h>
#endif

#if EVEE_LINK_ENABLED
#include <evee_link.h>
#endif

namespace Esk8OS {
namespace Transports {

static RawVescData gRawData;
static bool gHasNewData = false;
static SemaphoreHandle_t gDataMutex = NULL;
static bool gPollPaused = false;
static TaskHandle_t gVescTaskHandle = NULL;

// First CAN ID tried when hunting for the second motor's VESC: the id learned
// on a previous session (passed in from NVS at begin; 0 = never found one).
// Only an ordering hint — the sweep still auto-detects any id.
static uint8_t gSlaveHint = 0;

#ifndef WOKWI_SIMULATION

static VescProtocol gProto;

// Which telemetry path the connected ESC supports, decided by probing after
// every link-up. MODERN = COMM_GET_VALUES_SETUP_SELECTIVE (FW ~3.6+): one
// CAN-aggregated packet with speed/battery/odometer extras. LEGACY = plain
// COMM_GET_VALUES with manual master+slave summing, for ancient firmware.
enum ProtoPath { PATH_UNKNOWN, PATH_MODERN, PATH_LEGACY };
static ProtoPath gPath = PATH_UNKNOWN;
static VescFwInfo gFw;

// Slave auto-detection state. gScanIdx walks the candidate order (hint first,
// then 1..253 skipping the master's own ID); the sweep runs one 50 ms probe
// per cycle so it never stalls telemetry, and stops for good once the sweep
// completes on a genuinely single-motor board.
static uint8_t gLocalId = 0;
static uint8_t gSlaveId = 0;
static bool    gSlaveSearchDone = false;
static int     gScanIdx = 0;

static uint8_t scanCandidate(int idx) {
    if (gSlaveHint == 0) {                       // nothing learned yet: plain 1..253 sweep
        int id = idx + 1;
        return (id <= 253) ? (uint8_t)id : 0;
    }
    if (idx == 0) return gSlaveHint;
    // Then 1..253 ascending, skipping the hint (already tried). The master's
    // own ID is skipped by the caller.
    int id = idx;   // idx >= 1
    if (id >= gSlaveHint) id++;
    return (id <= 253) ? (uint8_t)id : 0;
}

static void scanForSlave() {
    if (gSlaveId != 0 || gSlaveSearchDone) return;
    uint8_t candidate = scanCandidate(gScanIdx++);
    if (candidate == 0) { gSlaveSearchDone = true; return; }
    if (candidate == gLocalId) return;                    // next cycle tries the next id
    if (gProto.probeCanId(candidate)) gSlaveId = candidate;
}

// Fields pulled from the aggregated setup-values packet each cycle.
static const uint32_t SETUP_MASK =
    VESC_SETUP_TEMP_FET | VESC_SETUP_TEMP_MOTOR |
    VESC_SETUP_CURRENT_TOT | VESC_SETUP_CURRENT_IN_TOT |
    VESC_SETUP_DUTY | VESC_SETUP_RPM | VESC_SETUP_SPEED | VESC_SETUP_V_IN |
    VESC_SETUP_BATT_LEVEL | VESC_SETUP_WH_TOT | VESC_SETUP_WH_CHG_TOT |
    VESC_SETUP_FAULT | VESC_SETUP_CONTROLLER_ID | VESC_SETUP_NUM_VESCS |
    VESC_SETUP_WH_LEFT | VESC_SETUP_ODOMETER;

// Per-slave read: enough to split the per-motor diag values out of the
// aggregate, and to sum manually when the ESC can't aggregate for us.
static const uint32_t SLAVE_MASK =
    VESC_VAL_TEMP_FET | VESC_VAL_TEMP_MOTOR |
    VESC_VAL_CURRENT_MOTOR | VESC_VAL_CURRENT_IN |
    VESC_VAL_WH | VESC_VAL_WH_CHARGED | VESC_VAL_FAULT;

static uint32_t gPublishes = 0;
static float gLastVin = 0;

// ---- raw COMM_GET_MCCONF capture ---------------------------------------------
// One-shot per boot, attempted only after a successful telemetry cycle so it
// runs on a link that is provably up. Lives in this task (not the console)
// because the console is USB-side and the pack/USB combination isn't available
// for live probing on this board — the bytes are persisted to /mcconf.hex and
// read back later instead. File write from this task is safe: esp_littlefs
// serializes VFS access internally, and it's a different file from the session
// log. NO parsing here by design — the mcconf layout is firmware-version-
// specific (see docs/VESC_CONFIG_READ_HANDOFF.md).
static uint8_t gMcconfRaw[1024];
static volatile int     gMcconfLen = 0;
static volatile uint8_t gMcconfStatus = 0;    // 0 waiting, 1 captured, 2 gave up
static uint8_t gMcconfAttempts = 0;
static const uint8_t MCCONF_MAX_ATTEMPTS = 10;

// ---- mcconf base-config extraction -----------------------------------------
// Field offsets are valid ONLY for MCCONF_SIGNATURE 1065524471 (0x3F829CF7,
// bldc release_6_05 / "FW 6.5"). They were derived by walking confgenerator.c's
// serializer against a real 477-byte capture (docs/captures/, consumed exactly)
// — see docs/VESC_CONFIG_READ_HANDOFF.md. Any other signature: valid=false.
static const uint32_t MCCONF_SIG_6_05 = 1065524471UL;

static VescBaseConfig gVescBase;   // zeroed => valid=false

static uint32_t beU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// bldc buffer_get_float32_auto (sign/exp/mantissa, NOT IEEE memcpy).
static float beF32Auto(const uint8_t* p) {
    uint32_t res = beU32(p);
    int e = (res >> 23) & 0xFF;
    uint32_t sigI = res & 0x7FFFFF;
    float sig = 0.0f;
    if (e != 0 || sigI != 0) {
        sig = (float)sigI / (8388608.0f * 2.0f) + 0.5f;
        e -= 126;
    }
    if (res & (1u << 31)) sig = -sig;
    return ldexpf(sig, e);
}

static float beF16(const uint8_t* p, float scale) {
    int16_t v = (int16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (float)v / scale;
}

// payload = mcconf reply WITHOUT the leading command-id byte.
static void parseMcconfBase(const uint8_t* payload, int len) {
    gVescBase = {};
    if (len < 456) return;
    if (beU32(payload) != MCCONF_SIG_6_05) return;   // unknown layout: refuse, don't guess
    gVescBase.cutStartV      = beF16(payload + 54, 10.0f);
    gVescBase.cutEndV        = beF16(payload + 56, 10.0f);
    gVescBase.motorAmpMax    = beF32Auto(payload + 8);
    gVescBase.battAmpMax     = beF32Auto(payload + 16);
    gVescBase.battAmpRegen   = beF32Auto(payload + 20);
    gVescBase.motorPoles     = payload[441];
    gVescBase.gearRatio      = beF32Auto(payload + 442);
    gVescBase.wheelDiameterM = beF32Auto(payload + 446);
    gVescBase.cells          = payload[451];
    gVescBase.packAh         = beF32Auto(payload + 452);

    // Sanity gate (handoff step 4): reject implausible values wholesale — a
    // partially-plausible parse is worse than no parse.
    bool sane = gVescBase.cells >= 3 && gVescBase.cells <= 24 &&
                gVescBase.motorPoles >= 2 && gVescBase.motorPoles <= 60 &&
                gVescBase.wheelDiameterM > 0.05f && gVescBase.wheelDiameterM < 1.5f &&
                gVescBase.gearRatio > 0.5f && gVescBase.gearRatio < 30.0f &&
                gVescBase.packAh > 1.0f && gVescBase.packAh < 200.0f &&
                gVescBase.cutEndV > 2.0f * gVescBase.cells &&
                gVescBase.cutStartV >= gVescBase.cutEndV;
    gVescBase.valid = sane;
}

static void saveMcconfFile() {
    File f = LittleFS.open("/mcconf.hex", "w");
    if (!f) return;
    f.printf("COMM_GET_MCCONF raw capture (no parsing)\n");
    f.printf("esc fw %u.%u %s\n", gFw.major, gFw.minor, gFw.hwName[0] ? gFw.hwName : "?");
    if (gMcconfLen > 0) {
        f.printf("len %d\n", (int)gMcconfLen);
        for (int i = 0; i < gMcconfLen; i++)
            f.printf((i % 16 == 15 || i == gMcconfLen - 1) ? "%02X\n" : "%02X ", gMcconfRaw[i]);
    } else {
        f.printf("FAILED: no valid reply in %u attempts\n", (unsigned)gMcconfAttempts);
    }
    f.close();
}

// One-shot VESC terminal passthrough (see header). The buffer is handed to the
// consumer by pointer after state flips to READY; single-flight keeps it safe.
static char gTermCmd[32];
static char gTermText[1024];
static volatile uint8_t gTermState = 0;   // 0 idle, 1 queued, 2 ready

// ESC-side ride stats (COMM_GET_STATS). Slow-changing, so read every ~20th
// poll cycle (~2 s); a few consecutive failures means the firmware predates
// the command — stop asking (retries again after a link re-probe).
static VescStats gStats;
static bool    gStatsHave = false;
static uint8_t gStatsFails = 0;

static void pollStats() {
    static uint32_t cycle = 0;
    if (gStatsFails >= 3 || ++cycle < 20) return;
    cycle = 0;
    VescStats s;
    if (gProto.getStats(s)) {
        gStats = s;
        gStatsHave = true;
        gStatsFails = 0;
    } else {
        gStatsFails++;
    }
}

static void tryMcconfCapture() {
    gMcconfAttempts++;
    int n = gProto.rawCommand(VESC_COMM_GET_MCCONF, gMcconfRaw, sizeof(gMcconfRaw));
    if (n > 0) {
        gMcconfLen = n;
        gMcconfStatus = 1;
        saveMcconfFile();
        parseMcconfBase(gMcconfRaw + 1, n - 1);   // strip the command-id echo
    } else if (gMcconfAttempts >= MCCONF_MAX_ATTEMPTS) {
        gMcconfStatus = 2;
        saveMcconfFile();   // record the failure so a later USB read explains itself
    }
}

static void publish(const RawVescData& d) {
    if (xSemaphoreTake(gDataMutex, portMAX_DELAY) == pdTRUE) {
        gRawData = d;
        gHasNewData = true;
        gPublishes++;
        gLastVin = d.inpVoltage;
        xSemaphoreGive(gDataMutex);
    }
}

static void fillCommon(RawVescData& d, const VescPpm& ppm, bool ppmOk) {
    d.ppmDecoded = ppm.decoded;
    d.ppmPulseMs = ppm.pulseMs;
    // Over PPM "connected" means "a valid pulse is present" — it CANNOT
    // distinguish the handheld being OFF from ON-but-idle, because the
    // receiver failsafes to the same center pulse (~1.49 ms) in both cases
    // (measured). Real remote link/battery would need the VX1's UART
    // telemetry on a COMM port.
    d.ppmConnected = ppmOk && ppm.pulseMs > 0.5f && ppm.pulseMs < 2.5f;
    d.fwMajor = gFw.major;
    d.fwMinor = gFw.minor;
    strncpy(d.hwName, gFw.hwName, sizeof(d.hwName));
    d.hwName[sizeof(d.hwName) - 1] = 0;
    d.slaveCanId = gSlaveId;
}

// Modern cycle: one aggregated packet + one small forwarded slave read.
// Returns false on a failed master read (caller counts misses).
static bool pollModern() {
    VescSetupValues sv;
    if (!gProto.getSetupValuesSelective(SETUP_MASK, sv)) return false;
    if (sv.controllerId != 0) gLocalId = sv.controllerId;

    VescMotorValues slave;
    bool slaveOk = (gSlaveId != 0) && gProto.getValuesSelective(SLAVE_MASK, slave, gSlaveId);
    scanForSlave();

    VescPpm ppm;
    bool ppmOk = gProto.getDecodedPpm(ppm);

    RawVescData d = {};
    d.modernProto = true;
    d.rpm = sv.rpm;
    d.inpVoltage = sv.vIn;
    d.dutyCycleNow = sv.duty;

    // The ESC aggregates over CAN only when the slave broadcasts CAN status
    // messages; numVescs tells us whether that actually happened. If not
    // (status frames off, or a fresh link), fall back to summing the slave's
    // forwarded read ourselves so dual-drive power/energy stay honest.
    bool escAggregated = sv.numVescs >= 2;
    d.avgInputCurrent = sv.currentInTot;
    d.avgMotorCurrent = sv.currentTot;
    d.wattHours = sv.whTot;
    d.wattHoursCharged = sv.whChgTot;
    if (escAggregated) {
        d.masterMotorAmps = slaveOk ? (sv.currentTot - slave.currentMotor) : sv.currentTot;
    } else {
        d.masterMotorAmps = sv.currentTot;
        if (slaveOk) {
            d.avgInputCurrent += slave.currentIn;
            d.avgMotorCurrent += slave.currentMotor;
            d.wattHours += slave.wattHours;
            d.wattHoursCharged += slave.wattHoursCharged;
        }
    }

    // Temps: keep the hotter of the two so a warning trips on either motor/ESC.
    d.masterTempMotor = sv.tempMotor;
    d.masterTempMosfet = sv.tempFet;
    d.tempMotor = sv.tempMotor;
    d.tempMosfet = sv.tempFet;
    if (slaveOk) {
        d.slaveMotorAmps = slave.currentMotor;
        d.slaveTempMotor = slave.tempMotor;
        d.slaveTempMosfet = slave.tempFet;
        if (slave.tempMotor > d.tempMotor) d.tempMotor = slave.tempMotor;
        if (slave.tempFet > d.tempMosfet) d.tempMosfet = slave.tempFet;
    }
    d.slaveOnline = slaveOk;

    // Surface a slave fault even when the master itself is clean.
    d.error = sv.fault != 0 ? sv.fault : (slaveOk ? slave.fault : 0);

    d.vescSpeedKmh = sv.speedMs * 3.6f;
    d.vescBatteryPct = (int)lroundf(sv.battLevel * 100.0f);
    d.vescWhLeft = sv.whLeft;
    d.vescOdometerKm = sv.odometerM / 1000.0f;
    d.numVescs = sv.numVescs;

    fillCommon(d, ppm, ppmOk);
    publish(d);
    return true;
}

// Legacy cycle for pre-selective firmware: full COMM_GET_VALUES from the
// master, forwarded full read of the slave, manual summing — the exact
// field-proven behavior this transport always had.
static bool pollLegacy() {
    VescMotorValues m;
    if (!gProto.getValues(m)) return false;
    if (m.controllerId != 0) gLocalId = m.controllerId;

    VescMotorValues slave;
    bool slaveOk = (gSlaveId != 0) && gProto.getValues(slave, gSlaveId);
    scanForSlave();

    VescPpm ppm;
    bool ppmOk = gProto.getDecodedPpm(ppm);

    RawVescData d = {};
    d.modernProto = false;
    d.rpm = m.rpm;
    d.inpVoltage = m.vIn;
    d.dutyCycleNow = m.duty;
    d.avgInputCurrent = m.currentIn;
    d.avgMotorCurrent = m.currentMotor;
    d.wattHours = m.wattHours;
    d.wattHoursCharged = m.wattHoursCharged;
    d.tempMotor = m.tempMotor;
    d.tempMosfet = m.tempFet;
    d.error = m.fault;
    d.masterMotorAmps = m.currentMotor;
    d.masterTempMotor = m.tempMotor;
    d.masterTempMosfet = m.tempFet;

    if (slaveOk) {
        d.slaveOnline = true;
        d.slaveMotorAmps = slave.currentMotor;
        d.slaveTempMotor = slave.tempMotor;
        d.slaveTempMosfet = slave.tempFet;
        d.avgInputCurrent += slave.currentIn;
        d.avgMotorCurrent += slave.currentMotor;
        d.wattHours += slave.wattHours;
        d.wattHoursCharged += slave.wattHoursCharged;
        if (slave.tempMotor > d.tempMotor) d.tempMotor = slave.tempMotor;
        if (slave.tempFet > d.tempMosfet) d.tempMosfet = slave.tempFet;
        if (d.error == 0) d.error = slave.fault;
    }
    d.numVescs = slaveOk ? 2 : 1;

    fillCommon(d, ppm, ppmOk);
    publish(d);
    return true;
}

// Decide MODERN vs LEGACY for whatever ESC just appeared on the wire. Runs
// once per link-up (and again after a sustained loss, so an ESC swap or
// firmware update mid-session is picked up).
static void probePath() {
    VescSetupValues sv;
    if (gProto.getSetupValuesSelective(SETUP_MASK, sv)) {
        gPath = PATH_MODERN;
        gStatsFails = 0;   // fresh link: retry GET_STATS (ESC may have been swapped)
    } else {
        VescMotorValues m;
        if (gProto.getValues(m)) gPath = PATH_LEGACY;
    }
    if (gPath != PATH_UNKNOWN && gFw.major == 0) {
        gProto.getFwVersion(gFw);   // best effort; retried while major == 0
    }
}

// This task is the SOLE owner of the VESC UART, and (with EVEE Link) it is also
// the throttle's control loop. Both live here on purpose: two writers on one
// UART interleave bytes and corrupt each other's frames, and the throttle must
// not share a core with the renderer (core 1) where a redraw could stretch a
// control tick.
//
// Cadence without EVEE Link: 100 ms, exactly as before.
// Cadence with it:  a 20 ms tick. The throttle write goes out FIRST on every
//                   tick (fire-and-forget, well under 1 ms), and the telemetry
//                   poll runs on every 5th tick — so telemetry keeps its old
//                   10 Hz and the throttle gets its 50 Hz.
//
// While ARMED the telemetry poll is skipped entirely. A poll is several blocking
// transacts, and if the ESC goes quiet they each burn their timeout; a poll cycle
// that overran EVEE_VESC_TIMEOUT_MS would trip the VESC's own UART timeout and
// coast the rider mid-ride for no reason. Telemetry is worth less than that.
static void vescPollTask(void* pvParameters) {
    int consecutiveMisses = 0;
#if EVEE_LINK_ENABLED
    uint8_t tickN = 0;
#endif

    for (;;) {
#if EVEE_LINK_ENABLED
        // Throttle first, always. It is the only thing here with a deadline.
        const bool linkArmed = RemoteLink::tick(gProto);

        const bool pollThisTick = !linkArmed && (++tickN % 5 == 0);
#else
        const bool linkArmed = false;
        const bool pollThisTick = true;
#endif

        if (!gPollPaused && !linkArmed && pollThisTick) {
            if (gPath == PATH_UNKNOWN) {
                probePath();
                if (gPath != PATH_UNKNOWN) consecutiveMisses = 0;
            } else {
                bool ok = (gPath == PATH_MODERN) ? pollModern() : pollLegacy();
                if (ok) {
                    consecutiveMisses = 0;
                    if (gFw.major == 0) gProto.getFwVersion(gFw);
                    if (gMcconfStatus == 0) tryMcconfCapture();
                    pollStats();
                } else if (++consecutiveMisses >= 20) {
                    // ~2s of silence: treat as a link loss and re-probe from
                    // scratch when the ESC comes back. The slave ID is kept —
                    // a power cycle doesn't change CAN IDs.
                    gPath = PATH_UNKNOWN;
                    consecutiveMisses = 0;
                }
            }
            // Terminal passthrough: only while this task owns the UART (never
            // during bridge mode, when VESC Tool owns the wire).
            if (gTermState == 1) {
                gTermText[0] = 0;
                gProto.terminalCmd(gTermCmd, gTermText, sizeof(gTermText));
                gTermState = 2;
            }
        }

#if EVEE_LINK_ENABLED
        vTaskDelay(pdMS_TO_TICKS(EVEE_CONTROL_MS));
#else
        vTaskDelay(pdMS_TO_TICKS(100));
#endif
    }
}
#endif

void beginVescUart(uint8_t slaveIdHint) {
    gDataMutex = xSemaphoreCreateMutex();
#ifndef WOKWI_SIMULATION
    gSlaveHint = slaveIdHint;
    Serial1.begin(115200, SERIAL_8N1, 18, 17); // GPIO 18 RX, 17 TX
    gProto.begin(&Serial1);

#if EVEE_LINK_ENABLED
    RemoteLink::begin();
#endif

    xTaskCreatePinnedToCore(
        vescPollTask,
        "VESC_Poll",
        4096,
        NULL,
#if EVEE_LINK_ENABLED
        // With EVEE Link this task carries the throttle, so it must not be
        // preempted by the WiFi/BLE housekeeping that also lives on core 0.
        configMAX_PRIORITIES - 2,
#else
        1,
#endif
        &gVescTaskHandle,
        0  // Pin to Core 0 (UI runs on Core 1)
    );
#endif
}

bool getLatestVescData(RawVescData* outData) {
    if (gDataMutex == NULL) return false;
    bool gotNew = false;
    if (xSemaphoreTake(gDataMutex, 0) == pdTRUE) {
        if (gHasNewData) {
            *outData = gRawData;
            gHasNewData = false; // consume it
            gotNew = true;
        }
        xSemaphoreGive(gDataMutex);
    }
    return gotNew;
}

void setVescPollPaused(bool paused) {
    gPollPaused = paused;
}

bool peekVescData(RawVescData* outData) {
#ifndef WOKWI_SIMULATION
    if (gDataMutex == NULL) return false;
    bool any = false;
    if (xSemaphoreTake(gDataMutex, 0) == pdTRUE) {
        any = gPublishes > 0;
        *outData = gRawData;
        xSemaphoreGive(gDataMutex);
    }
    return any;
#else
    (void)outData;
    return false;   // no real transport in the Wokwi build
#endif
}

bool getVescBaseConfig(VescBaseConfig* out) {
    *out = {};
#ifndef WOKWI_SIMULATION
    if (!gVescBase.valid) return false;
    *out = gVescBase;
    return true;
#else
    return false;
#endif
}

bool requestVescTerminal(const char* cmd) {
#ifndef WOKWI_SIMULATION
    if (gTermState == 1 || cmd == nullptr || !cmd[0]) return false;
    strlcpy(gTermCmd, cmd, sizeof(gTermCmd));
    gTermState = 1;
    return true;
#else
    (void)cmd;
    return false;
#endif
}

bool fetchVescTerminal(const char** text) {
#ifndef WOKWI_SIMULATION
    if (gTermState != 2) return false;
    *text = gTermText;
    gTermState = 0;
    return true;
#else
    (void)text;
    return false;
#endif
}

bool getVescRideStats(VescRideStats* out) {
    *out = {};
#ifndef WOKWI_SIMULATION
    if (!gStatsHave) return false;
    out->valid = true;
    out->speedAvgKmh   = gStats.speedAvg * 3.6f;
    out->speedMaxKmh   = gStats.speedMax * 3.6f;
    out->powerAvgW     = gStats.powerAvg;
    out->powerMaxW     = gStats.powerMax;
    out->currentAvgA   = gStats.currentAvg;
    out->currentMaxA   = gStats.currentMax;
    out->tempMosAvgC   = gStats.tempMosAvg;
    out->tempMosMaxC   = gStats.tempMosMax;
    out->tempMotorAvgC = gStats.tempMotorAvg;
    out->tempMotorMaxC = gStats.tempMotorMax;
    out->rideTimeS     = gStats.countTime;
    return true;
#else
    return false;
#endif
}

void getMcconfCaptureState(McconfCapture* out) {
    *out = {};
#ifndef WOKWI_SIMULATION
    out->status = gMcconfStatus;
    out->len = gMcconfLen;
    out->attempts = gMcconfAttempts;
#endif
}

void getVescLinkDebug(VescLinkDebug* out) {
    *out = {};
#ifndef WOKWI_SIMULATION
    out->path = (gPath == PATH_MODERN) ? 1 : (gPath == PATH_LEGACY) ? 2 : 0;
    out->slaveId = gSlaveId;
    out->searchDone = gSlaveSearchDone;
    out->scanIdx = gScanIdx;
    out->publishes = gPublishes;
    out->lastVin = gLastVin;
    out->txFrames = gProto.dbgTxFrames;
    out->replies = gProto.dbgReplies;
    out->rxBytes = gProto.dbgRxBytes;
    out->crcErrors = gProto.dbgCrcErrors;
    out->timeouts = gProto.dbgTimeouts;
#endif
}

}
}
