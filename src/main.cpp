#include <Arduino.h>
#include "esk8os.h"
#include "board/BoardLilyGoTDisplayS3.h"
#include "board/StatusLed.h"
#include "config/Settings.h"
#include "ui/UiRenderer.h"
#include "ui/ui.h"
#include "app/App.h"
#include "transports/VescUartTransport.h"
#include "transports/DalyBms.h"
#include "services/companion_ble.h"
#include "telemetry/telemetry.h"
#include "logging/sessionlog.h"

// Forward declares for functions we still call in setup but haven't extracted yet
void applyBrightness();

void setup() {
    Serial.begin(115200);
#if ESK8OS_DUAL_CONSOLE
    Serial0.begin(115200);
#endif
    delay(500);
    Esk8OS::StatusLed::begin();

    sessionLogBegin();
    Esk8OS::Settings::begin();

    rideStartMs = millis();
    sessionTripStartKm = tripDistanceKm;
    sessionMovingStartSec = tripMovingSec;   // AVG = distance / moving-time since this baseline

    #if ESK8OS_DISPLAY_TFT && !defined(WOKWI_SIMULATION)
    Esk8OS::Board::enableDisplayPower();
    #endif

    Esk8OS::UiRenderer::begin();
    applyTheme(gThemeIdx);
    Esk8OS::UiRenderer::showBootSplash(28, "THEME");
    
    Esk8OS::Board::begin();
    Esk8OS::UiRenderer::showBootSplash(44, "BOARD");
    Esk8OS::Transports::beginVescUart(prefs.getUChar("slaveId", 0));
    Esk8OS::Transports::beginDalyBms();   // no-op unless -DESK8OS_BMS_DALY
    Esk8OS::UiRenderer::showBootSplash(60, "UART");

    // Bring up the companion BLE service now so it advertises 100% of the time.
    // It owns the NimBLE stack; the VESC-Tool bridge co-hosts on the same server.
    companionBleBegin();
    Esk8OS::UiRenderer::showBootSplash(78, "PHONE");

    #if ESK8OS_DISPLAY_TFT && !defined(WOKWI_SIMULATION)
    applyBrightness();
    #endif

    waitForBootReady();
    // If the boot search actually found the VESC (lastVescOkMs set), pull that real
    // reading into the display now — the background poll task already has it — so the
    // first HUD frame shows true telemetry instead of the 0 placeholder (no flash /
    // no VESC-LINK-LOST blip). Skipped entirely when no VESC, so the bench adds no delay.
    if (lastVescOkMs != 0) {
        unsigned long t0 = millis();
        while (!telemetryLive && millis() - t0 < 250) { pollVescData(); delay(5); }
    }
    Esk8OS::UiRenderer::showBootSplash(92, "TELEMETRY");
    drawStaticFrame();
    updateRangeEstimate();
    updateClock();
    updateCurrentPageContent();
    updateBatteryCells();
    updateBottomBar();
    Esk8OS::UiRenderer::showBootSplash(100, "READY");
#if ESK8OS_DISPLAY_OLED
    delay(250);
#endif
    Esk8OS::UiRenderer::pushCanvasFull();
    gRedrawAll = false;

    sessionLogStart();
}

void loop() {
    Esk8OS::App::loop();
}
