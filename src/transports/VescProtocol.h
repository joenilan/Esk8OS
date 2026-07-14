#pragma once
#include <Arduino.h>

// ============================================================================
// Self-contained VESC UART protocol client. Replaces the abandoned
// SolidGeek/VescUart library (last commit Nov 2023) with an in-repo
// implementation verified against vedderb/bldc master (FW 6.x), so we can use
// the modern telemetry commands the old lib never learned:
//
//   COMM_GET_VALUES_SETUP_SELECTIVE  - CAN-aggregated dual-motor telemetry,
//                                      speed (m/s), battery level, persistent
//                                      odometer, Wh remaining, #VESCs — one
//                                      packet instead of a master+slave dance.
//   COMM_GET_VALUES_SELECTIVE        - per-motor reads with a field bitmask
//                                      (small packets for the diag view).
//   COMM_GET_STATS                   - ESC-side ride statistics (FW 6+).
//   COMM_SET_ODOMETER                - set the ESC's persistent odometer.
//
// Wire format (both directions):
//   [0x02 len | 0x03 lenHi lenLo] payload crc16Hi crc16Lo 0x03
// crc16 = CRC-16/XMODEM over the payload only. All multi-byte fields are
// big-endian. Field layouts and mask bit positions were transcribed from
// bldc/comm/commands.c — do not "fix" an offset without re-checking there.
// Older firmware (FW 3.x/4.x) lacks the selective commands; callers fall back
// to the fixed-layout COMM_GET_VALUES which is stable back to FW 3.4.
// ============================================================================

