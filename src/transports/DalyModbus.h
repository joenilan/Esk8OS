#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Esk8OS {
namespace Transports {
namespace DalyModbus {

// Verified against the physical UART K BLE module and DALY's official K/M/S
// BMSTool V1.14.60. The tool sends Modbus RTU reads to address 0x81 and receives
// replies from 0x51. These five consecutive 0x100-page registers are read-only
// here; this module intentionally exposes no Modbus write builder.
static const uint8_t  REQUEST_ADDRESS = 0x81;
static const uint8_t  RESPONSE_ADDRESS = 0x51;
static const uint8_t  READ_HOLDING_REGISTERS = 0x03;
static const uint16_t DISCHARGE_OC_FIRST_REGISTER = 0x0145;
static const uint16_t DISCHARGE_OC_REGISTER_COUNT = 5;
static const size_t   READ_REQUEST_SIZE = 8;
static const size_t   DISCHARGE_OC_RESPONSE_SIZE = 15;

struct DischargeOvercurrentRegisters {
    uint16_t warningRaw;
    uint16_t protection1Raw;
    uint16_t protection1DelayMs;
    uint16_t protection2Raw;
    uint16_t protection2DelayMs;
};

inline uint16_t crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

inline uint16_t be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

inline void buildDischargeOvercurrentRead(uint8_t out[READ_REQUEST_SIZE]) {
    out[0] = REQUEST_ADDRESS;
    out[1] = READ_HOLDING_REGISTERS;
    out[2] = (uint8_t)(DISCHARGE_OC_FIRST_REGISTER >> 8);
    out[3] = (uint8_t)DISCHARGE_OC_FIRST_REGISTER;
    out[4] = (uint8_t)(DISCHARGE_OC_REGISTER_COUNT >> 8);
    out[5] = (uint8_t)DISCHARGE_OC_REGISTER_COUNT;
    const uint16_t crc = crc16(out, 6);
    out[6] = (uint8_t)crc;         // Modbus RTU CRC is low byte first.
    out[7] = (uint8_t)(crc >> 8);
}

inline bool decodeDischargeOvercurrentResponse(
        const uint8_t* frame, size_t length,
        DischargeOvercurrentRegisters* out) {
    if (!frame || !out || length != DISCHARGE_OC_RESPONSE_SIZE) return false;
    if (frame[0] != RESPONSE_ADDRESS ||
        frame[1] != READ_HOLDING_REGISTERS ||
        frame[2] != DISCHARGE_OC_REGISTER_COUNT * 2) {
        return false;
    }

    const uint16_t expectedCrc = crc16(frame, length - 2);
    const uint16_t receivedCrc =
        (uint16_t)(frame[length - 2] | ((uint16_t)frame[length - 1] << 8));
    if (receivedCrc != expectedCrc) return false;

    DischargeOvercurrentRegisters decoded = {
        be16(&frame[3]),
        be16(&frame[5]),
        be16(&frame[7]),
        be16(&frame[9]),
        be16(&frame[11])
    };

    // DALY encodes discharge-current thresholds as (A * 10) + 30000.
    // Values below the zero-current offset cannot represent a valid positive
    // discharge threshold and are rejected instead of becoming huge unsigned
    // values or misleading negative limits.
    if (decoded.warningRaw < 30000 ||
        decoded.protection1Raw < 30000 ||
        decoded.protection2Raw < 30000) {
        return false;
    }

    *out = decoded;
    return true;
}

inline float dischargeCurrentAmps(uint16_t raw) {
    return ((int32_t)raw - 30000) / 10.0f;
}

}  // namespace DalyModbus
}  // namespace Transports
}  // namespace Esk8OS
