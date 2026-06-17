#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Fonts/GFXFF/gfxfont.h>
#include "BebasNeue18.h"
#include "BebasNeue24.h"
#include "BebasNeue34.h"
#include "BebasNeue40.h"
#include "BebasNeue80.h"
#include <WiFi.h>
#ifndef WOKWI_SIMULATION
#include <VescUart.h>
#endif

// ==========================================
// USER CONFIG  — edit these to personalize
// ==========================================
const char* PRODUCT_NAME = "ESK8 OS";   // brand shown on splash + top bar
const char* RIDER_NAME   = "JOE";        // your name
const char* FW_VERSION   = "v3.2";       // firmware version string
const bool  USE_MPH_DEFAULT = true;      // true = MPH, false = KM/H at boot

// ==========================================
// PHYSICAL HARDWARE SPECS
// ==========================================
const int   BATTERY_CELLS_COUNT = 10;
const float BATTERY_MAX_V = BATTERY_CELLS_COUNT * 4.2;
const float BATTERY_MIN_V = BATTERY_CELLS_COUNT * 3.2;
const float MOTOR_POLE_PAIRS = 7.0;
const float GEARING_RATIO = 16.0 / 72.0;
const float WHEEL_CIRCUMFERENCE_M = 0.203 * PI;

// Degree symbol in the TFT_eSPI GLCD font (Font 1 / CP437): 0xF8.
const char DEG = (char)0xF8;

// ==========================================
// VESC UART
// ==========================================
#define VESC_RX_PIN 18
#define VESC_TX_PIN 17

#ifndef WOKWI_SIMULATION
VescUart UART;
#endif

// ==========================================
// BUTTONS
// ==========================================
#define BTN_LEFT  0
#define BTN_RIGHT 14
bool lastLeftBtn = HIGH, lastRightBtn = HIGH;
unsigned long lastLeftPress = 0, lastRightPress = 0;

// ==========================================
// DISPLAY
// ==========================================
TFT_eSPI tft = TFT_eSPI();

// ==========================================
// COLORS (NZXT CAM Palette)
// ==========================================
uint16_t COL_BG;        // #1a1a1a
uint16_t COL_BORDER;    // #444444
uint16_t COL_DIM;       // #888888
uint16_t COL_LABEL;     // #aaaaaa
uint16_t COL_WHITE;     // #ffffff
uint16_t COL_GREEN;     // #00c864
uint16_t COL_RED;       // #ff3333
uint16_t COL_BLUE;      // #4488ff

// ==========================================
// TELEMETRY STATE
// ==========================================
float currentSpeedKmh = 0.0;
float currentSpeedMph = 0.0;
float currentVoltage = 39.2;
int   currentBatteryPercent = 78;
float currentMotorTemp = 38.0;
float currentBatteryTemp = 32.0;
float currentEscTemp = 35.0;
float currentAmps = 14.2;
float currentWattHours = 392.0;
float tripDistanceKm = 12.8;
float totalDistanceKm = 615.0;
float estimatedRangeKm = 18.5;
float remainingRangeKm = 21.3;
float avgWhPerKm = 18.2;

// Per-component health indicators shown next to each temperature (the green
// "(90%)" in the design). Placeholder values until wired to a real metric.
int   motorHealthPct = 90;
int   batteryHealthPct = 85;
int   escHealthPct = 75;

bool useMph = USE_MPH_DEFAULT;
unsigned long lastDataPoll = 0;

// ==========================================
// LAYOUT CONSTANTS (Designed for 170px width)
// ==========================================
int X0 = 0; // X offset to center 170px UI on wider Wokwi screen
const int UI_W = 170;

// Helpers
void drawHLine(int x, int y, int w, uint16_t color) {
    tft.drawFastHLine(x, y, w, color);
}

// Bordered card with a centered header label and an underline.
void drawCard(int x, int y, int w, int h, const char* title) {
    tft.drawRect(X0 + x, y, w, h, COL_BORDER);
    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_LABEL);
    tft.drawString(title, X0 + x + w / 2, y + 3);
    drawHLine(X0 + x + 4, y + 14, w - 8, COL_BORDER);
}

