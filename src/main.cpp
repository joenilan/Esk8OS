#include <Arduino.h>
#include "LGFX_Config.h"   // LovyanGFX device + sprite (DMA on the S3 parallel bus)
#include <WiFi.h>
#include <Preferences.h>
#ifndef WOKWI_SIMULATION
#include <VescUart.h>
#endif
#include "BebasNeue80.h"

// ==========================================
// USER CONFIG  — edit these to personalize
// ==========================================
const char* PRODUCT_NAME = "ESK8OS";     // brand shown on splash + top bar
const char* RIDER_NAME   = "JOE";        // your name
const char* FW_VERSION   = "v3.2";       // firmware version string
const bool  USE_MPH_DEFAULT = true;      // true = MPH, false = KM/H at boot
const int   STORAGE_SCHEMA_VERSION = 2;  // bump once to clear old demo trip/odo

// DEMO_MODE feeds simulated telemetry even on the physical board, so you can
// bench-test the UI on the device without the FSESC connected. Set false once
// it's wired to the VESC to read real data.
// TEMPORARY: re-enabled while the Flipsky Dual FSESC 6.7 Plus is out for
// replacement — flip back to false once the new ESC is wired in.
const bool  DEMO_MODE = true;

// True when telemetry is simulated — the Wokwi build, or DEMO_MODE on hardware.
// Gates demo-only conveniences (fast range learn-in, the recharge gesture).
#if defined(WOKWI_SIMULATION)
const bool  DEMO_DATA = true;
#else
const bool  DEMO_DATA = DEMO_MODE;
#endif

// Battery warning thresholds over the configured usable pack window. The display
// treats 0% as "stop riding now", not absolute empty/dead lithium cells.
const int   BATT_WARN_PCT = 50;          // below -> yellow
const int   BATT_LOW_PCT  = 30;          // below -> orange
const int   BATT_CRIT_PCT = 15;          // below -> red (stop / charge now)

// Battery pack capacity used by the display's range estimate. The cells are
// nominally 2800 mAh in 10S6P (16.8 Ah), but this pack is configured with a
// conservative effective capacity of 16.5 Ah to better match real cells/VESC.
const int   BATTERY_PARALLEL_COUNT = 6;
const int   BATTERY_CELL_CAPACITY_MAH = 2800;
const float BATTERY_EFFECTIVE_CAPACITY_AH = 16.5;
const float BATTERY_NOMINAL_CELL_V = 3.7;
const float BATTERY_FULL_CELL_V = 4.20;      // fully charged cell voltage
const float BATTERY_STOP_CELL_V = 3.30;      // dashboard 0%; align with VESC cutoff/end
const float RANGE_DEFAULT_WH_PER_MILE = 22.0; // used until enough real ride data is learned
const float RANGE_LEARN_MIN_DISTANCE_KM = 1.6; // wait ~1 mi before trusting learned Wh/mi
const float RANGE_LEARN_MIN_WH = 20.0;         // avoids nonsense from unloaded bench spins

// Over-temp alert thresholds (deg C). Past these the temp turns red and a
// banner fires on any page.
const float MOTOR_TEMP_LIMIT = 80.0;
const float ESC_TEMP_LIMIT   = 80.0;
const unsigned long VESC_LINK_TIMEOUT_MS = 3000;

// ==========================================
// PHYSICAL HARDWARE SPECS
// ==========================================
const int   BATTERY_CELLS_COUNT = 10;
const float BATTERY_MAX_V = BATTERY_CELLS_COUNT * BATTERY_FULL_CELL_V;
const float BATTERY_MIN_V = BATTERY_CELLS_COUNT * BATTERY_STOP_CELL_V;
// Wheel/gearing profiles for the DISPLAY's own speed + distance math (these do
// NOT write to the FSESC — they only keep the gauge accurate; use bridge mode +
// VESC Tool to change the VESC's own config). Selected profile persists in NVS.
struct WheelProfile {
    const char* name;
    float wheelDiameterM;
    int   motorPulley;
    int   wheelPulley;
    float polePairs;
};
WheelProfile wheelProfiles[] = {
    { "8IN PNEU", 0.203f, 16, 72, 7.0f },
    { "100MM",    0.100f, 16, 48, 7.0f },
};
const int WHEEL_PROFILE_COUNT = sizeof(wheelProfiles) / sizeof(wheelProfiles[0]);
int activeWheelProfile = 0;   // loaded from NVS in setup()

float profileGearRatio()    { return (float)wheelProfiles[activeWheelProfile].motorPulley /
                                     (float)wheelProfiles[activeWheelProfile].wheelPulley; }
float profileCircumfM()     { return wheelProfiles[activeWheelProfile].wheelDiameterM * PI; }
float profilePolePairs()    { return wheelProfiles[activeWheelProfile].polePairs; }

// Degree symbol in the GLCD font (LovyanGFX Font0, CP437): 0xF8.
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
LGFX tft;

// Off-screen frame buffer. All widgets render into `canvas` and the finished
// frame is blitted to the panel with a single DMA pushSprite() — no flicker, and
// the LCD_CAM DMA path makes a full-frame push ~5 ms instead of TFT_eSPI's
// ~38 ms. `GFX` is the active draw target: the canvas when allocated, otherwise
// `tft` directly (fallback if the SRAM sprite can't be created).
LGFX_Sprite      canvas(&tft);
lgfx::LovyanGFX* GFX = &tft;
bool gUseCanvas = false;
uint16_t gFps = 0;            // frames pushed in the last second (SYSTEM page)
unsigned long gLastPushUs = 0; // duration of the last blit (SYSTEM page diag)

// Brief on-screen confirmation banner (e.g. after a trip reset / recharge).
unsigned long gToastUntil = 0;
char gToastMsg[20] = "";
// NOTE: TFT_eSPI has no DMA path for 8-bit parallel on the ESP32-S3, so the
// canvas is blitted with a blocking pushSprite — fast enough that it only
// happens on changed frames (see PERFORMANCE.md).

// Where the canvas actually landed (shown on the SYSTEM page). Internal SRAM is
// far faster to blit than PSRAM, which bottlenecks the per-pixel parallel write.
bool gCanvasPsram = false;

// Non-volatile storage for the persisted odometer + trip
Preferences prefs;

// Set true to force a full repaint of every widget on the next loop (used
// after drawStaticFrame() wipes the screen, e.g. on unit toggle / trip reset).
bool gRedrawAll = true;

// VESC Tool WiFi-TCP bridge (active only in MODE_VESC_BRIDGE). Forwards raw
// bytes between a TCP client (desktop VESC Tool > TCP connection) and Serial1.
WiFiServer bridgeServer(65102);
WiFiClient bridgeClient;
const char* BRIDGE_SSID = "ESK8-BRIDGE";
const char* BRIDGE_PASS = "esk8bridge";   // must be >= 8 chars
String bridgeStatus = "WAITING";
unsigned long bridgeRxBytes = 0;   // VESC Tool -> ESC, bytes this session
unsigned long bridgeTxBytes = 0;   // ESC -> VESC Tool, bytes this session

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
uint16_t COL_YELLOW;    // #ffcd00
uint16_t COL_ORANGE;    // #ff8000

// Battery zone color: green -> yellow -> orange -> red as charge drops.
uint16_t battColor(int pct) {
    if (pct >= BATT_WARN_PCT) return COL_GREEN;
    if (pct >= BATT_LOW_PCT)  return COL_YELLOW;
    if (pct >= BATT_CRIT_PCT) return COL_ORANGE;
    return COL_RED;
}

// Power readout zones: white -> yellow -> orange -> red as wattage climbs.
uint16_t wattColor(int w) {
    if (w >= 3000) return COL_RED;
    if (w >= 2000) return COL_ORANGE;
    if (w >= 1000) return COL_YELLOW;
    return COL_WHITE;
}

// Duty-cycle zones: the top end (near 100%) is where you run out of headroom.
uint16_t dutyColor(int d) {
    if (d >= 95) return COL_RED;
    if (d >= 85) return COL_ORANGE;
    if (d >= 70) return COL_YELLOW;
    return COL_WHITE;
}

