#pragma once
#include <Arduino.h>

namespace Esk8OS {
namespace Board {

struct BoardStatus {
    bool displayPowerEnabled;
    bool touchPresent;
    bool batteryAdcValid;
    float boardBatteryVolts;
    uint8_t brightness;
};

void begin();
void enableDisplayPower();
void setBacklight(uint8_t brightness);
uint8_t getBacklight();
BoardStatus readStatus();

bool buttonA();
bool buttonB();

#if ESK8OS_HAS_TOUCH
bool touchAvailable();
bool readTouch(uint16_t& x, uint16_t& y);
#endif

} // namespace Board
} // namespace Esk8OS