// ==========================================
// BOOT SPLASH
// ==========================================
void drawBootSplash() {
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);

    // Version (top-right, faint)
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COL_BORDER);
    tft.drawString(FW_VERSION, X0 + UI_W - 6, 6);

    // Wordmark: big "ESK8" with a small superscript "OS" -> "ESK8 OS"
    const int topY = 86, gap = 3;
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&BebasNeue80pt7b);
    int wMain = tft.textWidth("ESK8");
    tft.setFreeFont(&BebasNeue34pt7b);
    int wOs = tft.textWidth("OS");
    int startX = X0 + (UI_W - (wMain + gap + wOs)) / 2;

    tft.setFreeFont(&BebasNeue80pt7b);
    tft.setTextColor(COL_WHITE);
    tft.drawString("ESK8", startX, topY);
    tft.setFreeFont(&BebasNeue34pt7b);
    tft.setTextColor(COL_BLUE);                 // superscript, top-aligned
    tft.drawString("OS", startX + wMain + gap, topY);

    // Tagline
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM);
    tft.drawString("RIDE DASHBOARD", X0 + UI_W / 2, 168);

    // Rider
    tft.setTextColor(COL_LABEL);
    tft.drawString(String("RIDER: ") + RIDER_NAME, X0 + UI_W / 2, 300);

    // Status line (while the bar fills)
    tft.setTextColor(COL_DIM);
    tft.drawString("CONNECTING TO VESC...", X0 + UI_W / 2, 272);

    // Progress bar shell
    int bw = 120, bh = 8, by = 250;
    int bx = X0 + (UI_W - bw) / 2;
    tft.drawRect(bx, by, bw, bh, COL_BORDER);

    // Animate the fill (~1.6s total)
    for (int pct = 0; pct <= 100; pct += 4) {
        int fillw = (bw - 2) * pct / 100;
        tft.fillRect(bx + 1, by + 1, fillw, bh - 2, COL_GREEN);
        delay(16);
    }

    // Report the real link state
    #ifndef WOKWI_SIMULATION
    bool linked = UART.getVescValues();
    #else
    bool linked = false;
    #endif

    tft.fillRect(X0, 266, UI_W, 12, COL_BG);
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    #ifdef WOKWI_SIMULATION
    tft.setTextColor(COL_BLUE);
    tft.drawString("SIMULATION MODE", X0 + UI_W / 2, 272);
    #else
    if (linked) {
        tft.setTextColor(COL_GREEN);
        tft.drawString("VESC LINKED", X0 + UI_W / 2, 272);
    } else {
        tft.setTextColor(COL_RED);
        tft.drawString("VESC OFFLINE", X0 + UI_W / 2, 272);
    }
    #endif
    delay(700);
}

// ==========================================
// DRAW THE COMPLETE STATIC FRAME
// ==========================================
void drawStaticFrame() {
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);

    // ── TOP STATUS BAR (y=0..16) ──
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM);
    tft.drawString(PRODUCT_NAME, X0 + 4, 4);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_DIM);
    tft.drawString(String("RIDER: ") + RIDER_NAME, X0 + 85, 4);

    drawHLine(X0, 16, UI_W, COL_BORDER);

    // ── PRO MODE PILL (y=122..144) ──
    int pmW = 84, pmH = 22, pmY = 122;
    int pmX = X0 + (UI_W - pmW) / 2;
    tft.fillRect(pmX, pmY, pmW, pmH, COL_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_BG);
    tft.drawString("PRO MODE", X0 + 85, pmY + pmH / 2);

    // ── TEMPS CARD (y=150..212) ──
    drawCard(4, 150, 162, 62, "TEMPS");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM);
    tft.drawString("MOTOR",   X0 + 12, 166);
    tft.drawString("BATTERY", X0 + 12, 180);
    tft.drawString("ESC",     X0 + 12, 194);

    // ── RANGE CARD (y=216..278) ──
    drawCard(4, 216, 162, 62, "RANGE");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM);
    tft.drawString("ESTIMATED",  X0 + 12, 232);
    tft.drawString("REMAINING",  X0 + 12, 246);
    tft.drawString(useMph ? "AVG. WH/MI" : "AVG. WH/KM", X0 + 12, 260);

    // ── BATTERY CELLS ROW (y=282..293) ──
    int cellW = 13, cellH = 11, cellGap = 2;
    int cellsTotalW = (BATTERY_CELLS_COUNT * cellW) + ((BATTERY_CELLS_COUNT - 1) * cellGap);
    int cellStartX = X0 + (UI_W - cellsTotalW) / 2;
    for (int i = 0; i < BATTERY_CELLS_COUNT; i++) {
        tft.drawRect(cellStartX + i * (cellW + cellGap), 282, cellW, cellH, COL_BORDER);
    }

    // ── BOTTOM STATUS BAR (y=298..318) ──
    drawHLine(X0, 298, UI_W, COL_BORDER);
}