// ==========================================
// TELEMETRY STATE
// ==========================================
float currentSpeedKmh = 0.0;
float currentSpeedMph = 0.0;
float currentVoltage = BATTERY_MAX_V;
int   currentBatteryPercent = 100;
float currentMotorTemp = 0.0;
float currentBatteryTemp = 0.0;
float currentEscTemp = 0.0;
float currentAmps = 0.0;        // battery / input amps
float currentMotorAmps = 0.0;   // motor phase amps
float currentDuty = 0.0;        // duty cycle %
float currentWatts = 0.0;
float peakWatts = 0.0;          // short peak-hold so the spike is readable
float currentWattHours = 0.0;   // Wh used
float currentWhRegen = 0.0;     // Wh recovered braking
float maxSpeedKmh = 0.0;        // session max
float avgSpeedKmh = 0.0;        // session average
float maxWattsSession = 0.0;            // session peak power (W)
float minVoltageSession = BATTERY_MAX_V; // session lowest pack voltage (sag)
float maxMotorAmpsSession = 0.0;        // session peak motor current (A)

// Page system: 0 dashboard, 1 power/stats, 2 trip, 3 settings, 4 system info
int currentPage = 0;
const int PAGE_COUNT = 5;

// Top-level system mode: ride dashboard vs wireless VESC Tool bridge.
enum SystemMode { MODE_DASHBOARD, MODE_VESC_BRIDGE };
SystemMode systemMode = MODE_DASHBOARD;

// Safety: VESC fault + link tracking
int vescFault = 0;              // mc_fault_code as int (0 = none)
unsigned long lastVescOkMs = 0; // last successful telemetry read
bool vescLinkOk = true;         // false -> telemetry stale (hardware only)

// Session tracking for avg speed
unsigned long rideStartMs = 0;
float sessionTripStartKm = 0.0;
float tripDistanceKm = 0.0;
float totalDistanceKm = 0.0;
float estimatedRangeKm = 0.0;
float remainingRangeKm = 0.0;
float avgWhPerKm = 0.0;
float rideStartVescWh = 0.0;
float rideStartVescWhRegen = 0.0;
bool  rideEnergyBaselineSet = false;
bool  rangeEstimateReady = false;

// Per-component health indicators shown next to each temperature (the green
// "(90%)" in the design). Placeholder values until wired to a real metric.
int   motorHealthPct = 90;
int   batteryHealthPct = 85;
int   escHealthPct = 75;

bool useMph = USE_MPH_DEFAULT;
unsigned long lastDataPoll = 0;

void updateRangeEstimate();

// ==========================================
// LAYOUT CONSTANTS (Designed for 170px width)
// ==========================================
int X0 = 0; // X offset to center 170px UI on wider Wokwi screen
const int UI_W = 170;

// Blit the off-screen canvas to the panel in a single write (flicker-free), and
// track frames/sec for the SYSTEM page. No-op when unbuffered (draws already
// went straight to the panel).
// Dirty-rectangle tracking. A full-frame blit on this 8-bit parallel panel is
// ~38 ms (TFT_eSPI bit-bangs the bus, no DMA), so instead of pushing all 320
// rows every change we push only the changed vertical bands. Bands are full
// width so each is a contiguous block copy (TFT_eSprite fast path).
int gCanvasH = 320;
struct DirtyBand { int16_t y0, y1; };
const int MAX_DIRTY = 10;
DirtyBand gDirty[MAX_DIRTY];
int gDirtyN = 0;

void markDirty(int y, int h) {
    if (!gUseCanvas) return;              // direct mode draws straight to the panel
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h;
    if (y1 > gCanvasH) y1 = gCanvasH;
    if (y1 <= y0) return;
    for (int i = 0; i < gDirtyN; i++) {  // merge into an overlapping band
        if (y0 <= gDirty[i].y1 && y1 >= gDirty[i].y0) {
            if (y0 < gDirty[i].y0) gDirty[i].y0 = y0;
            if (y1 > gDirty[i].y1) gDirty[i].y1 = y1;
            return;
        }
    }
    if (gDirtyN < MAX_DIRTY) { gDirty[gDirtyN].y0 = y0; gDirty[gDirtyN].y1 = y1; gDirtyN++; }
    else { gDirty[0].y0 = 0; gDirty[0].y1 = gCanvasH; gDirtyN = 1; }  // overflow -> full
}

void pushCanvas() {
    if (!gUseCanvas) return;
    // LovyanGFX blits the sprite over the LCD_CAM DMA path (~5 ms full frame),
    // so we just push the whole canvas whenever anything changed — no need for
    // the dirty-band splitting TFT_eSPI required. markDirty() still gates whether
    // a push is needed at all (gDirtyN > 0).
    unsigned long t0 = micros();
    canvas.pushSprite(0, 0);
    gLastPushUs = micros() - t0;
    gDirtyN = 0;

    static unsigned long fpsWindow = 0;
    static uint16_t fpsCount = 0;
    fpsCount++;
    unsigned long now = millis();
    if (now - fpsWindow >= 1000) { gFps = fpsCount; fpsCount = 0; fpsWindow = now; }
}

// Blit the whole frame. Used by the boot splash and bridge screens, which redraw
// full-screen and don't track dirty bands.
void pushCanvasFull() {
    markDirty(0, gCanvasH);
    pushCanvas();
}

// Show a brief centered confirmation banner over the dashboard (~1.2 s).
void showToast(const char* msg) {
    strncpy(gToastMsg, msg, sizeof(gToastMsg) - 1);
    gToastMsg[sizeof(gToastMsg) - 1] = '\0';
    gToastUntil = millis() + 1200;
}

// Helpers
void drawHLine(int x, int y, int w, uint16_t color) {
    GFX->drawFastHLine(x, y, w, color);
}

// Bordered card with a centered header label and an underline.
void drawCard(int x, int y, int w, int h, const char* title) {
    GFX->drawRect(X0 + x, y, w, h, COL_BORDER);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString(title, X0 + x + w / 2, y + 3);
    drawHLine(X0 + x + 4, y + 14, w - 8, COL_BORDER);
}

