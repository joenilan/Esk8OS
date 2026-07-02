#include "transports/VescUartTransport.h"
#include <Arduino.h>

#ifndef WOKWI_SIMULATION
#include "transports/VescProtocol.h"
#endif

namespace Esk8OS {
namespace Transports {

static RawVescData gRawData;
static bool gHasNewData = false;
static SemaphoreHandle_t gDataMutex = NULL;
static bool gPollPaused = false;
static TaskHandle_t gVescTaskHandle = NULL;

// First CAN ID tried when hunting for the second motor's VESC. The slave is
// auto-detected by probing IDs over CAN (one per poll cycle), so this is only
// an ordering hint that makes a known board lock on instantly instead of after
// a sweep. 114 = Joe's slave Controller ID (from VESC Tool's CAN Devices list).
static const uint8_t VESC_SLAVE_CAN_ID_HINT = 114;

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
    if (idx == 0) return VESC_SLAVE_CAN_ID_HINT;
    // Then 1..253 ascending, skipping the hint (already tried). The master's
    // own ID is skipped by the caller.
    int id = idx;   // idx >= 1
    if (id >= VESC_SLAVE_CAN_ID_HINT) id++;
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

static void publish(const RawVescData& d) {
    if (xSemaphoreTake(gDataMutex, portMAX_DELAY) == pdTRUE) {
        gRawData = d;
        gHasNewData = true;
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
    } else {
        VescMotorValues m;
        if (gProto.getValues(m)) gPath = PATH_LEGACY;
    }
    if (gPath != PATH_UNKNOWN && gFw.major == 0) {
        gProto.getFwVersion(gFw);   // best effort; retried while major == 0
    }
}

static void vescPollTask(void* pvParameters) {
    int consecutiveMisses = 0;
    for (;;) {
        if (!gPollPaused) {
            if (gPath == PATH_UNKNOWN) {
                probePath();
                if (gPath != PATH_UNKNOWN) consecutiveMisses = 0;
            } else {
                bool ok = (gPath == PATH_MODERN) ? pollModern() : pollLegacy();
                if (ok) {
                    consecutiveMisses = 0;
                    if (gFw.major == 0) gProto.getFwVersion(gFw);
                } else if (++consecutiveMisses >= 20) {
                    // ~2s of silence: treat as a link loss and re-probe from
                    // scratch when the ESC comes back. The slave ID is kept —
                    // a power cycle doesn't change CAN IDs.
                    gPath = PATH_UNKNOWN;
                    consecutiveMisses = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

void beginVescUart() {
    gDataMutex = xSemaphoreCreateMutex();
#ifndef WOKWI_SIMULATION
    Serial1.begin(115200, SERIAL_8N1, 18, 17); // GPIO 18 RX, 17 TX
    gProto.begin(&Serial1);

    xTaskCreatePinnedToCore(
        vescPollTask,
        "VESC_Poll",
        4096,
        NULL,
        1,
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

}
}
