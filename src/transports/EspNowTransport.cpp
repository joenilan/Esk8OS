#include "transports/EspNowTransport.h"
#include "telemetry/AuxTelemetry.h"
#include "esk8os.h"
#include <WiFi.h>
#include <esp_now.h>

namespace Esk8OS {
namespace Transports {

static void onEspNowReceive(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
    Esk8OS::Telemetry::processAuxPacket(mac_addr, data, data_len);
}

void beginEspNow() {
    if (!gEspnowEnabled) return;

    WiFi.mode(WIFI_STA); // Or WIFI_AP_STA if bridging
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW Init Failed");
        return;
    }
    
    esp_now_register_recv_cb(onEspNowReceive);
    Serial.println("ESP-NOW Initialized successfully");
}

}
}