// ==========================================
// BOOT SPLASH  (rendered into the canvas too, so it is flicker-free)
// ==========================================
void drawBootProgress(int pct, const char* status, uint16_t color) {
    int bw = 120, bh = 8, by = 250;
    int bx = X0 + (UI_W - bw) / 2;
    pct = constrain(pct, 0, 100);

    GFX->fillRect(bx + 1, by + 1, bw - 2, bh - 2, COL_BG);
    GFX->fillRect(bx + 1, by + 1, (bw - 2) * pct / 100, bh - 2, COL_GREEN);

    GFX->fillRect(X0, 266, UI_W, 13, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(color);
    GFX->drawString(status, X0 + UI_W / 2, 272);
    pushCanvasFull();
}

void drawBootSplashFrame() {
    GFX->fillScreen(COL_BG);
    GFX->setFont(&fonts::Font0);

    // Version (top-right, faint)
    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(COL_BORDER);
    GFX->drawString(FW_VERSION, X0 + UI_W - 6, 6);

    // Wordmark: big "ESK8" with a small superscript "OS" -> "ESK8 OS"
    const int topY = 86, gap = 3;
    GFX->setTextDatum(TL_DATUM);
    GFX->setFreeFont(&fonts::FreeSansBold24pt7b);
    int wMain = GFX->textWidth("ESK8");
    GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
    int wOs = GFX->textWidth("OS");
    int startX = X0 + (UI_W - (wMain + gap + wOs)) / 2;

    GFX->setFreeFont(&fonts::FreeSansBold24pt7b);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString("ESK8", startX, topY);
    GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
    GFX->setTextColor(COL_BLUE);                 // superscript, top-aligned
    GFX->drawString("OS", startX + wMain + gap, topY);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("RIDE DASHBOARD", X0 + UI_W / 2, 168);

    // Controls legend (boot-time reference; keeps the dashboard itself uncluttered)
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("L: page     R: units", X0 + UI_W / 2, 196);
    GFX->drawString("hold L: reset trip",   X0 + UI_W / 2, 210);
    GFX->drawString("hold L+R: bridge mode", X0 + UI_W / 2, 224);

    GFX->setTextColor(COL_LABEL);
    GFX->drawString(String("RIDER: ") + RIDER_NAME, X0 + UI_W / 2, 300);

    int bw = 120, bh = 8, by = 250;
    int bx = X0 + (UI_W - bw) / 2;
    GFX->drawRect(bx, by, bw, bh, COL_BORDER);
    pushCanvasFull();
}

void waitForBootReady() {
    drawBootSplashFrame();
    drawBootProgress(20, "DISPLAY READY", COL_DIM);
    delay(150);
    drawBootProgress(40, "STORAGE READY", COL_DIM);
    delay(150);

    #ifdef WOKWI_SIMULATION
    drawBootProgress(100, "SIMULATION MODE", COL_BLUE);
    delay(500);
    #else
    drawBootProgress(60, "UART READY", COL_DIM);
    delay(150);

    if (DEMO_MODE) {
        drawBootProgress(100, "DEMO TELEMETRY", COL_BLUE);
        delay(500);
        return;
    }

    unsigned long lastDraw = 0;
    int pulse = 0;
    while (!UART.getVescValues()) {
        if (millis() - lastDraw > 350) {
            lastDraw = millis();
            int pct = 65 + (pulse % 26);  // pulse between 65% and 90% while waiting
            pulse = (pulse + 5) % 26;
            drawBootProgress(pct, "CONNECTING TO VESC", COL_YELLOW);
        }
        delay(25);
    }

    lastVescOkMs = millis();
    drawBootProgress(100, "VESC LINKED", COL_GREEN);
    delay(500);
    #endif
}

// A static "LABEL ............ (value drawn later)" row in a card.
void drawRowLabel(const char* label, int y) {
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(label, X0 + 12, y);
}

void drawSpeedReadout(int spdInt, bool clearZone) {
    if (clearZone) {
        GFX->fillRect(X0, 17, UI_W, 73, COL_BG);
    }

    // Value: tall Bebas Neue hero number. Digits are 58 px high, so they fill
    // the 73 px speed band without the overflow caused by scaled FreeSans.
    GFX->setTextColor(COL_WHITE);
    GFX->setFont(&BebasNeue80pt7b);
    GFX->setTextDatum(TC_DATUM);
    GFX->drawString(String(spdInt), X0 + 96, 17);

    // Unit: one step smaller than the old 12pt label so double-digit speeds do
    // not collide with KM/H while the label still reads as dashboard chrome.
    GFX->setFont(&fonts::FreeSansBold9pt7b);
    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString(useMph ? "MPH" : "KM/H", X0 + 10, 27);
}

// ── PAGE 0: DASHBOARD static chrome ──
void drawStaticDash() {
    int spdInt = (int)(useMph ? currentSpeedMph : currentSpeedKmh);
    drawSpeedReadout(spdInt, false);

    // VOLTS | WATTS panel (y=86..118) — values drawn by updateStatPanel
    int spW = 162, spH = 32, spY = 86;
    int spX = X0 + (UI_W - spW) / 2;
    GFX->drawRect(spX, spY, spW, spH, COL_BORDER);
    GFX->drawFastVLine(X0 + UI_W / 2, spY + 5, spH - 10, COL_BORDER);

    drawCard(4, 122, 162, 70, "TEMPS");
    drawRowLabel("MOTOR",   140);
    drawRowLabel("BATTERY", 156);
    drawRowLabel("ESC",     172);

    drawCard(4, 198, 162, 70, "RANGE");
    drawRowLabel("ESTIMATED", 216);
    drawRowLabel("REMAINING", 232);
    drawRowLabel(useMph ? "AVG. WH/MI" : "AVG. WH/KM", 248);
}

// ── PAGE 1: POWER / ENERGY / SPEED static chrome ──
void drawStaticPower() {
    drawCard(4, 22, 162, 82, "POWER");
    drawRowLabel("MOTOR",   40);
    drawRowLabel("BATTERY", 56);
    drawRowLabel("DUTY",    72);
    drawRowLabel("PEAK",    88);

    drawCard(4, 108, 162, 48, "ENERGY");
    drawRowLabel("USED",  126);
    drawRowLabel("REGEN", 142);

    drawCard(4, 160, 162, 48, "SPEED");
    drawRowLabel("MAX", 178);
    drawRowLabel("AVG", 194);

    drawCard(4, 212, 162, 48, "SESSION");
    drawRowLabel("MAX PWR",  230);
    drawRowLabel("MIN VOLT", 246);
}

// ── PAGE 2: TRIP static chrome ──
void drawStaticTrip() {
    drawCard(4, 22, 162, 102, "THIS TRIP");
    drawRowLabel("TIME",       40);
    drawRowLabel("DISTANCE",   56);
    drawRowLabel("AVG SPEED",  72);
    drawRowLabel("MAX SPEED",  88);
    drawRowLabel("EFFICIENCY", 104);

    drawCard(4, 132, 162, 40, "ODOMETER");
    drawRowLabel("TOTAL", 150);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(DEMO_DATA ? "hold L: reset + recharge" : "hold L to reset trip",
                    X0 + UI_W / 2, 190);
}

// ── PAGE 3: SETTINGS static chrome ──
void drawStaticSettings() {
    drawCard(4, 22, 162, 82, "WHEEL PROFILE");
    drawRowLabel("PROFILE",  40);
    drawRowLabel("DIAMETER", 56);
    drawRowLabel("GEARING",  72);
    drawRowLabel("POLES",    88);

    drawCard(4, 110, 162, 54, "DISPLAY");
    drawRowLabel("UNITS", 128);
    drawRowLabel("DEMO",  144);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("R: change wheel", X0 + UI_W / 2, 176);
    GFX->drawString("hold L+R: bridge", X0 + UI_W / 2, 190);
}

// Human-readable last-reset cause for the SYSTEM page.
const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWER-ON";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT WDT";
        case ESP_RST_TASK_WDT:  return "TASK WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_EXT:       return "EXTERNAL";
        default:                return "UNKNOWN";
    }
}

// ── PAGE 4: SYSTEM INFO static chrome ──
void drawStaticSystem() {
    drawCard(4, 22, 162, 70, "DEVICE");
    drawRowLabel("CHIP",     40);
    drawRowLabel("CORES",    56);
    drawRowLabel("FIRMWARE", 72);

    drawCard(4, 98, 162, 70, "MEMORY");
    drawRowLabel("HEAP",  116);
    drawRowLabel("MIN",   132);
    drawRowLabel("PSRAM", 148);

    drawCard(4, 174, 162, 70, "RUNTIME");
    drawRowLabel("TEMP",    192);
    drawRowLabel("UPTIME",  208);
    drawRowLabel("REFRESH", 224);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(String("reset: ") + resetReasonStr(), X0 + UI_W / 2, 256);
    GFX->drawString(String("canvas: ") + (gCanvasPsram ? "PSRAM" : "SRAM"), X0 + UI_W / 2, 268);
}

// ==========================================
// DRAW THE COMPLETE STATIC FRAME (page-aware)
// ==========================================
void drawStaticFrame() {
    GFX->fillScreen(COL_BG);
    GFX->setFont(&fonts::Font0);

    // ── TOP STATUS BAR (common, y=0..16) ──
    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(PRODUCT_NAME, X0 + 4, 4);
    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(String("RIDER: ") + RIDER_NAME, X0 + 85, 4);
    drawHLine(X0, 16, UI_W, COL_BORDER);

    if (currentPage == 1)      drawStaticPower();
    else if (currentPage == 2) drawStaticTrip();
    else if (currentPage == 3) drawStaticSettings();
    else if (currentPage == 4) drawStaticSystem();
    else                       drawStaticDash();

    // ── BATTERY CELLS OUTLINE (common, y=276..288) ──
    int cellW = 13, cellH = 12, cellGap = 2;
    int cellsTotalW = (BATTERY_CELLS_COUNT * cellW) + ((BATTERY_CELLS_COUNT - 1) * cellGap);
    int cellStartX = X0 + (UI_W - cellsTotalW) / 2;
    for (int i = 0; i < BATTERY_CELLS_COUNT; i++) {
        GFX->drawRect(cellStartX + i * (cellW + cellGap), 276, cellW, cellH, COL_BORDER);
    }

    // ── PAGE INDICATOR DOTS (in the gap between cells and bottom bar) ──
    int dotGap = 8;
    int dotsW = (PAGE_COUNT - 1) * dotGap;
    int dotX0 = X0 + UI_W / 2 - dotsW / 2;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int dx = dotX0 + i * dotGap;
        if (i == currentPage) GFX->fillCircle(dx, 293, 2, COL_BLUE);
        else                  GFX->drawCircle(dx, 293, 2, COL_BORDER);
    }

    // ── BOTTOM STATUS BAR (common, y=298..318) ──
    drawHLine(X0, 298, UI_W, COL_BORDER);
    gRedrawAll = true;
    markDirty(0, gCanvasH);
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
    if (key == lastShown && !gRedrawAll) return;
    lastShown = key;

    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    GFX->fillRect(X0 + 120, 4, 46, 9, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString(buf, X0 + 166, 4);
    markDirty(0, 16);
}

