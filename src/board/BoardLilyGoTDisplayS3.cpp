#include "BoardLilyGoTDisplayS3.h"
#include "../LGFX_Config.h"

// Hardware Pin Definitions
#define PIN_DISPLAY_PWR 15
#define PIN_BTN_LEFT    0
#define PIN_BTN_RIGHT   14

extern LGFX tft;

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
    pinMode(PIN_BTN_LEFT, INPUT_PULLUP);
    pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
    
    // In future, battery ADC and touch initialization would go here.
}

void enableDisplayPower() {
    pinMode(PIN_DISPLAY_PWR, OUTPUT);
    digitalWrite(PIN_DISPLAY_PWR, HIGH);
    status.displayPowerEnabled = true;
}

void setBacklight(uint8_t brightness) {
    status.brightness = brightness;
    tft.setBrightness(brightness);
}

uint8_t getBacklight() {
    return status.brightness;
}

BoardStatus readStatus() {
    return status;
}

bool buttonA() {
    return digitalRead(PIN_BTN_LEFT) == LOW; // LOW means pressed due to INPUT_PULLUP
}

bool buttonB() {
    return digitalRead(PIN_BTN_RIGHT) == LOW; // LOW means pressed due to INPUT_PULLUP
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
