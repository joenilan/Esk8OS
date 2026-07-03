#include "transports/VescProtocol.h"
#include <math.h>

namespace Esk8OS {
namespace Transports {

// ---- primitives -------------------------------------------------------------
// CRC-16/XMODEM (poly 0x1021, init 0), bit-serial — provably identical to the
// table in bldc/crc.c without 256 transcribed constants. Payloads are tiny.
static uint16_t crc16(const uint8_t* buf, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// Big-endian readers matching bldc/buffer.c. Every read is bounds-guarded by
// the callers via remaining-length checks before decoding each field.
static int16_t  bGetI16(const uint8_t* b, int32_t& i) { int16_t v = ((int16_t)b[i] << 8) | b[i+1]; i += 2; return v; }
static uint16_t bGetU16(const uint8_t* b, int32_t& i) { uint16_t v = ((uint16_t)b[i] << 8) | b[i+1]; i += 2; return v; }
static int32_t  bGetI32(const uint8_t* b, int32_t& i) {
    int32_t v = ((uint32_t)b[i] << 24) | ((uint32_t)b[i+1] << 16) | ((uint32_t)b[i+2] << 8) | b[i+3];
    i += 4; return v;
}
static uint32_t bGetU32(const uint8_t* b, int32_t& i) { return (uint32_t)bGetI32(b, i); }
static float bGetF16(const uint8_t* b, int32_t& i, float scale) { return (float)bGetI16(b, i) / scale; }
static float bGetF32(const uint8_t* b, int32_t& i, float scale) { return (float)bGetI32(b, i) / scale; }

// bldc's portable float encoding (buffer_get_float32_auto) used by GET_STATS:
// sign(1) | exponent(8) | mantissa(23), decoded via ldexp — NOT a raw IEEE754
// memcpy, the exponent bias handling differs.
static float bGetF32Auto(const uint8_t* b, int32_t& i) {
    uint32_t res = bGetU32(b, i);
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

// ---- framing ----------------------------------------------------------------
int VescProtocol::transact(const uint8_t* payload, int len, uint8_t expectCmd,
                           uint8_t* reply, int maxReply, uint32_t timeoutMs) {
    if (_port == nullptr || len <= 0 || len > 250) return 0;

    while (_port->available()) _port->read();   // drop stale bytes

    uint8_t frame[256 + 5];
    int n = 0;
    frame[n++] = 0x02;
    frame[n++] = (uint8_t)len;
    memcpy(frame + n, payload, len); n += len;
    uint16_t crc = crc16(payload, len);
    frame[n++] = (uint8_t)(crc >> 8);
    frame[n++] = (uint8_t)(crc & 0xFF);
    frame[n++] = 0x03;
    _port->write(frame, n);
    _port->flush();
    dbgTxFrames++;

    const uint32_t start = millis();
    auto readByte = [&](int& out) -> bool {
        while (!_port->available()) {
            if (millis() - start > timeoutMs) { dbgTimeouts++; return false; }
            delay(1);   // yield so a wait can't starve lower-priority tasks
        }
        out = _port->read();
        dbgRxBytes++;
        return true;
    };

    // Sync to a start byte, allowing one resync pass over line noise. Replies
    // use 0x02 (payload <= 255) or 0x03 + 16-bit length (larger, e.g. configs).
    int b;
    for (;;) {
        do { if (!readByte(b)) return 0; } while (b != 0x02 && b != 0x03);
        int plen;
        if (b == 0x02) {
            if (!readByte(plen)) return 0;
        } else {
            int hi, lo;
            if (!readByte(hi) || !readByte(lo)) return 0;
            plen = (hi << 8) | lo;
        }
        if (plen <= 0 || plen > maxReply) return 0;
        for (int i = 0; i < plen; i++) { if (!readByte(b)) return 0; reply[i] = (uint8_t)b; }
        int crcHi, crcLo, stop;
        if (!readByte(crcHi) || !readByte(crcLo) || !readByte(stop)) return 0;
        if (stop != 0x03) continue;                                   // mis-sync: hunt again
        if (((crcHi << 8) | crcLo) != crc16(reply, plen)) { dbgCrcErrors++; return 0; }   // corrupt: caller retries
        if (reply[0] != expectCmd) continue;                          // unrelated packet (e.g. COMM_PRINT)
        dbgReplies++;
        return plen;
    }
}

// ---- decoders ---------------------------------------------------------------
// Shared decoder for GET_VALUES (fixed order = mask 0xFFFFFFFF, maskEchoed
// false) and GET_VALUES_SELECTIVE (fields present per echoed mask). Field
// order and scales transcribed from commands.c. Stops cleanly if the payload
// ends early (older firmware sends fewer trailing fields).
bool VescProtocol::parseValues(const uint8_t* p, int len, uint32_t mask, bool maskEchoed, VescMotorValues& out) {
    int32_t i = 1;   // skip command id
    if (maskEchoed) {
        if (len < 5) return false;
        mask = bGetU32(p, i);
    }
    auto has = [&](int bytes) { return i + bytes <= len; };

    if (mask & VESC_VAL_TEMP_FET)      { if (!has(2)) return true; out.tempFet      = bGetF16(p, i, 10.0f); }
    if (mask & VESC_VAL_TEMP_MOTOR)    { if (!has(2)) return true; out.tempMotor    = bGetF16(p, i, 10.0f); }
    if (mask & VESC_VAL_CURRENT_MOTOR) { if (!has(4)) return true; out.currentMotor = bGetF32(p, i, 100.0f); }
    if (mask & VESC_VAL_CURRENT_IN)    { if (!has(4)) return true; out.currentIn    = bGetF32(p, i, 100.0f); }
    if (mask & (1u << 4))              { if (!has(4)) return true; i += 4; }   // avg id
    if (mask & (1u << 5))              { if (!has(4)) return true; i += 4; }   // avg iq
    if (mask & VESC_VAL_DUTY)          { if (!has(2)) return true; out.duty = bGetF16(p, i, 1000.0f); }
    if (mask & VESC_VAL_RPM)           { if (!has(4)) return true; out.rpm  = bGetF32(p, i, 1.0f); }
    if (mask & VESC_VAL_V_IN)          { if (!has(2)) return true; out.vIn  = bGetF16(p, i, 10.0f); }
    if (mask & VESC_VAL_AH)            { if (!has(4)) return true; out.ampHours        = bGetF32(p, i, 10000.0f); }
    if (mask & VESC_VAL_AH_CHARGED)    { if (!has(4)) return true; out.ampHoursCharged = bGetF32(p, i, 10000.0f); }
    if (mask & VESC_VAL_WH)            { if (!has(4)) return true; out.wattHours        = bGetF32(p, i, 10000.0f); }
    if (mask & VESC_VAL_WH_CHARGED)    { if (!has(4)) return true; out.wattHoursCharged = bGetF32(p, i, 10000.0f); }
    if (mask & VESC_VAL_TACHO)         { if (!has(4)) return true; out.tachometer    = bGetI32(p, i); }
    if (mask & VESC_VAL_TACHO_ABS)     { if (!has(4)) return true; out.tachometerAbs = bGetI32(p, i); }
    if (mask & VESC_VAL_FAULT)         { if (!has(1)) return true; out.fault = p[i++]; }
    if (mask & VESC_VAL_PID_POS)       { if (!has(4)) return true; i += 4; }
    if (mask & VESC_VAL_CONTROLLER_ID) { if (!has(1)) return true; out.controllerId = p[i++]; }
    return true;
}

// ---- commands ---------------------------------------------------------------
// Requests forwarded to a CAN slave get the [COMM_FORWARD_CAN, id] prefix; the
// master strips it and relays the slave's reply as a normal packet.
static int buildForwarded(uint8_t* buf, uint8_t canId, const uint8_t* inner, int innerLen) {
    int n = 0;
    if (canId != 0) {
        buf[n++] = VESC_COMM_FORWARD_CAN;
        buf[n++] = canId;
    }
    memcpy(buf + n, inner, innerLen);
    return n + innerLen;
}

bool VescProtocol::getFwVersion(VescFwInfo& out, uint8_t canId) {
    uint8_t inner[1] = { VESC_COMM_FW_VERSION };
    uint8_t payload[3];
    int len = buildForwarded(payload, canId, inner, sizeof(inner));
    uint8_t reply[80];
    int n = transact(payload, len, VESC_COMM_FW_VERSION, reply, sizeof(reply));
    if (n < 3) return false;
    out.major = reply[1];
    out.minor = reply[2];
    // Followed by the null-terminated hardware name (then UUID etc.).
    int h = 0;
    for (int i = 3; i < n && h < (int)sizeof(out.hwName) - 1 && reply[i] != 0; i++)
        out.hwName[h++] = (char)reply[i];
    out.hwName[h] = 0;
    return true;
}

bool VescProtocol::getValues(VescMotorValues& out, uint8_t canId) {
    uint8_t inner[1] = { VESC_COMM_GET_VALUES };
    uint8_t payload[3];
    int len = buildForwarded(payload, canId, inner, sizeof(inner));
    uint8_t reply[160];
    int n = transact(payload, len, VESC_COMM_GET_VALUES, reply, sizeof(reply));
    if (n < 56) return false;   // full fixed layout through controller id
    return parseValues(reply, n, 0xFFFFFFFF, false, out);
}

bool VescProtocol::getValuesSelective(uint32_t mask, VescMotorValues& out, uint8_t canId) {
    uint8_t inner[5] = { VESC_COMM_GET_VALUES_SELECTIVE,
                         (uint8_t)(mask >> 24), (uint8_t)(mask >> 16),
                         (uint8_t)(mask >> 8),  (uint8_t)mask };
    uint8_t payload[7];
    int len = buildForwarded(payload, canId, inner, sizeof(inner));
    uint8_t reply[160];
    int n = transact(payload, len, VESC_COMM_GET_VALUES_SELECTIVE, reply, sizeof(reply));
    if (n < 5) return false;
    return parseValues(reply, n, mask, true, out);
}

bool VescProtocol::getSetupValuesSelective(uint32_t mask, VescSetupValues& out) {
    uint8_t payload[5] = { VESC_COMM_GET_VALUES_SETUP_SELECTIVE,
                           (uint8_t)(mask >> 24), (uint8_t)(mask >> 16),
                           (uint8_t)(mask >> 8),  (uint8_t)mask };
    uint8_t reply[160];
    int n = transact(payload, sizeof(payload), VESC_COMM_GET_VALUES_SETUP_SELECTIVE, reply, sizeof(reply));
    if (n < 5) return false;

    int32_t i = 1;
    uint32_t m = bGetU32(reply, i);   // firmware echoes the effective mask
    auto has = [&](int bytes) { return i + bytes <= n; };

    if (m & VESC_SETUP_TEMP_FET)       { if (!has(2)) return true; out.tempFet      = bGetF16(reply, i, 10.0f); }
    if (m & VESC_SETUP_TEMP_MOTOR)     { if (!has(2)) return true; out.tempMotor    = bGetF16(reply, i, 10.0f); }
    if (m & VESC_SETUP_CURRENT_TOT)    { if (!has(4)) return true; out.currentTot   = bGetF32(reply, i, 100.0f); }
    if (m & VESC_SETUP_CURRENT_IN_TOT) { if (!has(4)) return true; out.currentInTot = bGetF32(reply, i, 100.0f); }
    if (m & VESC_SETUP_DUTY)           { if (!has(2)) return true; out.duty    = bGetF16(reply, i, 1000.0f); }
    if (m & VESC_SETUP_RPM)            { if (!has(4)) return true; out.rpm     = bGetF32(reply, i, 1.0f); }
    if (m & VESC_SETUP_SPEED)          { if (!has(4)) return true; out.speedMs = bGetF32(reply, i, 1000.0f); }
    if (m & VESC_SETUP_V_IN)           { if (!has(2)) return true; out.vIn     = bGetF16(reply, i, 10.0f); }
    if (m & VESC_SETUP_BATT_LEVEL)     { if (!has(2)) return true; out.battLevel = bGetF16(reply, i, 1000.0f); }
    if (m & VESC_SETUP_AH_TOT)         { if (!has(4)) return true; out.ahTot    = bGetF32(reply, i, 10000.0f); }
    if (m & VESC_SETUP_AH_CHG_TOT)     { if (!has(4)) return true; out.ahChgTot = bGetF32(reply, i, 10000.0f); }
    if (m & VESC_SETUP_WH_TOT)         { if (!has(4)) return true; out.whTot    = bGetF32(reply, i, 10000.0f); }
    if (m & VESC_SETUP_WH_CHG_TOT)     { if (!has(4)) return true; out.whChgTot = bGetF32(reply, i, 10000.0f); }
    if (m & VESC_SETUP_DIST)           { if (!has(4)) return true; out.distM    = bGetF32(reply, i, 1000.0f); }
    if (m & VESC_SETUP_DIST_ABS)       { if (!has(4)) return true; out.distAbsM = bGetF32(reply, i, 1000.0f); }
    if (m & (1u << 15))                { if (!has(4)) return true; i += 4; }   // pid pos
    if (m & VESC_SETUP_FAULT)          { if (!has(1)) return true; out.fault        = reply[i++]; }
    if (m & VESC_SETUP_CONTROLLER_ID)  { if (!has(1)) return true; out.controllerId = reply[i++]; }
    if (m & VESC_SETUP_NUM_VESCS)      { if (!has(1)) return true; out.numVescs     = reply[i++]; }
    if (m & VESC_SETUP_WH_LEFT)        { if (!has(4)) return true; out.whLeft    = bGetF32(reply, i, 1000.0f); }
    if (m & VESC_SETUP_ODOMETER)       { if (!has(4)) return true; out.odometerM = bGetU32(reply, i); }
    if (m & VESC_SETUP_UPTIME)         { if (!has(4)) return true; out.uptimeMs  = bGetU32(reply, i); }
    return true;
}

bool VescProtocol::getDecodedPpm(VescPpm& out) {
    uint8_t payload[1] = { VESC_COMM_GET_DECODED_PPM };
    uint8_t reply[32];
    int n = transact(payload, sizeof(payload), VESC_COMM_GET_DECODED_PPM, reply, sizeof(reply));
    if (n < 9) return false;
    int32_t i = 1;
    out.decoded = bGetI32(reply, i) / 1000000.0f;
    out.pulseMs = bGetI32(reply, i) / 1000000.0f;
    return true;
}

bool VescProtocol::getStats(VescStats& out) {
    const uint16_t mask = 0x07FF;   // all 11 stat fields
    uint8_t payload[3] = { VESC_COMM_GET_STATS, (uint8_t)(mask >> 8), (uint8_t)mask };
    uint8_t reply[64];
    int n = transact(payload, sizeof(payload), VESC_COMM_GET_STATS, reply, sizeof(reply));
    if (n < 5) return false;
    int32_t i = 1;
    uint32_t m = bGetU32(reply, i);
    auto has = [&]() { return i + 4 <= n; };
    if (m & (1u << 0))  { if (!has()) return true; out.speedAvg     = bGetF32Auto(reply, i); }
    if (m & (1u << 1))  { if (!has()) return true; out.speedMax     = bGetF32Auto(reply, i); }
    if (m & (1u << 2))  { if (!has()) return true; out.powerAvg     = bGetF32Auto(reply, i); }
    if (m & (1u << 3))  { if (!has()) return true; out.powerMax     = bGetF32Auto(reply, i); }
    if (m & (1u << 4))  { if (!has()) return true; out.currentAvg   = bGetF32Auto(reply, i); }
    if (m & (1u << 5))  { if (!has()) return true; out.currentMax   = bGetF32Auto(reply, i); }
    if (m & (1u << 6))  { if (!has()) return true; out.tempMosAvg   = bGetF32Auto(reply, i); }
    if (m & (1u << 7))  { if (!has()) return true; out.tempMosMax   = bGetF32Auto(reply, i); }
    if (m & (1u << 8))  { if (!has()) return true; out.tempMotorAvg = bGetF32Auto(reply, i); }
    if (m & (1u << 9))  { if (!has()) return true; out.tempMotorMax = bGetF32Auto(reply, i); }
    if (m & (1u << 10)) { if (!has()) return true; out.countTime    = bGetF32Auto(reply, i); }
    return true;
}

int VescProtocol::rawCommand(uint8_t cmd, uint8_t* reply, int maxReply, uint32_t timeoutMs) {
    uint8_t payload[1] = { cmd };
    return transact(payload, 1, cmd, reply, maxReply, timeoutMs);
}

void VescProtocol::setOdometerMeters(uint32_t meters) {
    if (_port == nullptr) return;
    uint8_t payload[5] = { VESC_COMM_SET_ODOMETER,
                           (uint8_t)(meters >> 24), (uint8_t)(meters >> 16),
                           (uint8_t)(meters >> 8),  (uint8_t)meters };
    // Fire-and-forget: the ESC sends no ack for SET_ODOMETER.
    uint8_t frame[16];
    int n = 0;
    frame[n++] = 0x02;
    frame[n++] = sizeof(payload);
    memcpy(frame + n, payload, sizeof(payload)); n += sizeof(payload);
    uint16_t crc = crc16(payload, sizeof(payload));
    frame[n++] = (uint8_t)(crc >> 8);
    frame[n++] = (uint8_t)(crc & 0xFF);
    frame[n++] = 0x03;
    _port->write(frame, n);
    _port->flush();
}

bool VescProtocol::probeCanId(uint8_t canId, uint32_t timeoutMs) {
    if (canId == 0) return false;
    uint8_t payload[7] = { VESC_COMM_FORWARD_CAN, canId, VESC_COMM_GET_VALUES_SELECTIVE,
                           0, 0, 0, (uint8_t)VESC_VAL_TEMP_FET };
    uint8_t reply[32];
    return transact(payload, sizeof(payload), VESC_COMM_GET_VALUES_SELECTIVE,
                    reply, sizeof(reply), timeoutMs) >= 5;
}

}
}