// ==========================================
// UPDATE: Speed Display
// ==========================================
void updateSpeed() {
    static int lastSpeedInt = -1;
    static bool lastUseMph = !useMph;

    int spdInt = (int)(useMph ? currentSpeedMph : currentSpeedKmh);

    if (spdInt != lastSpeedInt || useMph != lastUseMph || gRedrawAll) {
        drawSpeedReadout(spdInt, true);
        lastSpeedInt = spdInt;
        lastUseMph = useMph;
        markDirty(17, 73);
    }
}

// One stat in the VOLTS|WATTS panel: big value (colored) + dim unit, as a
// centered group around cx.
void drawStat(int cx, String value, const char* unit, uint16_t vcol) {
    const int cy = 103;   // vertical middle of the panel (86 + 32/2), nudged
    const int g = 3;
    GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
    int wn = GFX->textWidth(value);
    GFX->setFreeFont(&fonts::FreeSans12pt7b);
    int wu = GFX->textWidth(unit);
    int gx = cx - (wn + g + wu) / 2;

    GFX->setTextDatum(ML_DATUM);
    GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
    GFX->setTextColor(vcol);
    GFX->drawString(value, gx, cy);
    GFX->setFreeFont(&fonts::FreeSans12pt7b);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(unit, gx + wn + g, cy);
}

// ==========================================
// UPDATE: Volts | Watts panel
// ==========================================
void updateStatPanel() {
    static float lastV = -1;
    static int lastW = -1;
    static int lastPct = -1;

    int bx = X0 + (UI_W - 162) / 2;
    int midx = X0 + UI_W / 2;
    int by = 86, bh = 32;

    int w = (int)round(peakWatts);
    bool vchanged = abs(currentVoltage - lastV) > 0.05 || currentBatteryPercent != lastPct || gRedrawAll;
    bool wchanged = abs(w - lastW) >= 5 || gRedrawAll;

    if (vchanged) {
        GFX->fillRect(bx + 1, by + 1, midx - bx - 2, bh - 2, COL_BG);
        drawStat((bx + midx) / 2, String(currentVoltage, 1), "V", battColor(currentBatteryPercent));
        lastV = currentVoltage; lastPct = currentBatteryPercent;
    }
    if (wchanged) {
        GFX->fillRect(midx + 1, by + 1, (bx + 162) - midx - 2, bh - 2, COL_BG);
        drawStat((midx + bx + 162) / 2, String(w), "W", wattColor(w));
        lastW = w;
    }
    if (vchanged || wchanged) markDirty(86, 32);
}

// One temps row: dim label is static; this redraws "<temp>C (pct%)".
// `hot` turns the temperature red when past its limit.
void drawTempRow(int y, float temp, int pct, bool hot) {
    GFX->setFont(&fonts::Font0);
    String pstr = String("(") + pct + "%)";
    String tstr = String((int)round(temp)) + DEG + "C";

    int pw = GFX->textWidth(pstr);
    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(COL_GREEN);
    GFX->drawString(pstr, X0 + 158, y);

    GFX->setTextColor(hot ? COL_RED : COL_WHITE);
    GFX->drawString(tstr, X0 + 158 - pw - 4, y);

    int barX = X0 + 8;
    int barY = y + 11;
    int barW = 154;
    int fillW = constrain((barW * pct) / 100, 0, barW);
    uint16_t c = hot ? COL_RED : (pct < 70 ? COL_YELLOW : COL_GREEN);
    GFX->drawFastHLine(barX, barY, barW, COL_BORDER);
    if (fillW > 0) {
        GFX->drawFastHLine(barX, barY, fillW, c);
    }
}

// ==========================================
// UPDATE: Temps Card
// ==========================================
void updateTemps() {
    static float lastMotor = -999, lastBat = -999, lastEsc = -999;

    if (abs(currentMotorTemp - lastMotor) > 0.3 ||
        abs(currentBatteryTemp - lastBat) > 0.3 ||
        abs(currentEscTemp - lastEsc) > 0.3 || gRedrawAll) {

        // Clear the values column for all three rows
        GFX->fillRect(X0 + 90, 140, 72, 44, COL_BG);

        drawTempRow(140, currentMotorTemp,   motorHealthPct,  currentMotorTemp > MOTOR_TEMP_LIMIT);
        drawTempRow(156, currentBatteryTemp, batteryHealthPct, false);
        drawTempRow(172, currentEscTemp,     escHealthPct,    currentEscTemp > ESC_TEMP_LIMIT);

        lastMotor = currentMotorTemp; lastBat = currentBatteryTemp; lastEsc = currentEscTemp;
        markDirty(140, 44);
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
        useMph != lastUseMph || gRedrawAll) {

        // Clear the values column for all three rows
        GFX->fillRect(X0 + 80, 216, 82, 44, COL_BG);

        GFX->setFont(&fonts::Font0);
        GFX->setTextDatum(TR_DATUM);
        GFX->setTextColor(COL_WHITE);

        String du = useMph ? "mi" : "km";
        float cv = useMph ? 0.621371 : 1.0;        // km -> mi for distances
        // Energy-per-distance scales inversely: wh/km -> wh/mi multiplies by 1.609.
        float avgWh = useMph ? avgWhPerKm / 0.621371 : avgWhPerKm;

        // REMAINING also shows an estimated time-left (range / current avg speed),
        // which riders tend to read more intuitively than distance.
        String remStr = String(remainingRangeKm * cv, 1) + " " + du;
        if (avgSpeedKmh > 1.0) {
            int mins = (int)(remainingRangeKm / avgSpeedKmh * 60.0);
            if (mins > 0 && mins < 1000) remStr += " " + String(mins) + "m";
        }

        GFX->drawString(String(estimatedRangeKm * cv, 1) + " " + du, X0 + 158, 216);
        GFX->drawString(remStr,                                      X0 + 158, 232);
        GFX->drawString(String(avgWh, 1) + " wh/" + du,              X0 + 158, 248);

        lastEst = estimatedRangeKm; lastRem = remainingRangeKm; lastWh = avgWhPerKm;
        lastUseMph = useMph;
        markDirty(216, 44);
    }
}

// ==========================================
// UPDATE: Battery Cells
// ==========================================
void updateBatteryCells() {
    static int lastPct = -1;

    if (currentBatteryPercent != lastPct || gRedrawAll) {
        int filled = (currentBatteryPercent * BATTERY_CELLS_COUNT + 50) / 100;
        int cellW = 13, cellH = 12, cellGap = 2;
        int cellsTotalW = (BATTERY_CELLS_COUNT * cellW) + ((BATTERY_CELLS_COUNT - 1) * cellGap);
        int cellStartX = X0 + (UI_W - cellsTotalW) / 2;

        for (int i = 0; i < BATTERY_CELLS_COUNT; i++) {
            int cx = cellStartX + i * (cellW + cellGap);
            GFX->fillRect(cx + 1, 277, cellW - 2, cellH - 2, COL_BG); // Clear
            if (i < filled) {
                uint16_t c = battColor(currentBatteryPercent);
                GFX->fillRect(cx + 1, 277, cellW - 2, cellH - 2, c);
            } else {
                GFX->drawFastHLine(cx + 2, 286, cellW - 4, COL_BORDER);
            }
        }
        lastPct = currentBatteryPercent;
        markDirty(276, 13);
    }
}