// ==========================================
// UPDATE: Clock / ride timer (top-right)
// ==========================================
void updateClock() {
    static int lastShown = -1;
    unsigned long s = millis() / 1000;
    int mins = (s / 60) % 100;
    int secs = s % 60;
    int key = mins * 100 + secs;
    if (key == lastShown) return;
    lastShown = key;

    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    tft.fillRect(X0 + 120, 4, 46, 9, COL_BG);
    tft.setTextFont(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COL_WHITE);
    tft.drawString(buf, X0 + 166, 4);
}

// ==========================================
// UPDATE: Speed Display
// ==========================================
void updateSpeed() {
    static int lastSpeedInt = -1;
    static bool lastUseMph = !useMph;

    int spdInt = (int)(useMph ? currentSpeedMph : currentSpeedKmh);

    if (spdInt != lastSpeedInt || useMph != lastUseMph) {
        // Clear the speed zone (y=20..118)
        tft.fillRect(X0, 20, UI_W, 98, COL_BG);

        // Value
        tft.setTextColor(COL_WHITE);
        tft.setFreeFont(&BebasNeue80pt7b);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(String(spdInt), X0 + 80, 70);

        // Unit (top-right of the speed zone)
        tft.setFreeFont(&BebasNeue18pt7b);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(COL_LABEL);
        tft.drawString(useMph ? "MPH" : "KM/H", X0 + 164, 24);

        lastSpeedInt = spdInt;
        lastUseMph = useMph;
    }
}

// One temps row: dim label is static; this redraws "<temp>C (pct%)".
void drawTempRow(int y, float temp, int pct) {
    tft.setTextFont(1);
    String pstr = String("(") + pct + "%)";
    String tstr = String((int)round(temp)) + DEG + "C";

    int pw = tft.textWidth(pstr);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COL_GREEN);
    tft.drawString(pstr, X0 + 158, y);

    tft.setTextColor(COL_WHITE);
    tft.drawString(tstr, X0 + 158 - pw - 4, y);
}

// ==========================================
// UPDATE: Temps Card
// ==========================================
void updateTemps() {
    static float lastMotor = -999, lastBat = -999, lastEsc = -999;

    if (abs(currentMotorTemp - lastMotor) > 0.3 ||
        abs(currentBatteryTemp - lastBat) > 0.3 ||
        abs(currentEscTemp - lastEsc) > 0.3) {

        // Clear the values column for all three rows
        tft.fillRect(X0 + 90, 166, 72, 42, COL_BG);

        drawTempRow(166, currentMotorTemp,   motorHealthPct);
        drawTempRow(180, currentBatteryTemp, batteryHealthPct);
        drawTempRow(194, currentEscTemp,     escHealthPct);

        lastMotor = currentMotorTemp; lastBat = currentBatteryTemp; lastEsc = currentEscTemp;
    }
}

// ==========================================
// UPDATE: Range Card
// ==========================================
void updateRange() {
    static float lastEst = -999, lastRem = -999, lastWh = -999;
    static bool lastUseMph = !useMph;

    if (abs(estimatedRangeKm - lastEst) > 0.1 ||
        abs(remainingRangeKm - lastRem) > 0.1 ||
        abs(avgWhPerKm - lastWh) > 0.1 ||
        useMph != lastUseMph) {

        // Clear the values column for all three rows
        tft.fillRect(X0 + 80, 232, 82, 42, COL_BG);

        tft.setTextFont(1);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(COL_WHITE);

        String du = useMph ? "mi" : "km";
        float cv = useMph ? 0.621371 : 1.0;        // km -> mi for distances
        // Energy-per-distance scales inversely: wh/km -> wh/mi multiplies by 1.609.
        float avgWh = useMph ? avgWhPerKm / 0.621371 : avgWhPerKm;

        tft.drawString(String(estimatedRangeKm * cv, 1) + " " + du, X0 + 158, 232);
        tft.drawString(String(remainingRangeKm * cv, 1) + " " + du, X0 + 158, 246);
        tft.drawString(String(avgWh, 1) + " wh/" + du,              X0 + 158, 260);

        lastEst = estimatedRangeKm; lastRem = remainingRangeKm; lastWh = avgWhPerKm;
        lastUseMph = useMph;
    }
}

// ==========================================
// UPDATE: Battery Cells
// ==========================================
void updateBatteryCells() {
    static int lastPct = -1;

    if (currentBatteryPercent != lastPct) {
        int filled = (currentBatteryPercent * BATTERY_CELLS_COUNT + 50) / 100;
        int cellW = 13, cellH = 11, cellGap = 2;
        int cellsTotalW = (BATTERY_CELLS_COUNT * cellW) + ((BATTERY_CELLS_COUNT - 1) * cellGap);
        int cellStartX = X0 + (UI_W - cellsTotalW) / 2;

        for (int i = 0; i < BATTERY_CELLS_COUNT; i++) {
            int cx = cellStartX + i * (cellW + cellGap);
            tft.fillRect(cx + 1, 283, cellW - 2, cellH - 2, COL_BG); // Clear
            if (i < filled) {
                uint16_t cc = (filled <= 2) ? COL_RED : COL_GREEN;
                tft.fillRect(cx + 1, 283, cellW - 2, cellH - 2, cc);
            }
        }
        lastPct = currentBatteryPercent;
    }
}

