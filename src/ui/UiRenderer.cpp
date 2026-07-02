#include "ui/UiRenderer.h"
#include "board/BoardLilyGoTDisplayS3.h"

#if ESK8OS_DISPLAY_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#ifndef OLED_SDA
#define OLED_SDA 8
#endif
#ifndef OLED_SCL
#define OLED_SCL 9
#endif
#ifndef OLED_ADDR
#define OLED_ADDR 0x3C
#endif
static Adafruit_SSD1306 oled(128, 32, &Wire, -1);
static bool gOledReady = false;
static bool gOledDimmed = false;
static unsigned long gOledSaverPreviewUntil = 0;

static bool oledTelemetryLive() {
    return telemetryLive && (gDemoMode || vescLinkOk);
}

static uint8_t oledContrastForPct(int pct) {
    pct = constrain(pct, 10, 100);
    // SSD1306 contrast is visually nonlinear; use a squared curve so the low
    // end is actually dim instead of just slightly less bright.
    uint32_t p = (uint32_t)pct;
    uint32_t curved = (p * p * 255UL) / 10000UL;
    return (uint8_t)constrain((int)curved, 1, 255);
}

static void applyOledContrast() {
    if (!gOledReady) return;
    uint8_t contrast = oledContrastForPct(gBrightnessPct);
    if (gOledDimmed) contrast = min<uint8_t>(contrast, 8);
    oled.ssd1306_command(SSD1306_SETCONTRAST);
    oled.ssd1306_command(contrast);
}

static void setOledDimmed(bool dimmed) {
    if (gOledDimmed == dimmed) return;
    oled.dim(dimmed);
    gOledDimmed = dimmed;
    applyOledContrast();
}

static bool oledShouldDim(bool screensaverActive) {
    return screensaverActive || currentBatteryPercent <= 15;
}

static void drawOledBootSplash(uint8_t progressPct, const char* status) {
    progressPct = constrain(progressPct, 0, 100);
    if (!status) status = "";

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.print("EVEE");

    oled.setTextSize(1);
    oled.setCursor(70, 1);
    oled.print("ESK8OS");
    oled.setCursor(70, 11);
    oled.print(status);

    const int barX = 0;
    const int barY = 24;
    const int barW = 128;
    const int barH = 8;
    oled.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    int fillW = map(progressPct, 0, 100, 0, barW - 2);
    if (fillW > 0) oled.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);

    int sheen = (millis() / 70) % 18;
    for (int x = barX + 2 + sheen; x < barX + 1 + fillW; x += 18) {
        oled.drawLine(x, barY + 1, min(x + 5, barX + fillW), barY + barH - 2, SSD1306_BLACK);
    }

    oled.display();
}