// ==========================================
// UPDATE: Bottom Bar
// ==========================================
void updateBottomBar() {
    static int lastPct = -1;
    static bool lastUseMph = !useMph;
    static float lastTrip = -1;

    if (currentBatteryPercent != lastPct || useMph != lastUseMph ||
        abs(tripDistanceKm - lastTrip) >= 0.05 || gRedrawAll) {
        GFX->fillRect(X0, 300, UI_W, 18, COL_BG);
        GFX->setFont(&fonts::Font0);

        String du = useMph ? "mi" : "km";
        float odo  = useMph ? totalDistanceKm * 0.621371 : totalDistanceKm;
        float trip = useMph ? tripDistanceKm * 0.621371 : tripDistanceKm;

        String pctStr = String(currentBatteryPercent) + "%";
        String rest = String("  T:") + String(trip, 1) + du +
                      "  O:" + String(odo, 0) + du;
        int wp = GFX->textWidth(pctStr);
        int wr = GFX->textWidth(rest);
        int sx = X0 + (UI_W - (wp + wr)) / 2;

        GFX->setTextDatum(ML_DATUM);
        GFX->setTextColor(battColor(currentBatteryPercent));
        GFX->drawString(pctStr, sx, 308);
        GFX->setTextColor(COL_DIM);
        GFX->drawString(rest, sx + wp, 308);

        lastPct = currentBatteryPercent;
        lastUseMph = useMph;
        lastTrip = tripDistanceKm;
        markDirty(300, 18);
    }
}

// Clear a card's value column and draw a right-aligned value at row y.
void drawVal(int y, String value, uint16_t color) {
    GFX->fillRect(X0 + 78, y - 1, 84, 12, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(color);
    GFX->drawString(value, X0 + 158, y);
}

// ==========================================
// UPDATE: Page 1 (POWER / ENERGY / SPEED)
// ==========================================
void updatePower() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 400) return;
    lastMs = millis();

    String su = useMph ? "mph" : "kmh";
    float cv = useMph ? 0.621371 : 1.0;

    int duty = (int)round(currentDuty);
    int peakW = (int)round(peakWatts);
    drawVal(40,  String(currentMotorAmps, 1) + " A", COL_WHITE);
    drawVal(56,  String(currentAmps, 1) + " A",      COL_WHITE);
    drawVal(72,  String(duty) + " %", dutyColor(duty));
    drawVal(88,  String(peakW) + " W", wattColor(peakW));
    drawVal(126, String((int)round(currentWattHours)) + " Wh", COL_WHITE);
    drawVal(142, String("+") + String((int)round(currentWhRegen)) + " Wh", COL_GREEN);
    drawVal(178, String((int)round(maxSpeedKmh * cv)) + " " + su, COL_WHITE);
    drawVal(194, String((int)round(avgSpeedKmh * cv)) + " " + su, COL_WHITE);

    int maxW = (int)round(maxWattsSession);
    drawVal(230, String(maxW) + " W", wattColor(maxW));
    drawVal(246, String(minVoltageSession, 1) + " V", COL_WHITE);
    markDirty(22, 252);
}

// ==========================================
// UPDATE: Page 2 (TRIP)
// ==========================================
void updateTrip() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 400) return;
    lastMs = millis();

    String du = useMph ? "mi" : "km";
    String su = useMph ? "mph" : "kmh";
    float cv = useMph ? 0.621371 : 1.0;
    float avgWh = useMph ? avgWhPerKm / 0.621371 : avgWhPerKm;

    unsigned long sec = (millis() - rideStartMs) / 1000;   // trip elapsed; resets with the trip
    char tb[6];
    snprintf(tb, sizeof(tb), "%02d:%02d", (int)((sec / 60) % 100), (int)(sec % 60));

    drawVal(40,  String(tb),                          COL_WHITE);
    drawVal(56,  String(tripDistanceKm * cv, 1) + " " + du, COL_WHITE);
    drawVal(72,  String((int)round(avgSpeedKmh * cv)) + " " + su, COL_WHITE);
    drawVal(88,  String((int)round(maxSpeedKmh * cv)) + " " + su, COL_WHITE);
    drawVal(104, String((int)round(avgWh)) + " wh/" + du, COL_WHITE);
    drawVal(150, String((int)round(totalDistanceKm * cv)) + " " + du, COL_WHITE);
    markDirty(22, 172);
}

// ==========================================
// UPDATE: Page 3 (SETTINGS)
// ==========================================
void updateSettings() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 400) return;
    lastMs = millis();

    WheelProfile &w = wheelProfiles[activeWheelProfile];
    drawVal(40,  String(w.name), COL_WHITE);
    drawVal(56,  String((int)round(w.wheelDiameterM * 1000)) + "mm", COL_WHITE);
    drawVal(72,  String(w.motorPulley) + ":" + String(w.wheelPulley), COL_WHITE);
    drawVal(88,  String((int)w.polePairs), COL_WHITE);
    drawVal(128, String(useMph ? "MPH" : "KM/H"), COL_WHITE);
    drawVal(144, String(DEMO_MODE ? "ON" : "OFF"), DEMO_MODE ? COL_YELLOW : COL_WHITE);
    markDirty(22, 142);
}

// ==========================================
// UPDATE: Page 4 (SYSTEM INFO) — live ESP32 board stats
// ==========================================
void updateSystem() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 500) return;
    lastMs = millis();

    drawVal(40, String(ESP.getChipModel()), COL_WHITE);
    drawVal(56, String(ESP.getChipCores()) + " @ " + String(getCpuFrequencyMhz()) + "M", COL_WHITE);
    float fwUsedMB = ESP.getSketchSize() / 1048576.0f;
    float fwTotMB  = (ESP.getSketchSize() + ESP.getFreeSketchSpace()) / 1048576.0f;
    drawVal(72, String(fwUsedMB, 1) + "/" + String(fwTotMB, 1) + "M", COL_WHITE);

    uint32_t freeK = ESP.getFreeHeap() / 1024;
    drawVal(116, String(freeK) + " kB", freeK < 30 ? COL_ORANGE : COL_WHITE);
    drawVal(132, String(ESP.getMinFreeHeap() / 1024) + " kB", COL_WHITE);
    if (ESP.getPsramSize() > 0)
        drawVal(148, String(ESP.getFreePsram() / 1024) + " kB", COL_WHITE);
    else
        drawVal(148, String("none"), COL_DIM);

    float t = temperatureRead();
    drawVal(192, String(t, 1) + DEG + "C", t > 70 ? COL_ORANGE : COL_WHITE);

    unsigned long up = millis() / 1000;
    char ub[12];
    snprintf(ub, sizeof(ub), "%02lu:%02lu:%02lu", up / 3600, (up / 60) % 60, up % 60);
    drawVal(208, String(ub), COL_WHITE);

    drawVal(224, String(gFps) + "f " + String(gLastPushUs / 1000) + "ms",
            gFps >= 30 ? COL_GREEN : COL_WHITE);
    markDirty(22, 210);
}

void updateCurrentPageContent() {
    if (currentPage == 1) {
        updatePower();
    } else if (currentPage == 2) {
        updateTrip();
    } else if (currentPage == 3) {
        updateSettings();
    } else if (currentPage == 4) {
        updateSystem();
    } else {
        updateSpeed();
        updateStatPanel();
        updateTemps();
        updateRange();
    }
}

// ==========================================
// SAFETY: alerts + overlay banners
// ==========================================
const char* faultName(int f) {
    switch (f) {
        case 1: return "OVER-VOLTAGE";
        case 2: return "UNDER-VOLTAGE";
        case 3: return "DRV FAULT";
        case 4: return "OVER-CURRENT";
        case 5: return "ESC OVER-TEMP";
        case 6: return "MOTOR OVER-TEMP";
        default: return "VESC FAULT";
    }
}

// Highest-priority alert: 1 fault, 2 link-lost, 3 over-temp, 4 crit battery.
int alertState() {
    if (vescFault != 0) return 1;
    if (!vescLinkOk) return 2;
    if (currentMotorTemp > MOTOR_TEMP_LIMIT || currentEscTemp > ESC_TEMP_LIMIT) return 3;
    if (currentBatteryPercent < BATT_CRIT_PCT) return 4;
    return 0;
}

