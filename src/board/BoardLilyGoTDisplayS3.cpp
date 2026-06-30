#include "BoardLilyGoTDisplayS3.h"
#if ESK8OS_DISPLAY_TFT
#include "../LGFX_Config.h"
#endif

// Hardware Pin Definitions
#define PIN_DISPLAY_PWR 15
#define PIN_BTN_LEFT    0
#define PIN_BTN_RIGHT   14

#if ESK8OS_DISPLAY_TFT
extern LGFX tft;
#endif

namespace Esk8OS {
namespace Board {

static BoardStatus status = {
    .displayPowerEnabled = false,
    .touchPresent = false,
    .batteryAdcValid = false,
    .boardBatteryVolts = 0.0f,
    .brightness = 0
};

void begin() {
#if ESK8OS_DISPLAY_TFT
    pinMode(PIN_BTN_LEFT, INPUT_PULLUP);
    pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
#endif
    
    // In future, battery ADC and touch initialization would go here.
}

void enableDisplayPower() {
#if ESK8OS_DISPLAY_TFT
    pinMode(PIN_DISPLAY_PWR, OUTPUT);
    digitalWrite(PIN_DISPLAY_PWR, HIGH);
    status.displayPowerEnabled = true;
#endif
}

void setBacklight(uint8_t brightness) {
    status.brightness = brightness;
#if ESK8OS_DISPLAY_TFT
    tft.setBrightness(brightness);
#endif
}

uint8_t getBacklight() {
    return status.brightness;
}

BoardStatus readStatus() {
    return status;
}

bool buttonA() {
#if ESK8OS_DISPLAY_TFT
    return digitalRead(PIN_BTN_LEFT) == LOW; // LOW means pressed due to INPUT_PULLUP
#else
    return false;
#endif
}

bool buttonB() {
#if ESK8OS_DISPLAY_TFT
    return digitalRead(PIN_BTN_RIGHT) == LOW; // LOW means pressed due to INPUT_PULLUP
#else
    return false;
#endif
}

#if ESK8OS_HAS_TOUCH
bool touchAvailable() {
    return status.touchPresent;
}

bool readTouch(uint16_t& x, uint16_t& y) {
    // Stub for touch read
    return false;
}
#endif

} // namespace Board
} // namespace Esk8OS