static void drawOledBadge(const char* label) {
    if (!label || !label[0]) return;
    int16_t w = strlen(label) * 6 + 6;
    int16_t x = 128 - w;
    oled.fillRect(x, 0, w, 10, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setTextSize(1);
    oled.setCursor(x + 3, 1);
    oled.print(label);
    oled.setTextColor(SSD1306_WHITE);
}

static const char* oledRangeBadge(int alertState) {
    if (alertState == 7 || rangeAlertState == 3) return "LIMP";
    if (alertState == 8 || rangeAlertState == 2) return "SAG";
    if (alertState == 9 || rangeAlertState == 1) return "TURN";
    return "";
}

static void drawOledSegmentBar(int y, int h, float fill, uint8_t segments) {
    fill = constrain(fill, 0.0f, 1.0f);
    if (segments < 1) segments = 1;
    const int gap = 1;
    const int w = (128 - (segments - 1) * gap) / segments;
    float filled = fill * segments;
    for (uint8_t i = 0; i < segments; i++) {
        int x = i * (w + gap);
        oled.drawRect(x, y, w, h, SSD1306_WHITE);
        float part = constrain(filled - i, 0.0f, 1.0f);
        if (part > 0.0f) {
            int fw = max(1, (int)roundf((w - 2) * part));
            oled.fillRect(x + 1, y + 1, fw, h - 2, SSD1306_WHITE);
        }
    }
}

static void drawOledSideBattery(float fill) {
    fill = constrain(fill, 0.0f, 1.0f);
    const int x = 113;
    const int y = 1;
    const int w = 14;
    const int h = 30;
    const int capH = 3;
    oled.drawRect(x + 3, y, w - 6, capH, SSD1306_WHITE);
    oled.drawRect(x, y + capH, w, h - capH, SSD1306_WHITE);
    const int segments = 6;
    const int gap = 1;
    const int innerX = x + 2;
    const int innerY = y + capH + 2;
    const int innerW = w - 4;
    const int innerH = h - capH - 4;
    const int segH = (innerH - (segments - 1) * gap) / segments;
    int lit = (int)ceilf(fill * segments);
    for (int i = 0; i < segments; i++) {
        int segFromTop = segments - 1 - i;
        int sy = innerY + segFromTop * (segH + gap);
        if (i < lit) oled.fillRect(innerX, sy, innerW, segH, SSD1306_WHITE);
    }
}

static void drawOledAmazonBatteryIcon(float fill) {
    fill = constrain(fill, 0.0f, 1.0f);
    const int x = 1;
    const int y = 2;
    const int w = 65;
    const int h = 28;
    const int nubW = 4;
    const int nubH = 10;
    oled.drawRect(x, y, w, h, SSD1306_WHITE);
    oled.drawRect(x + w, y + (h - nubH) / 2, nubW, nubH, SSD1306_WHITE);

    const int segments = 5;
    const int segW = 9;
    const int gap = 2;
    const int lit = (int)ceilf(fill * segments);
    for (int i = 0; i < segments; i++) {
        int sx = x + 6 + i * (segW + gap);
        int sy = y + 5;
        int slant = 3;
        if (i < lit) {
            oled.fillTriangle(sx + slant, sy, sx + segW, sy, sx, sy + h - 10, SSD1306_WHITE);
            oled.fillTriangle(sx + segW, sy, sx + segW - slant, sy + h - 10, sx, sy + h - 10, SSD1306_WHITE);
        } else {
            oled.drawLine(sx + slant, sy, sx + segW, sy, SSD1306_WHITE);
            oled.drawLine(sx + segW, sy, sx + segW - slant, sy + h - 10, SSD1306_WHITE);
            oled.drawLine(sx + segW - slant, sy + h - 10, sx, sy + h - 10, SSD1306_WHITE);
            oled.drawLine(sx, sy + h - 10, sx + slant, sy, SSD1306_WHITE);
        }
    }
}

static void drawOledLcdValue(float value, const char* unit, bool oneDecimal, bool tight) {
    char buf[10];
    if (oneDecimal) snprintf(buf, sizeof(buf), "%.1f", value);
    else snprintf(buf, sizeof(buf), "%d", (int)lroundf(value));

    oled.setTextSize(2);
    int16_t x1, y1;
    uint16_t bw, bh;
    oled.getTextBounds(buf, 0, 0, &x1, &y1, &bw, &bh);
    int unitW = (unit && unit[0]) ? (int)strlen(unit) * 6 : 0;
    int x = 127 - (int)bw - unitW;
    if (tight && x < 68) x = 68;
    else if (x < 70) x = 70;
    oled.setCursor(x, 8);
    oled.print(buf);

    if (unit && unit[0]) {
        oled.setTextSize(1);
        oled.setCursor(127 - unitW, 8);
        oled.print(unit);
    }
}

static void drawOledAmazonMeter(bool voltageValue, bool cellVoltage) {
    drawOledAmazonBatteryIcon(currentBatteryPercent / 100.0f);
    if (voltageValue) {
        drawOledLcdValue(cellVoltage ? loadedCellVoltage : currentVoltage,
                         cellVoltage ? "" : "V",
                         true,
                         cellVoltage);
    } else {
        drawOledLcdValue(currentBatteryPercent, "%", false, false);
    }
}

static void drawOledPowerBar(int y, int h) {
    float watts = max(0.0f, currentWatts);
    float maxW = max(1200.0f, max(peakWatts, maxWattsSession));
    maxW = min(maxW, 5000.0f);
    drawOledSegmentBar(y, h, watts / maxW, 16);
}

static void drawOledRangeSuffix() {
    oled.print(useMph ? remainingRangeKm * 0.621371f : remainingRangeKm, 1);
    oled.print(useMph ? "mi" : "km");
}

static void drawOledBatteryBar() {
    int segments = BATTERY_CELLS_COUNT < 6 ? 6 : BATTERY_CELLS_COUNT;
    drawOledSegmentBar(25, 7, currentBatteryPercent / 100.0f, (uint8_t)segments);
}

static void drawOledRightMiniBattery() {
    const int x = 112;
    const int y = 2;
    const int w = 13;
    const int h = 20;
    oled.drawRect(x, y, w, h, SSD1306_WHITE);
    oled.drawRect(x + 4, y - 2, 5, 2, SSD1306_WHITE);
    int fillH = (int)roundf((h - 4) * constrain(currentBatteryPercent / 100.0f, 0.0f, 1.0f));
    if (fillH > 0) oled.fillRect(x + 2, y + h - 2 - fillH, w - 4, fillH, SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(109, 25);
    oled.print(currentBatteryPercent);
}

static void drawOledSpeedFace() {
    int spd = (int)(useMph ? currentSpeedMph : currentSpeedKmh);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", spd);

    oled.setTextSize(3);
    int16_t x1, y1;
    uint16_t bw, bh;
    oled.getTextBounds(buf, 0, 0, &x1, &y1, &bw, &bh);
    int x = 0;
    if (bw < 58) x = (58 - (int)bw) / 2;
    oled.setCursor(x, 3);
    oled.print(buf);

    oled.setTextSize(1);
    oled.setCursor(62, 4);
    oled.print(useMph ? "MPH" : "KMH");
    oled.setCursor(62, 15);
    oled.print(currentVoltage, 1);
    oled.print("V");
    oled.setCursor(62, 25);
    oled.print(currentWatts >= 1000 ? (int)(currentWatts / 1000.0f) : (int)currentWatts);
    oled.print(currentWatts >= 1000 ? "kW" : "W");
    drawOledRightMiniBattery();
}

static void drawOledWattsFace() {
    float watts = max(0.0f, currentWatts);
    char value[10];
    const char* unit = "W";
    if (watts >= 1000.0f) {
        snprintf(value, sizeof(value), "%.1f", watts / 1000.0f);
        unit = "kW";
    } else {
        snprintf(value, sizeof(value), "%d", (int)watts);
    }

    oled.setTextSize(2);
    int16_t x1, y1;
    uint16_t bw, bh;
    oled.getTextBounds(value, 0, 0, &x1, &y1, &bw, &bh);
    int x = max(0, (70 - (int)bw) / 2);
    oled.setCursor(x, 1);
    oled.print(value);

    oled.setTextSize(1);
    oled.setCursor(min(72, x + (int)bw + 2), 3);
    oled.print(unit);

    oled.drawFastVLine(82, 1, 22, SSD1306_WHITE);
    oled.setCursor(88, 2);
    oled.print(currentAmps, 0);
    oled.print("A");
    oled.setCursor(88, 13);
    oled.print(currentVoltage, 1);
    oled.print("V");

    drawOledPowerBar(25, 7);
}

static void drawOledScreensaver() {
    unsigned long t = millis();
    const int logoW = 58;
    const int logoH = 20;
    const int maxX = 128 - logoW;
    const int maxY = 32 - logoH;
    int phaseX = (int)((t / 75) % (maxX * 2));
    int phaseY = (int)((t / 115) % (maxY * 2));
    int x = phaseX <= maxX ? phaseX : (maxX * 2 - phaseX);
    int y = phaseY <= maxY ? phaseY : (maxY * 2 - phaseY);

    oled.drawRect(x, y, logoW, logoH, SSD1306_WHITE);
    oled.fillRect(x + 2, y + 2, logoW - 4, logoH - 4, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setTextSize(2);
    oled.setCursor(x + 6, y + 3);
    oled.print("EVEE");
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(1, 0);
    oled.print(currentBatteryPercent);
    oled.print("%");
    oled.setCursor(92, 24);
    oled.print(useMph ? remainingRangeKm * 0.621371f : remainingRangeKm, 0);
    oled.print(useMph ? "mi" : "km");
}

static void drawOledVoltageMeter(bool packVoltageHero) {
    drawOledAmazonMeter(true, !packVoltageHero);
}

static void drawOledBatteryFace(bool voltsFocus) {
    if (voltsFocus) {
        drawOledVoltageMeter(true);
        return;
    }

    oled.setTextSize(2);
    drawOledAmazonMeter(false, false);
}

static void drawOledSecondary() {
    oled.setTextSize(1);
    oled.setCursor(0, 18);
    if (rangeAlertState >= 2) {
        oled.print(loadedCellVoltage, 2);
        oled.print("V/c ");
        oled.print(currentAmps, 0);
        oled.print("A ");
        oled.print(useMph ? remainingLimpRangeKm * 0.621371f : remainingLimpRangeKm, 1);
        oled.print(useMph ? "mi" : "km");
    } else {
        oled.print(currentBatteryPercent);
        oled.print("% ");
        oled.print(currentVoltage, 1);
        oled.print("V ");
        drawOledRangeSuffix();
    }
}

static void drawOledLiveHud(int alertState) {
    const char* badge = oledRangeBadge(alertState);

    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2);
    oled.setCursor(0, 0);

    if (gHudFace == HUD_FACE_SAFETY) {
        drawOledVoltageMeter(false);
    } else if (gHudFace == HUD_FACE_BATTERY) {
        drawOledBatteryFace(gBatteryFocus == BATTERY_FOCUS_VOLTS);
    } else if (gHudFace == HUD_FACE_WATTS) {
        drawOledWattsFace();
    } else {
        drawOledSpeedFace();
    }

    drawOledBadge(badge);
}
#endif

#if ESK8OS_DISPLAY_TFT
LGFX tft;
LGFX_Sprite canvas(&tft);
lgfx::LovyanGFX* GFX = &tft;
#endif

bool gUseCanvas = false;
uint16_t gFps = 0;            
unsigned long gLastPushUs = 0; 

int X0 = 0; 
bool gRedrawAll = true;
int gCanvasH = 320;
bool gCanvasPsram = false;

struct DirtyBand { int16_t y0, y1; };
const int MAX_DIRTY = 10;
DirtyBand gDirty[MAX_DIRTY];
int gDirtyN = 0;

unsigned long gToastUntil = 0;
char gToastMsg[20] = "";

namespace Esk8OS {
namespace UiRenderer {

void begin() {
#if ESK8OS_DISPLAY_TFT
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.setBrightness(0);
    tft.fillScreen(0);

    X0 = (tft.width() - UI_W) / 2;
    if (X0 < 0) X0 = 0;

    #ifndef WOKWI_SIMULATION
    canvas.setColorDepth(16);
    canvas.setPsram(false);
    void* cbuf = canvas.createSprite(tft.width(), tft.height());
    if (cbuf == nullptr) {
        canvas.setPsram(true);
        cbuf = canvas.createSprite(tft.width(), tft.height());
        gCanvasPsram = true;
    }
    if (cbuf != nullptr) {
        GFX = &canvas;
        gUseCanvas = true;
        gCanvasH = canvas.height();
    }
    #endif
#elif ESK8OS_DISPLAY_OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    gOledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    if (gOledReady) {
        oled.invertDisplay(gOledInvert);
        applyOledContrast();
        drawOledBootSplash(12, "DISPLAY");
    }
    gCanvasH = 32;
#else
    gCanvasH = 0;
#endif
}

void showBootSplash(uint8_t progressPct, const char* status) {
#if ESK8OS_DISPLAY_OLED
    if (gOledReady) drawOledBootSplash(progressPct, status);
#else
    (void)progressPct;
    (void)status;
#endif
}

void applyOledInvert() {
#if ESK8OS_DISPLAY_OLED
    if (gOledReady) oled.invertDisplay(gOledInvert);
#endif
}

void applyDisplayBrightness() {
#if ESK8OS_DISPLAY_OLED
    applyOledContrast();
#endif
}

void renderTftScreensaver() {
#if ESK8OS_DISPLAY_TFT
    // ~30 fps cap. The caller runs at loop rate (hundreds of Hz); repainting +
    // full-canvas blitting every iteration burned max CPU/bus power in exactly
    // the idle state the saver exists to save. The bounce physics below steps
    // on its own >=30 ms clock, so capping the paint changes nothing visually.
    static unsigned long lastPaint = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastPaint < 33 && !gRedrawAll) return;
    lastPaint = nowMs;

    static const uint16_t colors[] = {
        0x07FF, // cyan
        0xF81F, // magenta
        0xFFE0, // yellow
        0x07E0, // green
        0xFD20, // orange
        0xFFFF, // white
    };
    static const int NCOL = sizeof(colors) / sizeof(colors[0]);
    int w = gUseCanvas ? canvas.width() : tft.width();
    int h = gUseCanvas ? canvas.height() : tft.height();
    const int logoW = 110;
    const int logoH = 42;
    const float maxX = max(1, w - logoW - 1);
    const float maxY = max(1, h - logoH - 1);

    // Integrate the bounce on a steady ~33 fps wall-clock cadence instead of
    // deriving x/y straight from millis() every loop. The main loop hitches on
    // each telemetry poll (~100 ms), so a per-loop position made the logo tick
    // unevenly ("jitter at certain spots"); a fixed time-step keeps it smooth no
    // matter how fast or irregularly the loop spins. Colour flips on each wall
    // hit, DVD-style.
    static bool  init = false;
    static float fx = 0, fy = 0, vx = 48, vy = 33;   // position px, velocity px/s
    static int   colIdx = 0;
    static unsigned long lastStep = 0;
    unsigned long now = millis();
    if (!init) { fx = (w - logoW) * 0.5f; fy = (h - logoH) * 0.5f; lastStep = now; init = true; }

    unsigned long dtMs = now - lastStep;
    if (dtMs >= 30) {
        float dt = dtMs / 1000.0f;
        if (dt > 0.10f) dt = 0.10f;                  // clamp so a stall can't fling it
        fx += vx * dt;
        fy += vy * dt;
        if (fx <= 0)    { fx = 0;    vx = -vx; colIdx = (colIdx + 1) % NCOL; }
        if (fx >= maxX) { fx = maxX; vx = -vx; colIdx = (colIdx + 1) % NCOL; }
        if (fy <= 0)    { fy = 0;    vy = -vy; colIdx = (colIdx + 1) % NCOL; }
        if (fy >= maxY) { fy = maxY; vy = -vy; colIdx = (colIdx + 1) % NCOL; }
        lastStep = now;
    }
    int x = (int)(fx + 0.5f);
    int y = (int)(fy + 0.5f);
    uint16_t c = colors[colIdx];

    GFX->fillScreen(COL_BG);
    GFX->drawRect(x, y, logoW, logoH, c);
    GFX->drawRect(x + 2, y + 2, logoW - 4, logoH - 4, c);
    GFX->fillRect(x + 5, y + 5, logoW - 10, logoH - 10, c);
    GFX->setTextDatum(MC_DATUM);
    GFX->setFont(&fonts::Font4);
    GFX->setTextColor(COL_BG);
    GFX->drawString("EVEE", x + logoW / 2, y + logoH / 2);
    GFX->setFont(&fonts::Font0);
    // Only show the live stat corners when the VESC is actually linked; with no
    // ESC connected they'd read a fake "0%  0.0V", so leave just the logo.
    if (telemetryLive) {
        GFX->setTextDatum(TL_DATUM);
        GFX->setTextColor(COL_DIM);
        GFX->drawString(String(currentBatteryPercent) + "%  " + String(currentVoltage, 1) + "V", 6, 6);
        String range = String(useMph ? remainingRangeKm * 0.621371f : remainingRangeKm, 0) + (useMph ? " mi" : " km");
        GFX->drawString(range, w - GFX->textWidth(range) - 6, h - 14);
    }
    // Pair code (BLE MAC tail) — visible while idle so the rider can match the
    // board to the right entry in the app's scan list before connecting.
    if (gPairCode[0]) {
        GFX->setTextDatum(BC_DATUM);
        GFX->setTextColor(COL_DIM);
        GFX->drawString(String("PAIR ") + gPairCode, w / 2, h - 4);
    }
    markDirty(0, gCanvasH);
#endif
}

void previewOledScreensaver(unsigned long durationMs) {
#if ESK8OS_DISPLAY_OLED
    gOledSaverPreviewUntil = millis() + durationMs;
    gRedrawAll = true;
#else
    (void)durationMs;
#endif
}

void markDirty(int y, int h) {
#if ESK8OS_DISPLAY_TFT
    if (!gUseCanvas) return;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h;
    if (y1 > gCanvasH) y1 = gCanvasH;
    if (y1 <= y0) return;
    for (int i = 0; i < gDirtyN; i++) {
        if (y0 <= gDirty[i].y1 && y1 >= gDirty[i].y0) {
            if (y0 < gDirty[i].y0) gDirty[i].y0 = y0;
            if (y1 > gDirty[i].y1) gDirty[i].y1 = y1;
            return;
        }
    }
    if (gDirtyN < MAX_DIRTY) { gDirty[gDirtyN].y0 = y0; gDirty[gDirtyN].y1 = y1; gDirtyN++; }
    else { gDirty[0].y0 = 0; gDirty[0].y1 = gCanvasH; gDirtyN = 1; }
#else
    (void)y;
    (void)h;
#endif
}

void pushCanvas() {
#if ESK8OS_DISPLAY_TFT
    if (!gUseCanvas || gDirtyN == 0) return;
    unsigned long t0 = micros();
    for (int i = 0; i < gDirtyN; i++) {
        int y0 = gDirty[i].y0;
        int h  = gDirty[i].y1 - y0;
        tft.setClipRect(0, y0, tft.width(), h);
        canvas.pushSprite(0, 0);
    }
    tft.clearClipRect();
    gLastPushUs = micros() - t0;
    gDirtyN = 0;
#endif
}

void pushCanvasFull() {
    markDirty(0, gCanvasH);
    pushCanvas();
}

void renderMiniFrame(int alertState) {
#if ESK8OS_DISPLAY_OLED
    if (!gOledReady) return;
    static unsigned long lastMs = 0;
    if (millis() - lastMs < 180 && !gRedrawAll) return;
    lastMs = millis();

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);

    static unsigned long idleSinceMs = 0;
    const bool noRideActivity = currentSpeedKmh < 0.5f && fabs(currentAmps) < 1.0f;
    bool idle = oledTelemetryLive() && !gDemoMode && noRideActivity &&
                rangeAlertState == 0 && alertState == 0;
    if (idle) {
        if (idleSinceMs == 0) idleSinceMs = millis();
    } else {
        idleSinceMs = 0;
    }
    bool previewSaver = (long)(gOledSaverPreviewUntil - millis()) > 0;
    bool screensaverActive = previewSaver || (idleSinceMs != 0 && millis() - idleSinceMs > 30000UL);
    setOledDimmed(oledShouldDim(screensaverActive));

    if (!oledTelemetryLive() && !gDemoMode) {
        oled.setTextSize(2);
        oled.println("NO VESC");
        oled.setTextSize(1);
        oled.setCursor(0, 18);
        oled.print("demo off  BLE ready");
    } else if (alertState == 1) {
        oled.println("VESC FAULT");
        oled.print("code ");
        oled.print(vescFault);
    } else if (alertState == 2) {
        oled.println("VESC LINK LOST");
    } else if (screensaverActive) {
        drawOledScreensaver();
    } else {
        drawOledLiveHud(alertState);
    }

    oled.display();
#else
    (void)alertState;
#endif
}

}
}

// These are global wrappers to satisfy esk8os.h definitions for now, since 
// the rest of the app calls them. They redirect to the module.
void markDirty(int y, int h) { Esk8OS::UiRenderer::markDirty(y, h); }
void pushCanvas() { Esk8OS::UiRenderer::pushCanvas(); }
void pushCanvasFull() { Esk8OS::UiRenderer::pushCanvasFull(); }