void updateOverlays(int state) {
    static int lastState = 0;
    static String lastText = "";

    if (state == 0) {
        if (lastState != 0) { drawStaticFrame(); gRedrawAll = true; }  // restore page
        lastState = 0;
        lastText = "";
        return;
    }

    bool crit = (state == 4);
    int by = crit ? 108 : 118;
    int bh = crit ? 92 : 64;

    String line1, line2, line3 = "";
    if (state == 1)      { line1 = "! FAULT";      line2 = faultName(vescFault); }
    else if (state == 2) { line1 = "VESC";         line2 = "LINK LOST"; }
    else if (state == 3) {
        line1 = "! HOT";
        line2 = (currentMotorTemp > MOTOR_TEMP_LIMIT)
                ? "MOTOR " + String((int)round(currentMotorTemp)) + DEG + "C"
                : "ESC " + String((int)round(currentEscTemp)) + DEG + "C";
    } else {
        line1 = "LOW BATTERY";
        line2 = "STOP & CHARGE";
        line3 = String(currentBatteryPercent) + "%   " + String(currentVoltage, 1) + " V";
    }

    String textKey = line1 + "|" + line2 + "|" + line3;
    if (state != lastState || textKey != lastText || gRedrawAll) {
        GFX->fillRect(X0 + 8, by, UI_W - 16, bh, COL_RED);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_WHITE);
        if (crit) {
            GFX->setFont(&fonts::FreeSansBold9pt7b);
            GFX->drawString(line1, X0 + UI_W / 2, by + 17);
            GFX->drawString(line2, X0 + UI_W / 2, by + 49);
            GFX->drawString(line3, X0 + UI_W / 2, by + 73);
        } else {
            GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
            GFX->drawString(line1, X0 + UI_W / 2, by + 16);
            GFX->setFont(&fonts::Font0);
            GFX->drawString(line2, X0 + UI_W / 2, by + 48);
        }
        lastText = textKey;
        markDirty(by, bh);
    }
    lastState = state;
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // Restore persisted odometer + trip
    prefs.begin("esk8os", false);
    int storedSchema = prefs.getInt("schema", 0);
    if (storedSchema != STORAGE_SCHEMA_VERSION) {
        totalDistanceKm = 0.0;
        tripDistanceKm = 0.0;
        prefs.putFloat("odo", totalDistanceKm);
        prefs.putFloat("trip", tripDistanceKm);
        prefs.putInt("schema", STORAGE_SCHEMA_VERSION);
    } else {
        totalDistanceKm = prefs.getFloat("odo", 0.0);
        tripDistanceKm  = prefs.getFloat("trip", 0.0);
    }

    activeWheelProfile = prefs.getInt("wheelprof", 0);
    if (activeWheelProfile < 0 || activeWheelProfile >= WHEEL_PROFILE_COUNT) activeWheelProfile = 0;

    rideStartMs = millis();
    sessionTripStartKm = tripDistanceKm;

    #ifndef WOKWI_SIMULATION
    pinMode(15, OUTPUT);      // T-Display-S3 display power rail
    digitalWrite(15, HIGH);
    #endif

    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.setBrightness(0);     // dark until the first frame, avoids a boot flash
    tft.fillScreen(0);

    // Center UI if screen is wider than 170 (e.g. Wokwi's 240px ILI9341)
    X0 = (tft.width() - UI_W) / 2;
    if (X0 < 0) X0 = 0;

    COL_BG      = tft.color565(26, 26, 26);
    COL_BORDER  = tft.color565(68, 68, 68);
    COL_DIM     = tft.color565(136, 136, 136);
    COL_LABEL   = tft.color565(170, 170, 170);
    COL_WHITE   = tft.color565(255, 255, 255);
    COL_GREEN   = tft.color565(0, 200, 100);
    COL_RED     = tft.color565(255, 51, 51);
    COL_BLUE    = tft.color565(68, 136, 255);
    COL_YELLOW  = tft.color565(255, 205, 0);
    COL_ORANGE  = tft.color565(255, 128, 0);

    // Allocate the off-screen frame buffer (full panel, 16-bit, lives in PSRAM
    // on the T-Display-S3). On success all dashboard drawing is double-buffered
    // and flicker-free; if it fails (or on Wokwi, which has no PSRAM) GFX stays
    // pointed at the panel and we render directly as before.
    #ifndef WOKWI_SIMULATION
    // Prefer fast internal SRAM for the frame buffer; fall back to PSRAM if it
    // won't fit, and finally to direct drawing — so the UI always comes up.
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

    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    #ifndef WOKWI_SIMULATION
    Serial1.begin(115200, SERIAL_8N1, VESC_RX_PIN, VESC_TX_PIN);
    UART.setSerialPort(&Serial1);
    tft.setBrightness(255);
    #endif

    waitForBootReady();
    drawStaticFrame();
    updateRangeEstimate();
    updateClock();
    updateCurrentPageContent();
    updateBatteryCells();
    updateBottomBar();
    pushCanvasFull();       // first complete frame -> panel
    gRedrawAll = false;
}

// ==========================================
// LOOP & VESC
// ==========================================
void saveOdo() {
    prefs.putFloat("odo", totalDistanceKm);
    prefs.putFloat("trip", tripDistanceKm);
}

// Fake telemetry for bench testing (Wokwi or DEMO_MODE on hardware). Drives the
// SAME state a real VESC poll would: a speed ramp, load-proportional draw with
// regen on braking, integrated energy (Wh used/regen), and first-order thermal
// models for motor/ESC/battery — so every page animates as if wired to the ESC.
void simulateTelemetry() {
    static unsigned long lastMs = 0;
    unsigned long now = millis();
    float dt = (lastMs == 0) ? 0.1f : (now - lastMs) / 1000.0f;   // seconds
    lastMs = now;

    // Speed: ramp 0 -> 45 km/h and back down, cycling.
    static bool accel = true;
    if (accel) currentSpeedKmh += 0.5; else currentSpeedKmh -= 0.5;
    if (currentSpeedKmh >= 45.0) accel = false;
    if (currentSpeedKmh <= 0.0) accel = true;
    currentSpeedMph = currentSpeedKmh * 0.621371;

    currentDuty = constrain(currentSpeedKmh / 45.0 * 100.0, 0.0, 100.0);

    // Current: draw under acceleration, regen (negative) under braking.
    if (accel) {
        currentMotorAmps = currentSpeedKmh * 1.2f;
        currentAmps      = currentSpeedKmh * 0.7f;
    } else {
        currentMotorAmps = -currentSpeedKmh * 0.5f;   // regen braking
        currentAmps      = -currentSpeedKmh * 0.3f;
    }
    currentWatts = currentVoltage * currentAmps;

    // Integrate energy: positive power -> Wh used, negative -> Wh regenerated.
    float dWh = currentVoltage * currentAmps * (dt / 3600.0f);
    if (dWh >= 0) currentWattHours += dWh;
    else          currentWhRegen   += -dWh;

    // Thermal models: each component eases toward a load-dependent target with a
    // first-order lag, so temps climb under load and cool when coasting. Targets
    // stay just under the alert limits so the demo doesn't trip the over-temp banner.
    float load = currentDuty / 100.0f;
    const float ambient = 24.0f;
    float kMotor = 1.0f - expf(-dt / 18.0f);
    float kEsc   = 1.0f - expf(-dt / 14.0f);
    float kBatt  = 1.0f - expf(-dt / 30.0f);
    currentMotorTemp   += ((ambient + 52.0f * load) - currentMotorTemp)   * kMotor;
    currentEscTemp     += ((ambient + 36.0f * load) - currentEscTemp)     * kEsc;
    currentBatteryTemp += ((ambient + 12.0f * load) - currentBatteryTemp) * kBatt;

    lastVescOkMs = millis();      // demo link always "up"

    static unsigned long lastDrain = 0;
    if (millis() - lastDrain > 2000) {
        lastDrain = millis();
        if (currentBatteryPercent > 0) currentBatteryPercent--;
        currentVoltage = BATTERY_MIN_V + (BATTERY_MAX_V - BATTERY_MIN_V) * currentBatteryPercent / 100.0;
    }
}

float configuredNominalPackWh() {
    return BATTERY_EFFECTIVE_CAPACITY_AH * BATTERY_CELLS_COUNT * BATTERY_NOMINAL_CELL_V;
}

float configuredUsablePackWh() {
    // Scale nominal pack Wh by the configured usable voltage window. This keeps
    // range conservative and aligned with the voltage where the dashboard says
    // to stop, instead of estimating all the way to a fully depleted cell.
    float usableWindow = max(0.1f, BATTERY_FULL_CELL_V - BATTERY_STOP_CELL_V);
    float nominalWindow = max(0.1f, BATTERY_FULL_CELL_V - 3.0f);
    return configuredNominalPackWh() * constrain(usableWindow / nominalWindow, 0.0f, 1.0f);
}

