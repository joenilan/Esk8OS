#pragma once
#include <Arduino.h>

namespace Esk8OS {
namespace Telemetry {

#pragma pack(push, 1)
struct AuxTelemetryPacketV1 {
    uint16_t magic;          // 0xE805
    uint8_t version;         // 1
    uint8_t nodeType;        // battery, enclosure, gps, etc.
    uint32_t nodeId;
    uint32_t seq;
    uint32_t uptimeMs;

    int32_t packMilliVolts;  // -1 if unavailable
    int16_t tempCx10;        // -32768 if unavailable
    int16_t humidityX10;     // -1 if unavailable

    uint16_t flags;
    uint32_t crc32;
};
#pragma pack(pop)

struct AuxNodeState {
    uint32_t nodeId = 0;
    uint32_t lastSeq = 0;
    unsigned long lastSeenMs = 0;

    float packVoltage = -1.0f;
    float tempC = -273.15f;
    float humidityPct = -1.0f;

    bool isStale() const {
        // Mark stale if unseen for 5 seconds
        return (millis() - lastSeenMs > 5000) || (lastSeenMs == 0);
    }
};

extern AuxNodeState globalAuxNode;

void processAuxPacket(const uint8_t* mac, const uint8_t* data, int len);

}
}
