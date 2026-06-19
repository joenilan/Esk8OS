// ============================================================================
// PHASE 1 PROBE (lovyangfx branch only)
//
// Minimal sketch to validate the LovyanGFX config on real hardware BEFORE
// porting the full UI: confirms the panel comes up with correct orientation,
// colors (RGB order + inversion), and offset, then measures the full-frame
// DMA blit time / FPS so we can compare against TFT_eSPI's ~38 ms / ~25 fps.
//
// main.cpp is excluded from the build while this probe runs (see
// build_src_filter in platformio.ini). Once verified on the bench, we flip the
// filter back and port the real dashboard onto LovyanGFX.
// ============================================================================
#include <Arduino.h>
#include "LGFX_Config.h"

static LGFX        tft;
static LGFX_Sprite canvas(&tft);

static uint16_t COL_BG;

void setup() {
    Serial.begin(115200);

    #ifndef WOKWI_SIMULATION
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);          // T-Display-S3 display power rail
    #endif

    tft.init();
    tft.setRotation(0);
    tft.setBrightness(255);
    tft.fillScreen(0);

    COL_BG = tft.color565(26, 26, 26);

    canvas.setColorDepth(16);
    canvas.setPsram(false);          // fast internal SRAM
    bool ok = (canvas.createSprite(tft.width(), tft.height()) != nullptr);

    canvas.fillScreen(COL_BG);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(tft.color565(255, 255, 255));
    canvas.setFont(&fonts::Font4);
    canvas.drawString("LovyanGFX", tft.width() / 2, 24);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(String(tft.width()) + "x" + String(tft.height()), tft.width() / 2, 56);
    canvas.drawString(ok ? "sprite: SRAM ok" : "sprite: FAILED", tft.width() / 2, 76);

    // RGB-order / inversion check: these must read red, green, blue left-to-right.
    canvas.fillRect(14, 100, 42, 42, tft.color565(255, 0, 0));
    canvas.fillRect(64, 100, 42, 42, tft.color565(0, 255, 0));
    canvas.fillRect(114, 100, 42, 42, tft.color565(0, 0, 255));

    canvas.pushSprite(0, 0);
}

void loop() {
    // Hammer full-frame blits and report DMA push time + FPS.
    static uint32_t frames = 0;
    static uint32_t window = millis();
    static uint32_t lastUs = 0;

    uint32_t a = micros();
    canvas.pushSprite(0, 0);
    lastUs = micros() - a;
    frames++;

    if (millis() - window >= 1000) {
        canvas.fillRect(0, 160, tft.width(), 90, COL_BG);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(tft.color565(0, 200, 100));
        canvas.setFont(&fonts::Font4);
        canvas.drawString(String(frames) + " fps", tft.width() / 2, 184);
        canvas.setTextColor(tft.color565(255, 255, 255));
        canvas.drawString(String(lastUs / 1000.0, 2) + " ms", tft.width() / 2, 216);
        frames = 0;
        window = millis();
    }
}