namespace Esk8OS {
namespace Transports {

// COMM_PACKET_ID subset (values from bldc/datatypes.h).
enum VescCommand : uint8_t {
    VESC_COMM_FW_VERSION                = 0,
    VESC_COMM_GET_VALUES                = 4,
    VESC_COMM_SET_CURRENT               = 6,
    VESC_COMM_SET_CURRENT_BRAKE         = 7,
    VESC_COMM_GET_MCCONF                = 14,
    VESC_COMM_TERMINAL_CMD              = 20,
    VESC_COMM_PRINT                     = 21,   // was wrongly 26 (=DETECT_FLUX); confirmed vs bldc datatypes.h
    VESC_COMM_ALIVE                     = 30,
    VESC_COMM_GET_DECODED_PPM           = 31,
    VESC_COMM_FORWARD_CAN               = 34,
    VESC_COMM_GET_VALUES_SETUP          = 47,
    VESC_COMM_GET_VALUES_SELECTIVE      = 50,
    VESC_COMM_GET_VALUES_SETUP_SELECTIVE= 51,
    VESC_COMM_PING_CAN                  = 62,
    VESC_COMM_SET_ODOMETER              = 110,
    VESC_COMM_GET_STATS                 = 128,
};

// COMM_GET_VALUES_SELECTIVE mask bits (commands.c, COMM_GET_VALUES case).
enum : uint32_t {
    VESC_VAL_TEMP_FET       = 1u << 0,
    VESC_VAL_TEMP_MOTOR     = 1u << 1,
    VESC_VAL_CURRENT_MOTOR  = 1u << 2,
    VESC_VAL_CURRENT_IN     = 1u << 3,
    VESC_VAL_DUTY           = 1u << 6,
    VESC_VAL_RPM            = 1u << 7,
    VESC_VAL_V_IN           = 1u << 8,
    VESC_VAL_AH             = 1u << 9,
    VESC_VAL_AH_CHARGED     = 1u << 10,
    VESC_VAL_WH             = 1u << 11,
    VESC_VAL_WH_CHARGED     = 1u << 12,
    VESC_VAL_TACHO          = 1u << 13,
    VESC_VAL_TACHO_ABS      = 1u << 14,
    VESC_VAL_FAULT          = 1u << 15,
    VESC_VAL_PID_POS        = 1u << 16,
    VESC_VAL_CONTROLLER_ID  = 1u << 17,
};

// COMM_GET_VALUES_SETUP_SELECTIVE mask bits (commands.c, GET_VALUES_SETUP case).
enum : uint32_t {
    VESC_SETUP_TEMP_FET       = 1u << 0,   // master-local FET temp
    VESC_SETUP_TEMP_MOTOR     = 1u << 1,   // master-local motor temp
    VESC_SETUP_CURRENT_TOT    = 1u << 2,   // motor current, summed over CAN
    VESC_SETUP_CURRENT_IN_TOT = 1u << 3,   // battery current, summed over CAN
    VESC_SETUP_DUTY           = 1u << 4,
    VESC_SETUP_RPM            = 1u << 5,
    VESC_SETUP_SPEED          = 1u << 6,   // m/s, from the ESC's gearing setup
    VESC_SETUP_V_IN           = 1u << 7,
    VESC_SETUP_BATT_LEVEL     = 1u << 8,   // 0..1
    VESC_SETUP_AH_TOT         = 1u << 9,
    VESC_SETUP_AH_CHG_TOT     = 1u << 10,
    VESC_SETUP_WH_TOT         = 1u << 11,
    VESC_SETUP_WH_CHG_TOT     = 1u << 12,
    VESC_SETUP_DIST           = 1u << 13,  // meters
    VESC_SETUP_DIST_ABS       = 1u << 14,  // meters
    VESC_SETUP_FAULT          = 1u << 16,
    VESC_SETUP_CONTROLLER_ID  = 1u << 17,
    VESC_SETUP_NUM_VESCS      = 1u << 18,  // controllers seen on the CAN bus
    VESC_SETUP_WH_LEFT        = 1u << 19,  // Wh left per the ESC battery config
    VESC_SETUP_ODOMETER       = 1u << 20,  // persistent, meters
    VESC_SETUP_UPTIME         = 1u << 21,  // ms since ESC boot
};

// Per-motor values (COMM_GET_VALUES / COMM_GET_VALUES_SELECTIVE). Fields not
// requested (or absent on old firmware) stay zeroed.
struct VescMotorValues {
    float tempFet = 0, tempMotor = 0;
    float currentMotor = 0, currentIn = 0;
    float duty = 0, rpm = 0, vIn = 0;
    float ampHours = 0, ampHoursCharged = 0;
    float wattHours = 0, wattHoursCharged = 0;
    int32_t tachometer = 0, tachometerAbs = 0;
    uint8_t fault = 0, controllerId = 0;
};

// CAN-aggregated values (COMM_GET_VALUES_SETUP_SELECTIVE).
struct VescSetupValues {
    float tempFet = 0, tempMotor = 0;         // master-local
    float currentTot = 0, currentInTot = 0;   // summed across CAN
    float duty = 0, rpm = 0;
    float speedMs = 0;
    float vIn = 0;
    float battLevel = 0;                      // 0..1
    float ahTot = 0, ahChgTot = 0;
    float whTot = 0, whChgTot = 0;
    float distM = 0, distAbsM = 0;
    uint8_t fault = 0, controllerId = 0, numVescs = 0;
    float whLeft = 0;
    uint32_t odometerM = 0, uptimeMs = 0;
};

struct VescFwInfo {
    uint8_t major = 0, minor = 0;
    char hwName[17] = {0};
};

struct VescPpm {
    float decoded = 0;   // -1..1 throttle
    float pulseMs = 0;   // last servo pulse length
};

// ESC-side ride statistics (COMM_GET_STATS, FW 6+). Speeds m/s, temps C.
struct VescStats {
    float speedAvg = 0, speedMax = 0;
    float powerAvg = 0, powerMax = 0;
    float currentAvg = 0, currentMax = 0;
    float tempMosAvg = 0, tempMosMax = 0;
    float tempMotorAvg = 0, tempMotorMax = 0;
    float countTime = 0;   // seconds accumulated
};

class VescProtocol {
public:
    void begin(Stream* port) { _port = port; }

