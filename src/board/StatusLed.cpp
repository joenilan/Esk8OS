#include "board/StatusLed.h"
#include <Arduino.h>
#include "esk8os.h"
#include "services/webexport.h"

#ifndef ESK8OS_STATUS_RGB
#define ESK8OS_STATUS_RGB 0
#endif

#ifndef ESK8OS_STATUS_RGB_PIN
#define ESK8OS_STATUS_RGB_PIN 48
#endif

namespace Esk8OS {
namespace StatusLed {

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

#if ESK8OS_STATUS_RGB
static Rgb gLast = {255, 255, 255};

static Rgb scale(Rgb c, uint8_t pct) {
    return {
        (uint8_t)((uint16_t)c.r * pct / 100),
        (uint8_t)((uint16_t)c.g * pct / 100),
        (uint8_t)((uint16_t)c.b * pct / 100),
    };
}

static void write(Rgb c) {
    if (c.r == gLast.r && c.g == gLast.g && c.b == gLast.b) return;
    neopixelWrite(ESK8OS_STATUS_RGB_PIN, c.r, c.g, c.b);
    gLast = c;
}

static Rgb blink(Rgb on, Rgb off, uint16_t periodMs = 500) {
    return ((millis() / periodMs) & 1) ? on : off;
}
#endif

void begin() {
#if ESK8OS_STATUS_RGB
    pinMode(ESK8OS_STATUS_RGB_PIN, OUTPUT);
    if (!gStatusRgbEnabled) {
        neopixelWrite(ESK8OS_STATUS_RGB_PIN, 0, 0, 0);
        gLast = {0, 0, 0};
        return;
    }
    write(scale({0, 80, 255}, 20)); // booting / initialized
#endif
}

void tick() {
#if ESK8OS_STATUS_RGB
    static unsigned long lastMs = 0;
    if (millis() - lastMs < 100) return;
    lastMs = millis();

    const Rgb off = {0, 0, 0};
    if (!gStatusRgbEnabled) {
        write(off);
        return;
    }
    Rgb color = {0, 24, 60}; // idle / waiting

    if (systemMode == MODE_VESC_BRIDGE || webServiceActive()) {
        color = scale({0, 180, 255}, 30);       // bridge / WiFi export
    } else if (vescFault != 0) {
        color = blink(scale({255, 0, 0}, 35), off, 250);
    } else if (rangeAlertState == 3) {
        color = blink(scale({255, 0, 0}, 35), off, 350);
    } else if (rangeAlertState == 2) {
        color = blink(scale({255, 90, 0}, 35), off, 400);
    } else if (rangeAlertState == 1) {
        color = scale({255, 180, 0}, 28);
    } else if (gDemoMode) {
        color = scale({160, 0, 255}, 24);       // demo telemetry
    } else if (telemetryLive && vescLinkOk) {
        color = scale({0, 255, 60}, 22);        // real VESC telemetry
    } else {
        color = blink(scale({255, 90, 0}, 25), scale({20, 6, 0}, 18), 700);
    }

    write(color);
#endif
}

} // namespace StatusLed
} // namespace Esk8OS
