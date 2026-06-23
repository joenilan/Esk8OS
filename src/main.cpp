#include <Arduino.h>
#include "esk8os.h"
#include "board/BoardLilyGoTDisplayS3.h"
#include "config/Settings.h"
#include "ui/UiRenderer.h"
#include "ui/ui.h"
#include "app/App.h"
#include "transports/VescUartTransport.h"
#include "services/companion_ble.h"
#include "telemetry/telemetry.h"
#include "logging/ridelog.h"

// Forward declares for functions we still call in setup but haven't extracted yet
void applyBrightness();

void setup() {
    Serial.begin(115200);
    delay(500);

    ridelogBegin();
    Esk8OS::Settings::begin();

    rideStartMs = millis();
    sessionTripStartKm = tripDistanceKm;
    sessionMovingStartSec = tripMovingSec;   // AVG = distance / moving-time since this baseline

    #ifndef WOKWI_SIMULATION
    Esk8OS::Board::enableDisplayPower();
    #endif

    Esk8OS::UiRenderer::begin();
    applyTheme(gThemeIdx);
    
    Esk8OS::Board::begin();
    Esk8OS::Transports::beginVescUart();

    // Bring up the companion BLE service now so it advertises 100% of the time.
    // It owns the NimBLE stack; the VESC-Tool bridge co-hosts on the same server.
    companionBleBegin();

    #ifndef WOKWI_SIMULATION
    applyBrightness();
    #endif

    waitForBootReady();
    drawStaticFrame();
    updateRangeEstimate();
    updateClock();
    updateCurrentPageContent();
    updateBatteryCells();
    updateBottomBar();
    Esk8OS::UiRenderer::pushCanvasFull();
    gRedrawAll = false;

    ridelogStartRide();
}

void loop() {
    Esk8OS::App::loop();
}
