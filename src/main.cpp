#include <Arduino.h>
#include "LGFX_Config.h"   // LovyanGFX device + sprite (DMA on the S3 parallel bus)
#include <Preferences.h>
#ifndef WOKWI_SIMULATION
#include <VescUart.h>
#endif
#include "esk8os.h"        // shared core: types, enums, cross-module globals
#include "bridge.h"        // VESC Tool bridge mode (WiFi-TCP + BLE)
#include "telemetry.h"     // VESC poll / demo sim / range / history / ride logs
#include "ble_bridge.h"    // BLE (Nordic UART) backend for mobile VESC Tool
#include "ui.h"            // boot splash, pages, status bars, overlays

// ==========================================
// USER CONFIG  — edit these to personalize
// ==========================================
const char* PRODUCT_NAME = "ESK8OS";     // brand shown on splash + top bar
const char* RIDER_NAME   = "JOE";        // your name
// Firmware version comes from version.txt (stamped into version.h at build time):
//   FW_VERSION      e.g. "v0.4.0"          -> splash / top corner (ui.cpp)
//   FW_VERSION_FULL e.g. "v0.4.0 a1b2c3d"  -> System page (ui.cpp)
const bool  USE_MPH_DEFAULT = true;      // true = MPH, false = KM/H at boot
const int   STORAGE_SCHEMA_VERSION = 2;  // bump once to clear old demo trip/odo

// Demo mode feeds simulated telemetry even on the physical board, so you can
// bench-test the UI on the device without the FSESC connected. This is now a
// RUNTIME setting (persisted in NVS) you toggle from the Settings page — no
// reflashing to switch between demo and live. DEMO_MODE_DEFAULT is the value
// used on first boot / after a settings wipe.
// TEMPORARY: defaulted ON while the Flipsky Dual FSESC 6.7 Plus is out for
// replacement — set the default false once the new ESC is wired in.
const bool DEMO_MODE_DEFAULT = true;
bool gDemoMode = DEMO_MODE_DEFAULT;   // loaded from NVS in setup(); edited in Settings
// DEMO_DATA macro + PageId/SystemMode enums now live in esk8os.h.
// Battery warning thresholds (BATT_*_PCT) + temp limits (MOTOR/ESC_TEMP_LIMIT)
// now live in esk8os.h (shared with ui.cpp).

// Battery pack capacity used by the display's range estimate. The cells are
// nominally 2800 mAh in 10S6P (16.8 Ah), but this pack is configured with a
// conservative effective capacity of 16.5 Ah to better match real cells/VESC.
const int   BATTERY_PARALLEL_COUNT = 6;
const int   BATTERY_CELL_CAPACITY_MAH = 2800;
float BATTERY_EFFECTIVE_CAPACITY_AH = 16.5;     // runtime setting: Settings > BATTERY > PACK AH
// const cell voltages + range learn-in thresholds live in esk8os.h
float BATTERY_STOP_CELL_V = 3.30;               // runtime setting: dashboard 0%; align with VESC cutoff/end
float RANGE_DEFAULT_WH_PER_MILE = 22.0;         // runtime setting: used until enough ride data is learned

// VESC_LINK_TIMEOUT_MS lives in esk8os.h

// ==========================================
// PHYSICAL HARDWARE SPECS
// ==========================================
int   BATTERY_CELLS_COUNT = 10;       // runtime setting: Settings > BATTERY > CELLS
float BATTERY_MAX_V = BATTERY_CELLS_COUNT * BATTERY_FULL_CELL_V;
float BATTERY_MIN_V = BATTERY_CELLS_COUNT * BATTERY_STOP_CELL_V;

void recalcBatteryBounds() {
    BATTERY_MAX_V = BATTERY_CELLS_COUNT * BATTERY_FULL_CELL_V;
    BATTERY_MIN_V = BATTERY_CELLS_COUNT * BATTERY_STOP_CELL_V;
}
// Wheel/gearing profiles for the DISPLAY's own speed + distance math (these do
// NOT write to the FSESC — they only keep the gauge accurate; use bridge mode +
// VESC Tool to change the VESC's own config). Selected profile persists in NVS.
// (WheelProfile struct lives in esk8os.h, shared with ui.cpp.)
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

