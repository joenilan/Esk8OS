#include "telemetry/AuxTelemetry.h"

namespace Esk8OS {
namespace Telemetry {

AuxNodeState globalAuxNode;

void processAuxPacket(const uint8_t* mac, const uint8_t* data, int len) {
    if (len != sizeof(AuxTelemetryPacketV1)) return;

    AuxTelemetryPacketV1 packet;
    memcpy(&packet, data, sizeof(packet));

    if (packet.magic != 0xE805 || packet.version != 1) return;

    // TODO: Verify CRC32 if needed
    
    globalAuxNode.nodeId = packet.nodeId;
    globalAuxNode.lastSeq = packet.seq;
    globalAuxNode.lastSeenMs = millis();

    if (packet.packMilliVolts != -1) {
        globalAuxNode.packVoltage = packet.packMilliVolts / 1000.0f;
    }
    if (packet.tempCx10 != -32768) {
        globalAuxNode.tempC = packet.tempCx10 / 10.0f;
    }
    if (packet.humidityX10 != -1) {
        globalAuxNode.humidityPct = packet.humidityX10 / 10.0f;
    }
}

}
}