float defaultWhPerKm() {
    return RANGE_DEFAULT_WH_PER_MILE / 1.609344f;
}

void updateRangeEstimate() {
    float sessionDistanceKm = tripDistanceKm - sessionTripStartKm;
    float netWhUsed = currentWattHours - currentWhRegen;
    float whPerKm = defaultWhPerKm();

    // Simulated telemetry has no real ride to learn from, so shorten the
    // learn-in window in demo/sim — lets ESTIMATED + AVG WH animate on the bench.
    // Real-ESC riding keeps the conservative full thresholds.
    float learnDist = DEMO_DATA ? 0.05f : RANGE_LEARN_MIN_DISTANCE_KM;
    float learnWh   = DEMO_DATA ? 1.0f  : RANGE_LEARN_MIN_WH;

    rangeEstimateReady = false;
    if (sessionDistanceKm >= learnDist && netWhUsed >= learnWh) {
        float learnedWhPerKm = netWhUsed / sessionDistanceKm;
        if (learnedWhPerKm >= defaultWhPerKm() * 0.6f && learnedWhPerKm <= defaultWhPerKm() * 2.0f) {
            whPerKm = learnedWhPerKm;
            rangeEstimateReady = true;
        }
    }

    avgWhPerKm = whPerKm;
    float packWh = configuredUsablePackWh();
    float remainingWh = packWh * constrain(currentBatteryPercent, 0, 100) / 100.0f;
    estimatedRangeKm = packWh / avgWhPerKm;
    remainingRangeKm = remainingWh / avgWhPerKm;
}

void pollVescData() {
    bool useSim = true;
    #ifndef WOKWI_SIMULATION
    useSim = DEMO_MODE;
    if (!useSim && UART.getVescValues()) {
        float wheelRPM = (UART.data.rpm / profilePolePairs()) * profileGearRatio();
        currentSpeedKmh = (wheelRPM * profileCircumfM() * 60.0) / 1000.0;
        currentSpeedMph = currentSpeedKmh * 0.621371;

        currentVoltage = UART.data.inpVoltage;
        float pct = ((currentVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) * 100.0;
        currentBatteryPercent = constrain((int)pct, 0, 100);

        currentMotorTemp = UART.data.tempMotor;
        currentEscTemp = UART.data.tempMosfet;
        currentAmps = UART.data.avgInputCurrent;
        currentMotorAmps = UART.data.avgMotorCurrent;
        currentDuty = UART.data.dutyCycleNow * 100.0;
        if (!rideEnergyBaselineSet) {
            rideStartVescWh = UART.data.wattHours;
            rideStartVescWhRegen = UART.data.wattHoursCharged;
            rideEnergyBaselineSet = true;
        }
        currentWattHours = max(0.0f, UART.data.wattHours - rideStartVescWh);
        currentWhRegen = max(0.0f, UART.data.wattHoursCharged - rideStartVescWhRegen);
        currentWatts = currentVoltage * currentAmps;
        vescFault = (int)UART.data.error;
        lastVescOkMs = millis();
    }
    #endif
    if (useSim) simulateTelemetry();

    // Peak-hold: rise instantly to new peaks, ease back down (~2-3s) so the
    // spike is readable instead of flickering.
    if (currentWatts >= peakWatts) peakWatts = currentWatts;
    else peakWatts += (currentWatts - peakWatts) * 0.15f;
    if (peakWatts < 0) peakWatts = 0;   // regen can drive watts negative; peak-hold stays >= 0

    // Session extremes for the Power page SESSION card
    if (currentWatts > maxWattsSession)              maxWattsSession = currentWatts;
    if (currentVoltage < minVoltageSession)          minVoltageSession = currentVoltage;
    if (fabs(currentMotorAmps) > maxMotorAmpsSession) maxMotorAmpsSession = fabs(currentMotorAmps);

    // Accumulate trip + odometer from speed (km), then persist to flash on
    // every stop and every 60s so they survive a disconnect / power-off.
    static unsigned long lastDistMs = 0;
    unsigned long nowMs = millis();
    if (lastDistMs != 0) {
        float dKm = currentSpeedKmh * ((nowMs - lastDistMs) / 3600000.0f);
        if (dKm > 0) { tripDistanceKm += dKm; totalDistanceKm += dKm; }
    }
    lastDistMs = nowMs;

    static unsigned long lastSave = 0;
    static bool wasMoving = false;
    bool moving = currentSpeedKmh > 1.0;
    if ((wasMoving && !moving) || (millis() - lastSave > 60000)) {
        saveOdo();
        lastSave = millis();
    }
    wasMoving = moving;

    // Telemetry link freshness, session max + average speed
    vescLinkOk = (millis() - lastVescOkMs < VESC_LINK_TIMEOUT_MS);
    if (currentSpeedKmh > maxSpeedKmh) maxSpeedKmh = currentSpeedKmh;
    float hrs = (millis() - rideStartMs) / 3600000.0f;
    if (hrs > 0.0001f) avgSpeedKmh = (tripDistanceKm - sessionTripStartKm) / hrs;
    updateRangeEstimate();
}

// ==========================================
// VESC TOOL BRIDGE MODE (WiFi-TCP)  — rendered into the canvas (flicker-free)
// ==========================================
void updateBridgeStatus(const char* status) {
    bridgeStatus = status;
    GFX->fillRect(X0 + 8, 212, UI_W - 16, 38, COL_BG);
    GFX->drawRect(X0 + 8, 212, UI_W - 16, 38, COL_BORDER);
    uint16_t c = COL_DIM;
    if (strcmp(status, "CONNECTED") == 0 || strcmp(status, "TRAFFIC") == 0) c = COL_GREEN;
    else if (strcmp(status, "ERROR") == 0) c = COL_RED;
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(c);
    GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
    GFX->drawString(status, X0 + UI_W / 2, 232);
    GFX->setFont(&fonts::Font0);
    pushCanvasFull();
}

// Live throughput + connected-station count, just under the status box.
void updateBridgeStats() {
    GFX->fillRect(X0 + 8, 254, UI_W - 16, 12, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    String s = "RX " + String(bridgeRxBytes / 1024) + "K  TX " + String(bridgeTxBytes / 1024) +
               "K  STA " + String(WiFi.softAPgetStationNum());
    GFX->drawString(s, X0 + UI_W / 2, 260);
    pushCanvasFull();
}

void drawBridgeScreen() {
    GFX->fillScreen(COL_BG);

    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_BLUE);
    GFX->setFreeFont(&fonts::FreeSansBold12pt7b);
    GFX->drawString("BRIDGE MODE", X0 + UI_W / 2, 18);

    GFX->setFont(&fonts::Font0);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString("VESC TOOL CONFIG", X0 + UI_W / 2, 56);

    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);  GFX->drawString("WiFi:",  X0 + 12, 84);
    GFX->setTextColor(COL_WHITE); GFX->drawString(BRIDGE_SSID, X0 + 46, 84);
    GFX->setTextColor(COL_DIM);  GFX->drawString("pass:",  X0 + 12, 100);
    GFX->setTextColor(COL_WHITE); GFX->drawString(BRIDGE_PASS, X0 + 46, 100);
    GFX->setTextColor(COL_DIM);  GFX->drawString("TCP:",   X0 + 12, 124);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString(WiFi.softAPIP().toString() + ":65102", X0 + 40, 124);

    GFX->setTextColor(COL_DIM);
    GFX->drawString("VESC Tool > Connection", X0 + 12, 150);
    GFX->drawString("> TCP > connect", X0 + 12, 162);

    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("hold L+R to exit", X0 + UI_W / 2, 300);

    updateBridgeStatus(bridgeStatus.c_str());
    updateBridgeStats();                        // pushes the finished frame
}

void bridgeStart() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(BRIDGE_SSID, BRIDGE_PASS);
    bridgeServer.begin();
    bridgeServer.setNoDelay(true);
    bridgeStatus = "WAITING";
    bridgeRxBytes = 0;
    bridgeTxBytes = 0;
}