// VESC Tool bridge mode (WiFi-TCP + BLE) lives in bridge.cpp / bridge.h.

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
uint16_t COL_ACCENT;    // #b950d7 brand violet/purple (highlights, logo, dots).
                        // Magenta-shifted (R~=B) so it reads purple, not blue, on the panel.
uint16_t COL_YELLOW;    // #ffcd00
uint16_t COL_ORANGE;    // #ff8000

// Color-zone helpers (battColor/wattColor/dutyColor) now live in ui.cpp.

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

// Page system + top-level mode: PageId / SystemMode enums are defined in esk8os.h.
int currentPage = PAGE_HUD;
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

// ==========================================
// TELEMETRY HISTORY BUFFER
// Stores recent telemetry in RAM for realtime graphs. This does not touch flash
// and resets on power cycle.
// ==========================================
TelemetrySample history[HIST_N];
int histHead = 0;
int histCount = 0;


// Per-component health indicators shown next to each temperature (the green
// "(90%)" in the design). Placeholder values until wired to a real metric.
int   motorHealthPct = 90;
int   batteryHealthPct = 85;
int   escHealthPct = 75;

bool useMph = USE_MPH_DEFAULT;
unsigned long lastDataPoll = 0;

// Display backlight, persisted, edited from the Settings page. Stepped through a
// few fixed levels so it's adjustable with one button.
int gBrightnessPct = 100;
const int BRIGHTNESS_STEPS[] = { 25, 50, 75, 100 };
const int BRIGHTNESS_STEP_COUNT = sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]);
void applyBrightness() { tft.setBrightness((uint8_t)(gBrightnessPct * 255 / 100)); }

// Settings page is an editable menu: a cursor selects a row, RIGHT changes its
// value. The SET_* row indices live in esk8os.h (shared with ui.cpp).
int settingsCursor = 0;