    // Wire-level counters for link diagnosis (`diag` console command): they
    // distinguish "ESC silent" (txFrames grows, rxBytes stays 0) from "ESC
    // talking but frames rejected" (rxBytes grows, crcErrors/timeouts grow).
    uint32_t dbgTxFrames = 0;   // requests sent
    uint32_t dbgReplies  = 0;   // well-formed replies accepted
    uint32_t dbgRxBytes  = 0;   // raw bytes seen on the wire
    uint32_t dbgCrcErrors = 0;  // frames dropped on CRC mismatch
    uint32_t dbgTimeouts = 0;   // requests that got no (complete) reply

    // All getters return false on timeout / CRC error / short reply.
    // canId != 0 forwards the request over CAN via the master (COMM_FORWARD_CAN).
    bool getFwVersion(VescFwInfo& out, uint8_t canId = 0);
    bool getValues(VescMotorValues& out, uint8_t canId = 0);                   // fixed-layout legacy
    bool getValuesSelective(uint32_t mask, VescMotorValues& out, uint8_t canId = 0);
    bool getSetupValuesSelective(uint32_t mask, VescSetupValues& out);
    bool getDecodedPpm(VescPpm& out);
    bool getStats(VescStats& out);

    // Send a bare one-byte command and hand back the raw reply payload
    // (reply[0] echoes cmd). No decoding — for evidence-first work on replies
    // whose layout is firmware-version-specific (COMM_GET_MCCONF is several
    // hundred bytes on the 16-bit-length framing, hence the longer timeout).
    int rawCommand(uint8_t cmd, uint8_t* reply, int maxReply, uint32_t timeoutMs = 500);

    // Send a VESC terminal command (COMM_TERMINAL_CMD) and collect the text
    // from its COMM_PRINT replies until the line goes quiet. Returns the text
    // length written to out (NUL-terminated; 0 = no reply). `faults`, `ping`,
    // `hw_status` etc — the same commands VESC Tool's Terminal tab speaks.
    int terminalCmd(const char* cmd, char* out, int maxOut, uint32_t totalMs = 1200);
    void setOdometerMeters(uint32_t meters);                                   // no ack from ESC

    // ---- motor commands (EVEE Link receiver mode) --------------------------
    // Fire-and-forget: the ESC acks none of these, and a throttle write must
    // never block behind a reply. ~10 bytes each, so a write costs under a
    // millisecond at 115200 and fits inside a 20 ms control tick with room to
    // spare for the telemetry poll that shares this UART.
    //
    // The VESC applies its own timeout to these (App Settings -> General ->
    // Timeout): if the commands stop arriving it zeroes the motor by itself.
    // That is the failsafe that still works when OUR firmware hangs, and it is
    // the reason the throttle goes out as a set-command rather than as a value
    // latched somewhere the ESC cannot supervise.
    void setCurrent(float amps);
    void setCurrentBrake(float amps);

    // Probe one CAN ID with a tiny forwarded request; true if it answered.
    // (COMM_PING_CAN exists but blocks the ESC for seconds scanning all 255
    // IDs, which would starve the poll loop — incremental probing instead.)
    bool probeCanId(uint8_t canId, uint32_t timeoutMs = 50);

private:
    Stream* _port = nullptr;

    // Send one framed payload and read one framed reply whose first payload
    // byte equals expectCmd. Returns payload length (>=1) or 0 on failure.
    int transact(const uint8_t* payload, int len, uint8_t expectCmd,
                 uint8_t* reply, int maxReply, uint32_t timeoutMs = 100);

    // Frame and send, expecting no reply. For commands the ESC does not ack.
    void sendNoReply(const uint8_t* payload, int len);
    bool parseValues(const uint8_t* p, int len, uint32_t mask, bool maskEchoed, VescMotorValues& out);
};

}
}