void bridgeStop() {
    if (bridgeClient) bridgeClient.stop();
    bridgeServer.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

// Raw byte forwarding between the TCP client (VESC Tool) and Serial1 (FSESC).
void bridgeLoop() {
    if (!bridgeClient || !bridgeClient.connected()) {
        WiFiClient nc = bridgeServer.available();
        if (nc) { bridgeClient = nc; updateBridgeStatus("CONNECTED"); }
    }

    uint8_t buf[256];
    bool traffic = false;

    if (bridgeClient && bridgeClient.connected()) {
        int n = bridgeClient.available();
        if (n > 0) {
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            int r = bridgeClient.read(buf, n);
            if (r > 0) { Serial1.write(buf, r); bridgeRxBytes += r; traffic = true; }
        }
    }
    int sa = Serial1.available();
    if (sa > 0 && bridgeClient && bridgeClient.connected()) {
        if (sa > (int)sizeof(buf)) sa = sizeof(buf);
        int r = Serial1.readBytes(buf, sa);
        if (r > 0) { bridgeClient.write(buf, r); bridgeTxBytes += r; traffic = true; }
    }

    static unsigned long lastTrafficShown = 0;
    if (traffic && millis() - lastTrafficShown > 300) {
        lastTrafficShown = millis();
        updateBridgeStatus("TRAFFIC");
    }
    static bool wasConn = false;
    bool nowConn = bridgeClient && bridgeClient.connected();
    if (wasConn && !nowConn) updateBridgeStatus("WAITING");
    wasConn = nowConn;

    // Refresh the throughput / station-count line a couple times a second.
    static unsigned long lastStats = 0;
    if (millis() - lastStats > 500) { lastStats = millis(); updateBridgeStats(); }
}

void enterBridgeMode() {
    if (!DEMO_DATA && currentSpeedKmh > 1.0) {   // live safety: only when stopped
        GFX->fillRect(X0 + 8, 140, UI_W - 16, 40, COL_RED);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_WHITE);
        GFX->setFreeFont(&fonts::FreeSans12pt7b);
        GFX->drawString("STOP BOARD FIRST", X0 + UI_W / 2, 160);
        GFX->setFont(&fonts::Font0);
        pushCanvasFull();
        delay(1200);
        drawStaticFrame();
        gRedrawAll = true;
        return;
    }
    saveOdo();
    systemMode = MODE_VESC_BRIDGE;
    bridgeStart();
    drawBridgeScreen();
}

void exitBridgeMode() {
    bridgeStop();
    while (Serial1.available()) Serial1.read();   // flush stale VESC replies
    systemMode = MODE_DASHBOARD;
    lastVescOkMs = millis();
    currentPage = 0;
    drawStaticFrame();
    gRedrawAll = true;
}

void checkButtons() {
    bool left = digitalRead(BTN_LEFT);
    bool right = digitalRead(BTN_RIGHT);

    // ---- BOTH held 2s: enter/exit bridge mode (suppresses single actions) ----
    static unsigned long bothDownAt = 0;
    static bool bothHandled = false;
    static bool lockSingles = false;
    if (left == LOW && right == LOW) {
        lockSingles = true;
        if (bothDownAt == 0) { bothDownAt = millis(); bothHandled = false; }
        if (!bothHandled && millis() - bothDownAt > 2000) {
            bothHandled = true;
            if (systemMode == MODE_DASHBOARD) enterBridgeMode();
            else                              exitBridgeMode();
        }
        lastLeftBtn = left; lastRightBtn = right;
        return;
    }
    bothDownAt = 0;
    bothHandled = false;
    if (lockSingles) {                       // wait for full release before singles resume
        if (left == HIGH && right == HIGH) lockSingles = false;
        lastLeftBtn = left; lastRightBtn = right;
        return;
    }
    if (systemMode == MODE_VESC_BRIDGE) { lastLeftBtn = left; lastRightBtn = right; return; }

    // LEFT button: short-press cycles pages, hold ~1.5s resets the trip
    static unsigned long leftDownAt = 0;
    static bool leftHandled = false;
    if (left == LOW) {
        if (lastLeftBtn == HIGH) { leftDownAt = millis(); leftHandled = false; }
        if (!leftHandled && millis() - leftDownAt > 1500) {     // long press: reset trip
            tripDistanceKm = 0;
            sessionTripStartKm = 0;
            rideStartMs = millis();
            avgSpeedKmh = 0;
            maxSpeedKmh = 0;
            currentWattHours = 0;
            currentWhRegen = 0;
            avgWhPerKm = 0;
            estimatedRangeKm = 0;
            remainingRangeKm = 0;
            rangeEstimateReady = false;
            rideEnergyBaselineSet = false;
            maxWattsSession = 0;
            minVoltageSession = BATTERY_MAX_V;
            maxMotorAmpsSession = 0;
            // Demo: also recharge the pack and clear temps so the bench loop can
            // run indefinitely. On a real ESC these come straight from the VESC,
            // so the recharge is skipped (it would be overwritten on next poll).
            if (DEMO_DATA) {
                currentBatteryPercent = 100;
                currentVoltage = BATTERY_MAX_V;
                currentMotorTemp = 0;
                currentEscTemp = 0;
                currentBatteryTemp = 0;
                peakWatts = 0;
            }
            saveOdo();
            leftHandled = true;
            drawStaticFrame();
            gRedrawAll = true;
            showToast(DEMO_DATA ? "RECHARGED" : "TRIP RESET");
        }
    } else if (lastLeftBtn == LOW && !leftHandled && millis() - leftDownAt > 30) {
        currentPage = (currentPage + 1) % PAGE_COUNT;        // short press: next page
        drawStaticFrame();
        gRedrawAll = true;
    }
    lastLeftBtn = left;

    // RIGHT button: on Settings page cycles wheel profile, else toggles units
    if (right == LOW && lastRightBtn == HIGH && millis() - lastRightPress > 200) {
        if (currentPage == 3) {
            activeWheelProfile = (activeWheelProfile + 1) % WHEEL_PROFILE_COUNT;
            prefs.putInt("wheelprof", activeWheelProfile);
            gRedrawAll = true;             // refresh settings values
        } else {
            useMph = !useMph;
            drawStaticFrame();
            gRedrawAll = true;
        }
        lastRightPress = millis();
    }
    lastRightBtn = right;
}

void dashboardLoop() {
    unsigned long now = millis();

    if (now - lastDataPoll > 100) {
        pollVescData();
        lastDataPoll = now;
    }

    int alert = alertState();
    updateClock();

    if (alert == 0 || gRedrawAll) {   // first repaint still draws zero placeholders under alerts
        updateCurrentPageContent();
    }

    updateBatteryCells();   // common, below any banner
    updateBottomBar();
    updateOverlays(alert);

    gRedrawAll = false;   // one-shot full repaint consumed

    // Transient confirmation toast (drawn on top of the finished frame).
    static bool toastWasUp = false;
    bool toastUp = (long)(gToastUntil - millis()) > 0;
    if (toastUp) {
        GFX->setFreeFont(&fonts::FreeSans12pt7b);
        int tw = GFX->textWidth(gToastMsg) + 28;
        int tx = X0 + (UI_W - tw) / 2;
        GFX->fillRect(tx, 150, tw, 30, COL_GREEN);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_BG);
        GFX->drawString(gToastMsg, X0 + UI_W / 2, 165);
        GFX->setFont(&fonts::Font0);
        markDirty(150, 30);
    } else if (toastWasUp) {
        // The toast covered chrome (card borders/labels), so repaint the whole
        // page rather than just the value bands.
        drawStaticFrame();
        updateCurrentPageContent();
        updateBatteryCells();
        updateBottomBar();
    }
    toastWasUp = toastUp;

    // SYSTEM page benchmarks the worst case: force a full-frame blit every loop
    // so its FPS readout shows the full-repaint ceiling. Other pages push only
    // the changed bands, so their effective refresh is much higher.
    if (currentPage == 4) markDirty(0, gCanvasH);
    if (gDirtyN) pushCanvas();

    delay(2);   // keep buttons responsive; pushes are change-gated anyway
}

void loop() {
    checkButtons();

    if (systemMode == MODE_VESC_BRIDGE) {
        bridgeLoop();
        delay(1);
        return;
    }

    dashboardLoop();
}
