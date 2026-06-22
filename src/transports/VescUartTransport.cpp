#include "transports/VescUartTransport.h"
#include <Arduino.h>

#ifndef WOKWI_SIMULATION
#include <VescUart.h>
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

                // Add the second motor over CAN. getVescValues(canId) forwards the
                // request through the master; on success UART.data holds the slave.
                // Sum the additive quantities; for temps keep the hotter so a
                // warning trips on either motor/ESC. Voltage/rpm/duty are shared.
                if (VESC_SLAVE_CAN_ID != 0 && UART.getVescValues(VESC_SLAVE_CAN_ID)) {
                    inA       += UART.data.avgInputCurrent;
                    motA      += UART.data.avgMotorCurrent;
                    wh        += UART.data.wattHours;
                    whCharged += UART.data.wattHoursCharged;
                    if (UART.data.tempMotor  > tMotor)  tMotor  = UART.data.tempMotor;
                    if (UART.data.tempMosfet > tMosfet) tMosfet = UART.data.tempMosfet;
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