// ==========================================
// LAYOUT CONSTANTS (Designed for 170px width)
// ==========================================
int X0 = 0; // X offset to center 170px UI on wider Wokwi screen (UI_W is in esk8os.h)

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
    if (!gUseCanvas || gDirtyN == 0) return;
    // ISOLATION TEST: partial clip-rect blit at 20 MHz. Push only the changed
    // bands (markDirty tracks them) to cut bus traffic. If this garbles the panel
    // like 40 MHz did, revert to the single full canvas.pushSprite(0,0) above.
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

    // Restore Settings-page preferences (editable at runtime, persisted in NVS).
    gDemoMode      = prefs.getBool("demo", DEMO_MODE_DEFAULT);
    useMph         = prefs.getBool("mph", USE_MPH_DEFAULT);
    gBrightnessPct = constrain(prefs.getInt("bright", 100), 10, 100);

    // Display-side battery settings. These affect dashboard math only. They do
    // not write VESC cutoffs/current limits; keep those matched in VESC Tool.
    BATTERY_CELLS_COUNT = constrain(prefs.getInt("cells", BATTERY_CELLS_COUNT), 6, 14);
    BATTERY_EFFECTIVE_CAPACITY_AH = constrain(prefs.getFloat("packAh", BATTERY_EFFECTIVE_CAPACITY_AH), 4.0f, 40.0f);
    BATTERY_STOP_CELL_V = constrain(prefs.getFloat("stopCell", BATTERY_STOP_CELL_V), 3.00f, 3.60f);
    RANGE_DEFAULT_WH_PER_MILE = constrain(prefs.getFloat("whmi", RANGE_DEFAULT_WH_PER_MILE), 14.0f, 40.0f);
    recalcBatteryBounds();
    currentVoltage = BATTERY_MAX_V;
    minVoltageSession = BATTERY_MAX_V;

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
    COL_ACCENT  = tft.color565(185, 80, 215);
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
    applyBrightness();          // light up at the persisted brightness level
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
            saveRideSummaryLog();
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
        // Short press. On the Settings page LEFT steps the cursor down through the
        // editable rows, then leaves to the next page after the last one.
        if (currentPage == PAGE_SETTINGS && settingsCursor < SETTINGS_COUNT - 1) {
            settingsCursor++;
            drawStaticFrame();                               // move the highlight
            gRedrawAll = true;
        } else {
            settingsCursor = 0;
            currentPage = (currentPage + 1) % PAGE_COUNT;    // next page
            drawStaticFrame();
            gRedrawAll = true;
        }
    }
    lastLeftBtn = left;

    // RIGHT button: on the Settings page edits the highlighted setting; on any
    // other page it toggles units (the common quick action).
    if (right == LOW && lastRightBtn == HIGH && millis() - lastRightPress > 200) {
        if (currentPage == PAGE_SETTINGS) {
            switch (settingsCursor) {
                case SET_PROFILE:
                    activeWheelProfile = (activeWheelProfile + 1) % WHEEL_PROFILE_COUNT;
                    prefs.putInt("wheelprof", activeWheelProfile);
                    break;
                case SET_UNITS:
                    useMph = !useMph;
                    prefs.putBool("mph", useMph);
                    break;
                case SET_DEMO:
                    gDemoMode = !gDemoMode;
                    prefs.putBool("demo", gDemoMode);
                    rideEnergyBaselineSet = false;   // re-baseline energy on the source switch
                    break;
                case SET_BRIGHT: {
                    int i = 0;
                    while (i < BRIGHTNESS_STEP_COUNT && BRIGHTNESS_STEPS[i] != gBrightnessPct) i++;
                    gBrightnessPct = BRIGHTNESS_STEPS[(i + 1) % BRIGHTNESS_STEP_COUNT];
                    prefs.putInt("bright", gBrightnessPct);
                    applyBrightness();
                    break;
                }
                case SET_CELLS:
                    BATTERY_CELLS_COUNT++;
                    if (BATTERY_CELLS_COUNT > 14) BATTERY_CELLS_COUNT = 6;
                    prefs.putInt("cells", BATTERY_CELLS_COUNT);
                    recalcBatteryBounds();
                    currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
                    minVoltageSession = BATTERY_MAX_V;
                    updateRangeEstimate();
                    drawStaticFrame();
                    break;
                case SET_PACK_AH:
                    BATTERY_EFFECTIVE_CAPACITY_AH += 0.5f;
                    if (BATTERY_EFFECTIVE_CAPACITY_AH > 40.0f) BATTERY_EFFECTIVE_CAPACITY_AH = 4.0f;
                    prefs.putFloat("packAh", BATTERY_EFFECTIVE_CAPACITY_AH);
                    updateRangeEstimate();
                    break;
                case SET_STOP_CELL:
                    BATTERY_STOP_CELL_V += 0.05f;
                    if (BATTERY_STOP_CELL_V > 3.60f) BATTERY_STOP_CELL_V = 3.00f;
                    prefs.putFloat("stopCell", BATTERY_STOP_CELL_V);
                    recalcBatteryBounds();
                    currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
                    updateRangeEstimate();
                    drawStaticFrame();
                    break;
                case SET_WHMI:
                    RANGE_DEFAULT_WH_PER_MILE += 1.0f;
                    if (RANGE_DEFAULT_WH_PER_MILE > 40.0f) RANGE_DEFAULT_WH_PER_MILE = 14.0f;
                    prefs.putFloat("whmi", RANGE_DEFAULT_WH_PER_MILE);
                    updateRangeEstimate();
                    break;
            }
            gRedrawAll = true;                 // refresh the settings values
        } else {
            useMph = !useMph;
            prefs.putBool("mph", useMph);
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
        recordHistorySample();
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
        GFX->setFont(&fonts::FreeSans12pt7b);
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
    if (currentPage == PAGE_SYSTEM) markDirty(0, gCanvasH);
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
