#include "transports/VescUartTransport.h"
#include <Arduino.h>

#ifndef WOKWI_SIMULATION
#include <VescUart.h>
#include <buffer.h>
#include <crc.h>
VescUart UART;
#endif

namespace Esk8OS {
namespace Transports {

static RawVescData gRawData;
static bool gHasNewData = false;
static SemaphoreHandle_t gDataMutex = NULL;
static bool gPollPaused = false;
static TaskHandle_t gVescTaskHandle = NULL;

// Second motor's VESC CAN ID (the slave's "Controller ID" in VESC Tool). The
// poll task adds the slave's battery current + watt-hours to the master so
// power, energy and the Wh/mi efficiency reflect BOTH motors — reading the
// master alone undercounts a dual drive by ~half. If the slave doesn't answer
// (single-motor build, CAN not set up, or wrong ID) we fall back to the master
// alone, so this is safe either way. Set to match your slave; 0 = local/master.
// 114 = Joe's slave Controller ID (from VESC Tool's CAN Devices list).
static const uint8_t VESC_SLAVE_CAN_ID = 114;

#ifndef WOKWI_SIMULATION
// Send a SHORT VESC command packet (payload < 256 B) and read the reply payload.
// The bundled VescUart lib keeps its packet framing private, so this is a minimal
// self-contained round-trip using the lib's crc16/buffer helpers. Only ever called
// sequentially from vescPollTask (same Serial1 as getVescValues — no concurrency).
// Returns reply payload length, or 0 on timeout/CRC/format error.
static int vescShortCommand(uint8_t cmd, uint8_t* reply, int maxReply) {
    while (Serial1.available()) Serial1.read();      // drop stale bytes
    uint8_t payload = cmd;
    uint16_t crc = crc16(&payload, 1);
    uint8_t pkt[6] = { 0x02, 0x01, cmd, (uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF), 0x03 };
    Serial1.write(pkt, sizeof(pkt));
    Serial1.flush();

    const uint32_t start = millis();
    auto readByte = [&](int& out) -> bool {
        while (!Serial1.available()) {
            if (millis() - start > 50) return false;
        }
        out = Serial1.read();
        return true;
    };

    int b;
    do { if (!readByte(b)) return 0; } while (b != 0x02);   // sync to short-packet start
    int len;
    if (!readByte(len) || len <= 0 || len > maxReply) return 0;
    for (int i = 0; i < len; i++) { if (!readByte(b)) return 0; reply[i] = (uint8_t)b; }
    int crcHi, crcLo, stop;
    if (!readByte(crcHi) || !readByte(crcLo) || !readByte(stop)) return 0;
    if (stop != 0x03) return 0;
    if (((crcHi << 8) | crcLo) != crc16(reply, len)) return 0;
    return len;
}

// Read the master VESC's decoded remote input (COMM_GET_DECODED_PPM): throttle
// level (-1..1) and last pulse length. Reply = [cmd, int32 level*1e6, int32 ms*1e6].
static bool readDecodedPpm(float* decoded, float* pulseMs) {
    uint8_t reply[32];
    int n = vescShortCommand(COMM_GET_DECODED_PPM, reply, sizeof(reply));
    if (n < 9 || reply[0] != COMM_GET_DECODED_PPM) return false;
    int32_t idx = 1;
    *decoded = buffer_get_int32(reply, &idx) / 1000000.0f;
    *pulseMs = buffer_get_int32(reply, &idx) / 1000000.0f;
    return true;
}

static void vescPollTask(void* pvParameters) {
    for (;;) {
        if (!gPollPaused) {
            if (UART.getVescValues()) {
                // Master (shared pack voltage + this motor's current/energy/temps).
                float rpm      = UART.data.rpm;
                float inpV     = UART.data.inpVoltage;
                float tMotor   = UART.data.tempMotor;
                float tMosfet  = UART.data.tempMosfet;
                float inA      = UART.data.avgInputCurrent;
                float motA     = UART.data.avgMotorCurrent;
                float duty     = UART.data.dutyCycleNow;
                float wh       = UART.data.wattHours;
                float whCharged= UART.data.wattHoursCharged;
                int   err      = UART.data.error;

                // Per-motor (master) values kept for the diagnostics view.
                float mMotA = motA, mTMotor = tMotor, mTMosfet = tMosfet;
                float sMotA = 0, sTMotor = 0, sTMosfet = 0;

                // Add the second motor over CAN. getVescValues(canId) forwards the
                // request through the master; on success UART.data holds the slave.
                // Sum the additive quantities; for temps keep the hotter so a
                // warning trips on either motor/ESC. Voltage/rpm/duty are shared.
                bool slaveOnline = false;
                if (VESC_SLAVE_CAN_ID != 0 && UART.getVescValues(VESC_SLAVE_CAN_ID)) {
                    slaveOnline = true;
                    sMotA = UART.data.avgMotorCurrent;
                    sTMotor = UART.data.tempMotor;
                    sTMosfet = UART.data.tempMosfet;
                    inA       += UART.data.avgInputCurrent;
                    motA      += UART.data.avgMotorCurrent;
                    wh        += UART.data.wattHours;
                    whCharged += UART.data.wattHoursCharged;
                    if (UART.data.tempMotor  > tMotor)  tMotor  = UART.data.tempMotor;
                    if (UART.data.tempMosfet > tMosfet) tMosfet = UART.data.tempMosfet;
                }

                // Decoded remote input from the master. NOTE: over PPM "connected"
                // really means "a valid pulse is present" — it CANNOT distinguish the
                // handheld being OFF from ON-but-idle, because the receiver failsafes
                // to the same center pulse (~1.49 ms) in both cases (measured). Real
                // remote link/battery would need the VX1's UART telemetry on a COMM port.
                float ppmDec = 0, ppmMs = 0;
                bool ppmOk = readDecodedPpm(&ppmDec, &ppmMs);
                bool ppmConn = ppmOk && ppmMs > 0.5f && ppmMs < 2.5f;

                // VESC firmware version: read once (it doesn't change), then cache.
                static uint8_t fwMaj = 0, fwMin = 0;
                if (fwMaj == 0 && UART.getFWversion()) {
                    fwMaj = UART.fw_version.major;
                    fwMin = UART.fw_version.minor;
                }

                if (xSemaphoreTake(gDataMutex, portMAX_DELAY) == pdTRUE) {
                    gRawData.rpm = rpm;
                    gRawData.inpVoltage = inpV;
                    gRawData.tempMotor = tMotor;
                    gRawData.tempMosfet = tMosfet;
                    gRawData.avgInputCurrent = inA;
                    gRawData.avgMotorCurrent = motA;
                    gRawData.dutyCycleNow = duty;
                    gRawData.wattHours = wh;
                    gRawData.wattHoursCharged = whCharged;
                    gRawData.error = err;
                    gRawData.ppmDecoded = ppmDec;
                    gRawData.ppmPulseMs = ppmMs;
                    gRawData.ppmConnected = ppmConn;
                    gRawData.fwMajor = fwMaj;
                    gRawData.fwMinor = fwMin;
                    gRawData.slaveOnline = slaveOnline;
                    gRawData.masterMotorAmps = mMotA;
                    gRawData.slaveMotorAmps = sMotA;
                    gRawData.masterTempMotor = mTMotor;
                    gRawData.slaveTempMotor = sTMotor;
                    gRawData.masterTempMosfet = mTMosfet;
                    gRawData.slaveTempMosfet = sTMosfet;
                    gHasNewData = true;
                    xSemaphoreGive(gDataMutex);
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
    UART.setSerialPort(&Serial1);
    
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