// ==========================================
// UPDATE: Bottom Bar
// ==========================================
void updateBottomBar() {
    static int lastPct = -1;
    static float lastV = -1;
    static bool lastUseMph = !useMph;

    if (currentBatteryPercent != lastPct || abs(currentVoltage - lastV) > 0.1 || useMph != lastUseMph) {
        tft.fillRect(X0, 300, UI_W, 18, COL_BG);
        tft.setTextFont(1);
        tft.setTextColor(COL_DIM);
        tft.setTextDatum(MC_DATUM);
        float odo = useMph ? totalDistanceKm * 0.621371 : totalDistanceKm;
        String bottom = String(currentBatteryPercent) + "%  |  " +
                        String(currentVoltage, 1) + "V  |  " +
                        String(odo, 0) + (useMph ? "mi" : "km");
        tft.drawString(bottom, X0 + UI_W / 2, 306);

        lastPct = currentBatteryPercent;
        lastV = currentVoltage;
        lastUseMph = useMph;
    }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);

    #ifndef WOKWI_SIMULATION
    // Power on the display rail + backlight (critical for T-Display-S3)
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    #endif

    tft.init();
    tft.setRotation(0);

    // Center UI if screen is wider than 170 (e.g. Wokwi's 240px ILI9341)
    X0 = (tft.width() - UI_W) / 2;
    if (X0 < 0) X0 = 0;

    COL_BG      = tft.color565(26, 26, 26);
    COL_BORDER  = tft.color565(68, 68, 68);
    COL_DIM     = tft.color565(136, 136, 136);
    COL_LABEL   = tft.color565(170, 170, 170);
    COL_WHITE   = TFT_WHITE;
    COL_GREEN   = tft.color565(0, 200, 100);
    COL_RED     = tft.color565(255, 51, 51);
    COL_BLUE    = tft.color565(68, 136, 255);

    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    #ifndef WOKWI_SIMULATION
    Serial1.begin(115200, SERIAL_8N1, VESC_RX_PIN, VESC_TX_PIN);
    UART.setSerialPort(&Serial1);
    #endif

    drawBootSplash();
    drawStaticFrame();
}

// ==========================================
// LOOP & VESC
// ==========================================
void pollVescData() {
    #ifndef WOKWI_SIMULATION
    if (UART.getVescValues()) {
        float wheelRPM = (UART.data.rpm / MOTOR_POLE_PAIRS) * GEARING_RATIO;
        currentSpeedKmh = (wheelRPM * WHEEL_CIRCUMFERENCE_M * 60.0) / 1000.0;
        currentSpeedMph = currentSpeedKmh * 0.621371;

        currentVoltage = UART.data.inpVoltage;
        float pct = ((currentVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) * 100.0;
        currentBatteryPercent = constrain((int)pct, 0, 100);

        currentMotorTemp = UART.data.tempMotor;
        currentEscTemp = UART.data.tempMosfet;
        currentAmps = UART.data.avgInputCurrent;
        currentWattHours = UART.data.wattHours;
    }
    #else
    static bool accel = true;
    if (accel) currentSpeedKmh += 0.5; else currentSpeedKmh -= 0.5;
    if (currentSpeedKmh >= 45.0) accel = false;
    if (currentSpeedKmh <= 0.0) accel = true;
    currentSpeedMph = currentSpeedKmh * 0.621371;
    #endif
}

void checkButtons() {
    bool left = digitalRead(BTN_LEFT);
    bool right = digitalRead(BTN_RIGHT);

    if (left == LOW && lastLeftBtn == HIGH && millis() - lastLeftPress > 200) {
        lastLeftPress = millis();
    }
    lastLeftBtn = left;

    if (right == LOW && lastRightBtn == HIGH && millis() - lastRightPress > 200) {
        useMph = !useMph;
        drawStaticFrame(); // Force redraw
        lastRightPress = millis();
    }
    lastRightBtn = right;
}

void loop() {
    unsigned long now = millis();
    checkButtons();

    if (now - lastDataPoll > 100) {
        pollVescData();
        lastDataPoll = now;
    }

    updateClock();
    updateSpeed();
    updateTemps();
    updateRange();
    updateBatteryCells();
    updateBottomBar();

    delay(10);
}
