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

#ifndef WOKWI_SIMULATION
static void vescPollTask(void* pvParameters) {
    for (;;) {
        if (!gPollPaused) {
            if (UART.getVescValues()) {
                if (xSemaphoreTake(gDataMutex, portMAX_DELAY) == pdTRUE) {
                    gRawData.rpm = UART.data.rpm;
                    gRawData.inpVoltage = UART.data.inpVoltage;
                    gRawData.tempMotor = UART.data.tempMotor;
                    gRawData.tempMosfet = UART.data.tempMosfet;
                    gRawData.avgInputCurrent = UART.data.avgInputCurrent;
                    gRawData.avgMotorCurrent = UART.data.avgMotorCurrent;
                    gRawData.dutyCycleNow = UART.data.dutyCycleNow;
                    gRawData.wattHours = UART.data.wattHours;
                    gRawData.wattHoursCharged = UART.data.wattHoursCharged;
                    gRawData.error = UART.data.error;
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
