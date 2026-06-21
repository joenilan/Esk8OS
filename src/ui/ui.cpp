#include "esk8os.h"        // shared core: types, enums, cross-module globals
#include "ui.h"            // public UI entry points used by main.cpp
#include "telemetry/telemetry.h"     // getHistorySample() for the graph pages
#include "logging/ridelog.h"       // detailed-log status for the LOGS page warning
#include "BebasNeue18.h"   // condensed small labels
#include "BebasNeue24.h"   // condensed medium — overlays, units
#include "BebasNeue34.h"   // condensed large — panel values, battery %
#include "BebasNeue40.h"   // condensed hero — splash wordmark
#include "BebasNeue80.h"   // hero speed number (DASH page)
#include "BebasNeue110.h"  // native crisp hero font for the Big HUD speed
// FW_VERSION / FW_VERSION_FULL are stamped into version.h by the pre-build hook
// (see main.cpp). Same __has_include fallback so this TU compiles standalone.
#if __has_include("version.h")
  #include "version.h"
#else
  #define FW_VERSION      "v0.0.0"
  #define FW_VERSION_FULL "v0.0.0 nobuild"
#endif

// ==========================================
// COLOR ZONE HELPERS
// ==========================================
// Battery zone color: green -> yellow -> orange -> red as charge drops.
static uint16_t battColor(int pct) {
    if (pct >= BATT_WARN_PCT) return COL_GREEN;
    if (pct >= BATT_LOW_PCT)  return COL_YELLOW;
    if (pct >= BATT_CRIT_PCT) return COL_ORANGE;
    return COL_RED;
}

// Power readout zones: white -> yellow -> orange -> red as wattage climbs.
static uint16_t wattColor(int w) {
    if (w >= 3000) return COL_RED;
    if (w >= 2000) return COL_ORANGE;
    if (w >= 1000) return COL_YELLOW;
    return COL_WHITE;
}

// Duty-cycle zones: the top end (near 100%) is where you run out of headroom.
static uint16_t dutyColor(int d) {
    if (d >= 95) return COL_RED;
    if (d >= 85) return COL_ORANGE;
    if (d >= 70) return COL_YELLOW;
    return COL_WHITE;
}

// ==========================================
// BATTERY CELL STRIP
// ==========================================
static int batteryCellGap() {
    return BATTERY_CELLS_COUNT > 12 ? 1 : 2;
}

static int batteryCellW() {
    int gap = batteryCellGap();
    int w = (UI_W - ((BATTERY_CELLS_COUNT - 1) * gap)) / BATTERY_CELLS_COUNT;
    return constrain(w, 5, 13);
}

static int batteryCellsTotalW() {
    int gap = batteryCellGap();
    return (BATTERY_CELLS_COUNT * batteryCellW()) + ((BATTERY_CELLS_COUNT - 1) * gap);
}

static int batteryCellsStartX() {
    return X0 + (UI_W - batteryCellsTotalW()) / 2;
}

static void drawBatteryCellsRow(int y, int cellH, bool drawFill) {
    int cellW = batteryCellW();
    int cellGap = batteryCellGap();
    int cellStartX = batteryCellsStartX();
    // Continuous level so the gauge depletes smoothly: whole cells fill solid and
    // the boundary cell fills a fractional WIDTH instead of popping on/off, so the
    // bar shrinks ~1px at a time while staying segmented.
    float level = currentBatteryPercent * BATTERY_CELLS_COUNT / 100.0f;
    int   full  = (int)level;
    float frac  = level - full;
    uint16_t fillCol = battColor(currentBatteryPercent);
    for (int i = 0; i < BATTERY_CELLS_COUNT; i++) {
        int cx = cellStartX + i * (cellW + cellGap);
        GFX->drawRect(cx, y, cellW, cellH, COL_BORDER);
        if (!drawFill) continue;
        GFX->fillRect(cx + 1, y + 1, cellW - 2, cellH - 2, COL_BG);   // clear interior
        if (i < full) {
            GFX->fillRect(cx + 1, y + 1, cellW - 2, cellH - 2, fillCol);
        } else if (i == full && frac > 0.0f) {
            int fw = (int)round((cellW - 2) * frac);
            if (fw > 0) GFX->fillRect(cx + 1, y + 1, fw, cellH - 2, fillCol);
            else        GFX->drawFastHLine(cx + 2, y + cellH - 2, max(0, cellW - 4), COL_BORDER);
        } else {
            GFX->drawFastHLine(cx + 2, y + cellH - 2, max(0, cellW - 4), COL_BORDER);
        }
    }
}

// Show a brief centered confirmation banner over the dashboard (~1.2 s).
void showToast(const char* msg) {
    strncpy(gToastMsg, msg, 20 - 1);
    gToastMsg[20 - 1] = '\0';
    gToastUntil = millis() + 1200;
}

// Helpers
static void drawHLine(int x, int y, int w, uint16_t color) {
    GFX->drawFastHLine(x, y, w, color);
}

// Bordered card with a centered header label and an underline.
static void drawCard(int x, int y, int w, int h, const char* title) {
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
static void drawBootProgress(int pct, const char* status, uint16_t color) {
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

static void drawBootSplashFrame() {
    GFX->fillScreen(COL_BG);
    GFX->setFont(&fonts::Font0);

    // Version (top-right, faint)
    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(COL_BORDER);
    GFX->drawString(FW_VERSION, X0 + UI_W - 6, 6);

    // Wordmark: big "ESK8" with a small superscript "OS" -> "ESK8 OS"
    const int topY = 60, gap = 4;
    GFX->setTextDatum(TL_DATUM);
    GFX->setFont(&BebasNeue110pt7b);
    int wMain = GFX->textWidth("ESK8");
    GFX->setFont(&BebasNeue34pt7b);
    int wOs = GFX->textWidth("OS");
    int startX = X0 + (UI_W - (wMain + gap + wOs)) / 2;

    GFX->setFont(&BebasNeue110pt7b);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString("ESK8", startX, topY);
    GFX->setFont(&BebasNeue34pt7b);
    GFX->setTextColor(COL_ACCENT);                 // superscript, top-aligned
    GFX->drawString("OS", startX + wMain + gap, topY);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("RIDE DASHBOARD", X0 + UI_W / 2, 188);

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
    drawBootProgress(100, "SIMULATION MODE", COL_ACCENT);
    delay(500);
    #else
    drawBootProgress(60, "UART READY", COL_DIM);
    delay(150);

    if (gDemoMode) {
        drawBootProgress(100, "DEMO TELEMETRY", COL_ACCENT);
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
static void drawRowLabel(const char* label, int y) {
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(label, X0 + 12, y);
}

static void drawSpeedReadout(int spdInt, bool clearZone) {
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
static void drawStaticDash() {
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
static void drawStaticPower() {
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
static void drawStaticTrip() {
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
// An editable settings row label: highlighted in the accent with a ">" cursor
// when it's the selected row, otherwise a plain dim label like the others.
static void drawSettingLabel(const char* label, int y, int idx) {
    bool sel = (settingsCursor == idx);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TL_DATUM);
    if (sel) {
        GFX->setTextColor(COL_ACCENT);
        GFX->drawString(">", X0 + 4, y);
    }
    GFX->setTextColor(sel ? COL_ACCENT : COL_DIM);
    GFX->drawString(label, X0 + 12, y);
}

// Settings is the tallest page; it hides the shared bottom strip + dots (see
// drawStaticFrame) and spreads its rows across the full reclaimed height with
// 17px spacing so every row sits inside its card and nothing tucks under the
// bottom bar. Keep these Y's in sync with updateSettings() below.
static void drawStaticSettings() {
    drawCard(4, 22, 162, 84, "WHEEL PROFILE");
    drawSettingLabel("PROFILE", 40, SET_PROFILE);
    drawRowLabel("DIAMETER", 57);
    drawRowLabel("GEARING",  74);
    drawRowLabel("POLES",    91);

    drawCard(4, 110, 162, 84, "DISPLAY");
    drawSettingLabel("UNITS",      128, SET_UNITS);
    drawSettingLabel("DEMO",       145, SET_DEMO);
    drawSettingLabel("BRIGHTNESS", 162, SET_BRIGHT);
    drawSettingLabel("THEME",      179, SET_THEME);

    drawCard(4, 198, 162, 84, "BATTERY");
    drawSettingLabel("CELLS",   216, SET_CELLS);
    drawSettingLabel("PACK AH", 233, SET_PACK_AH);
    drawSettingLabel("STOP/C",  250, SET_STOP_CELL);
    drawSettingLabel("WH/MI",   267, SET_WHMI);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("L: select  R: change", X0 + UI_W / 2, 288);
}

// Human-readable last-reset cause for the SYSTEM page.
static const char* resetReasonStr() {
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
static void drawStaticSystem() {
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

    // Footer spread into the reclaimed bottom strip (SYSTEM hides cells+dots) so
    // the version line clears everything instead of colliding with the battery bar.
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(String("reset: ") + resetReasonStr(), X0 + UI_W / 2, 258);
    GFX->drawString(String("canvas: ") + (gCanvasPsram ? "PSRAM" : "SRAM"), X0 + UI_W / 2, 272);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString(FW_VERSION_FULL, X0 + UI_W / 2, 286);
}

// ==========================================
// PAGE 0: BIG HUD
// Minimal riding screen with the important stuff huge and readable.
// ==========================================
static void drawStaticHud() {
    // Intentionally empty: the Big HUD uses the full ride area.
    // No footer title/page chrome here so the speed and main metrics can breathe.
}

static void drawHudSmallMetric(int x, int y, int w, const char* label, String value, uint16_t color) {
    GFX->drawRect(X0 + x, y, w, 44, COL_BORDER);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(label, X0 + x + w / 2, y + 5);

    // Auto-fit: drop to the smaller Bebas if the value would overflow the tile.
    GFX->setFont(&BebasNeue24pt7b);
    if (GFX->textWidth(value) > w - 6) GFX->setFont(&BebasNeue18pt7b);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(color);
    GFX->drawString(value, X0 + x + w / 2, y + 29);

    GFX->setFont(&fonts::Font0);
}

static void updateHud() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 200) return;
    lastMs = millis();

    // The HUD owns the whole panel below the status bar (no mid-screen cell strip
    // or page dots); the shared bottom status bar still lives at y>=298.
    GFX->fillRect(X0, 18, UI_W, 280, COL_BG);

    int spdInt = (int)round(useMph ? currentSpeedMph : currentSpeedKmh);

    // Hero speed: native BebasNeue110 (~79px digits) drawn at scale 1.0 — crisp,
    // no fractional scaling. Centered vertically between the status bar (~y16) and
    // the unit label (~y116). LOWER hudSpeedY to move the number UP, raise it to
    // drop it toward the label.
    int hudSpeedY = 10;
    GFX->setFont(&BebasNeue110pt7b);
    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString(String(spdInt), X0 + UI_W / 2, hudSpeedY);

    GFX->setFont(&BebasNeue24pt7b);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString(useMph ? "MPH" : "KM/H", X0 + UI_W / 2, 128);

    GFX->drawFastHLine(X0 + 8, 146, UI_W - 16, COL_BORDER);   // separator

    // Big battery cells (dynamic to the configured cell count) + percent. The
    // secondary cluster is pushed toward the bottom to fill the space under the
    // hero speed instead of leaving it empty.
    drawBatteryCellsRow(150, 18, true);
    GFX->setFont(&BebasNeue34pt7b);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString(String(currentBatteryPercent) + "%", X0 + UI_W / 2, 184);

    // Four key ride tiles (2x2), sitting just above the bottom status bar.
    int watts = (int)round(max(0.0f, peakWatts));
    float rangeDisplay = useMph ? remainingRangeKm * 0.621371f : remainingRangeKm;
    String rangeUnit = useMph ? "mi" : "km";
    float hottest = max(currentMotorTemp, currentEscTemp);

    drawHudSmallMetric(4,  202, 78, "WATTS", String(watts), wattColor(watts));
    drawHudSmallMetric(88, 202, 78, "VOLTS", String(currentVoltage, 1), battColor(currentBatteryPercent));
    drawHudSmallMetric(4,  250, 78, "RANGE", String(rangeDisplay, 1) + rangeUnit, COL_WHITE);
    drawHudSmallMetric(88, 250, 78, "TEMP", String((int)round(hottest)) + "C",
                       hottest > MOTOR_TEMP_LIMIT ? COL_RED : COL_GREEN);

    markDirty(18, 280);
}

// ==========================================
// PAGE 6: REALTIME GRAPHS
// Four mini line graphs fed by the 3-minute RAM history buffer.
// ==========================================
enum GraphField {
    GF_SPEED,
    GF_WATTS,
    GF_VOLTS,
    GF_MOTOR_TEMP
};

static float graphValue(GraphField f, const TelemetrySample& s) {
    switch (f) {
        case GF_SPEED:      return useMph ? s.speedKmh * 0.621371f : s.speedKmh;
        case GF_WATTS:      return max(0.0f, s.watts);
        case GF_VOLTS:      return s.volts;
        case GF_MOTOR_TEMP: return s.motorTemp;
        default:            return 0;
    }
}

static void graphRange(GraphField f, float& mn, float& mx) {
    switch (f) {
        case GF_SPEED:
            mn = 0;
            mx = useMph ? 40 : 65;
            break;
        case GF_WATTS:
            mn = 0;
            mx = max(3000.0f, maxWattsSession * 1.15f);
            break;
        case GF_VOLTS:
            mn = BATTERY_MIN_V;
            mx = BATTERY_MAX_V;
            break;
        case GF_MOTOR_TEMP:
            mn = 20;
            mx = 100;
            break;
    }
}

static String graphCurrentText(GraphField f) {
    switch (f) {
        case GF_SPEED:
            return String((int)round(useMph ? currentSpeedMph : currentSpeedKmh)) + (useMph ? " mph" : " kmh");
        case GF_WATTS:
            return String((int)round(max(0.0f, currentWatts))) + " W";
        case GF_VOLTS:
            return String(currentVoltage, 1) + " V";
        case GF_MOTOR_TEMP:
            return String((int)round(currentMotorTemp)) + "C";
        default:
            return "";
    }
}

static uint16_t graphColor(GraphField f) {
    switch (f) {
        case GF_SPEED:      return COL_ACCENT;
        case GF_WATTS:      return wattColor((int)round(max(0.0f, currentWatts)));
        case GF_VOLTS:      return battColor(currentBatteryPercent);
        case GF_MOTOR_TEMP: return currentMotorTemp > MOTOR_TEMP_LIMIT ? COL_RED : COL_GREEN;
        default:            return COL_WHITE;
    }
}

static const char* graphLabel(GraphField f) {
    switch (f) {
        case GF_SPEED:      return "SPEED";
        case GF_WATTS:      return "WATTS";
        case GF_VOLTS:      return "VOLTS";
        case GF_MOTOR_TEMP: return "MOTOR TEMP";
        default:            return "";
    }
}

static const char* trendSymbol(GraphField f) {
    if (histCount < 12) return "-";

    TelemetrySample newer = getHistorySample(histCount - 1);
    TelemetrySample older = getHistorySample(histCount - 10);

    float delta = graphValue(f, newer) - graphValue(f, older);

    if (delta > 0.8f) return "^";
    if (delta < -0.8f) return "v";
    return "-";
}

static void drawMiniGraph(int x, int y, int w, int h, GraphField field) {
    float mn, mx;
    graphRange(field, mn, mx);
    if (mx <= mn) mx = mn + 1;

    GFX->fillRect(X0 + x, y, w, h, COL_BG);
    GFX->drawRect(X0 + x, y, w, h, COL_BORDER);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(graphLabel(field), X0 + x + 4, y + 3);

    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(graphColor(field));
    GFX->drawString(String(trendSymbol(field)) + " " + graphCurrentText(field), X0 + x + w - 4, y + 3);

    if (histCount < 2) {
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_DIM);
        GFX->drawString("waiting data", X0 + x + w / 2, y + h / 2);
        return;
    }

    int gx = X0 + x + 4;
    int gy = y + 17;
    int gw = w - 8;
    int gh = h - 21;

    // Midline guide.
    GFX->drawFastHLine(gx, gy + gh / 2, gw, COL_BORDER);

    int lastX = 0, lastY = 0;
    bool haveLast = false;

    for (int i = 0; i < histCount; i++) {
        TelemetrySample s = getHistorySample(i);
        float v = graphValue(field, s);
        v = constrain(v, mn, mx);

        int px = gx + (i * (gw - 1)) / max(1, histCount - 1);
        int py = gy + gh - 1 - (int)((v - mn) * (gh - 1) / (mx - mn));

        if (haveLast) {
            GFX->drawLine(lastX, lastY, px, py, graphColor(field));
        }

        lastX = px;
        lastY = py;
        haveLast = true;
    }
}

static void drawStaticGraphs() {
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("3 MIN HISTORY", X0 + UI_W / 2, 262);
}

static void updateGraphs() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 500) return;
    lastMs = millis();

    drawMiniGraph(4, 22, 162, 56, GF_SPEED);
    drawMiniGraph(4, 82, 162, 56, GF_WATTS);
    drawMiniGraph(4, 142, 162, 56, GF_VOLTS);
    drawMiniGraph(4, 202, 162, 56, GF_MOTOR_TEMP);

    markDirty(22, 240);
}

// ==========================================
// PAGE 7: SAVED RIDE LOGS
// Shows the latest compact ride summaries stored in Preferences.
// ==========================================
static void drawStaticLogs() {
    drawCard(4, 22, 162, 240, "RIDE LOGS");

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("saved on trip reset", X0 + UI_W / 2, 268);
}

// Detailed-log (LittleFS) status line near the bottom of the LOGS card: a
// low-space failsafe warning, the off state, or free space. Bottom-aligned so it
// never collides with the ride summaries above it.
static void drawLogStatus(int y) {
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    if (ridelogFull()) {
        GFX->setTextColor(COL_RED);
        GFX->drawString("! LOG STORAGE FULL", X0 + UI_W / 2, y);
    } else if (!ridelogEnabled()) {
        GFX->setTextColor(COL_YELLOW);
        GFX->drawString("DETAIL LOGGING OFF", X0 + UI_W / 2, y);
    } else {
        GFX->setTextColor(COL_DIM);
        GFX->drawString("log: " + String(ridelogFreeBytes() / 1024) + " KB free", X0 + UI_W / 2, y);
    }
    GFX->setTextDatum(TL_DATUM);   // restore for any following list draws
}

static void updateLogs() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 1000) return;
    lastMs = millis();

    GFX->fillRect(X0 + 8, 40, UI_W - 16, 220, COL_BG);

    uint8_t head = prefs.getUChar("logHead", 0);
    uint8_t count = prefs.getUChar("logCount", 0);

    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TL_DATUM);

    if (count == 0) {
        GFX->setTextColor(COL_DIM);
        GFX->drawString("No saved rides yet.", X0 + 14, 48);
        drawLogStatus(252);
        markDirty(40, 222);
        return;
    }

    int rows = min((int)count, 5);

    for (int r = 0; r < rows; r++) {
        int idx = (head - 1 - r + RIDE_LOG_MAX) % RIDE_LOG_MAX;

        char key[12];
        snprintf(key, sizeof(key), "ride%02u", (unsigned int)idx);

        RideLog log;
        size_t got = prefs.getBytes(key, &log, sizeof(log));
        if (got != sizeof(log)) continue;

        float dist = useMph ? log.distanceKm * 0.621371f : log.distanceKm;
        float maxSpd = useMph ? log.maxSpeedKmh * 0.621371f : log.maxSpeedKmh;
        float whPerDist = 0;
        if (dist > 0.01f) whPerDist = log.whUsed / dist;

        int y = 42 + r * 40;

        GFX->setTextColor(COL_ACCENT);
        GFX->drawString("#" + String(r + 1), X0 + 12, y);

        GFX->setTextColor(COL_WHITE);
        GFX->drawString(String(dist, 1) + (useMph ? " mi" : " km"), X0 + 34, y);

        GFX->setTextColor(COL_DIM);
        GFX->drawString("max " + String((int)round(maxSpd)) + (useMph ? " mph" : " kmh"), X0 + 12, y + 12);
        GFX->drawString(String((int)round(whPerDist)) + (useMph ? " Wh/mi" : " Wh/km") +
                        "  " + String((int)round(log.maxWatts)) + "W", X0 + 12, y + 24);
    }

    drawLogStatus(252);
    markDirty(40, 222);
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

    if (currentPage == PAGE_HUD)           drawStaticHud();
    else if (currentPage == PAGE_DASH)     drawStaticDash();
    else if (currentPage == PAGE_POWER)    drawStaticPower();
    else if (currentPage == PAGE_TRIP)     drawStaticTrip();
    else if (currentPage == PAGE_SETTINGS) drawStaticSettings();
    else if (currentPage == PAGE_SYSTEM)   drawStaticSystem();
    else if (currentPage == PAGE_GRAPHS)   drawStaticGraphs();
    else if (currentPage == PAGE_LOGS)     drawStaticLogs();

    // ── BATTERY CELLS OUTLINE (common, y=276..288) ──
    // Hidden on the Big HUD (it has its own larger cell row) and on SETTINGS
    // (the menu is tall — reclaim this strip so the last rows don't tuck under
    // the bottom chrome). The bottom status bar still shows battery % there.
    bool ownsFullHeight = (currentPage == PAGE_HUD || currentPage == PAGE_SETTINGS ||
                           currentPage == PAGE_SYSTEM);
    if (!ownsFullHeight) {
        drawBatteryCellsRow(276, 12, false);
    }

    // ── PAGE INDICATOR DOTS (in the gap between cells and bottom bar) ──
    // Hidden on the pages that use the reclaimed vertical space (HUD, SETTINGS).
    if (!ownsFullHeight) {
        int dotGap = 8;
        int dotsW = (PAGE_COUNT - 1) * dotGap;
        int dotX0 = X0 + UI_W / 2 - dotsW / 2;
        for (int i = 0; i < PAGE_COUNT; i++) {
            int dx = dotX0 + i * dotGap;
            if (i == currentPage) GFX->fillCircle(dx, 293, 2, COL_ACCENT);
            else                  GFX->drawCircle(dx, 293, 2, COL_BORDER);
        }
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
static void updateSpeed() {
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
static void drawStat(int cx, String value, const char* unit, uint16_t vcol) {
    const int cy = 103;   // vertical middle of the panel (86 + 32/2), nudged
    const int g = 3;
    GFX->setFont(&BebasNeue34pt7b);
    int wn = GFX->textWidth(value);
    GFX->setFont(&BebasNeue18pt7b);
    int wu = GFX->textWidth(unit);
    int gx = cx - (wn + g + wu) / 2;

    GFX->setTextDatum(ML_DATUM);
    GFX->setFont(&BebasNeue34pt7b);
    GFX->setTextColor(vcol);
    GFX->drawString(value, gx, cy);
    GFX->setFont(&BebasNeue18pt7b);
    GFX->setTextColor(COL_DIM);
    GFX->drawString(unit, gx + wn + g, cy);
}

// ==========================================
// UPDATE: Volts | Watts panel
// ==========================================
static void updateStatPanel() {
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
static void drawTempRow(int y, float temp, int pct, bool hot) {
    GFX->setFont(&fonts::Font0);
    String pstr = String("(") + pct + "%)";
    String tstr = String((int)round(temp)) + "C";

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
static void updateTemps() {
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
static void updateRange() {
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
    static int lastCells = -1;

    // No shared bottom strip on the pages that reclaim it (HUD has its own larger
    // cell row; SETTINGS/SYSTEM use the space for their content).
    if (currentPage == PAGE_HUD || currentPage == PAGE_SETTINGS || currentPage == PAGE_SYSTEM) {
        lastPct = -1;
        lastCells = -1;
        return;
    }

    if (currentBatteryPercent != lastPct || BATTERY_CELLS_COUNT != lastCells || gRedrawAll) {
        drawBatteryCellsRow(276, 12, true);
        lastPct = currentBatteryPercent;
        lastCells = BATTERY_CELLS_COUNT;
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
static void drawVal(int y, String value, uint16_t color) {
    GFX->fillRect(X0 + 78, y - 1, 84, 12, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(TR_DATUM);
    GFX->setTextColor(color);
    GFX->drawString(value, X0 + 158, y);
}

// ==========================================
// UPDATE: Page 1 (POWER / ENERGY / SPEED)
// ==========================================
static void updatePower() {
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
static void updateTrip() {
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
static void updateSettings() {
    static unsigned long lastMs = 0;
    if (!gRedrawAll && millis() - lastMs < 400) return;
    lastMs = millis();

    // Y's mirror drawStaticSettings() above (17px spacing, 4/4/4 cards).
    WheelProfile &w = wheelProfiles[activeWheelProfile];
    drawVal(40,  String(w.name), COL_WHITE);
    drawVal(57,  String((int)round(w.wheelDiameterM * 1000)) + "mm", COL_WHITE);
    drawVal(74,  String(w.motorPulley) + ":" + String(w.wheelPulley), COL_WHITE);
    drawVal(91,  String((int)w.polePairs), COL_WHITE);

    drawVal(128, String(useMph ? "MPH" : "KM/H"), COL_WHITE);
    drawVal(145, String(gDemoMode ? "ON" : "OFF"), gDemoMode ? COL_YELLOW : COL_WHITE);
    drawVal(162, String(gBrightnessPct) + "%", COL_WHITE);
    drawVal(179, String(THEMES[gThemeIdx].name), COL_ACCENT);

    drawVal(216, String(BATTERY_CELLS_COUNT) + "S", COL_WHITE);
    drawVal(233, String(BATTERY_EFFECTIVE_CAPACITY_AH, 1) + "Ah", COL_WHITE);
    drawVal(250, String(BATTERY_STOP_CELL_V, 2) + "V", COL_WHITE);
    drawVal(267, String((int)round(RANGE_DEFAULT_WH_PER_MILE)), COL_WHITE);

    markDirty(22, 274);
}

// ==========================================
// UPDATE: Page 4 (SYSTEM INFO) — live ESP32 board stats
// ==========================================
static void updateSystem() {
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
    drawVal(192, String(t, 1) + "C", t > 70 ? COL_ORANGE : COL_WHITE);

    unsigned long up = millis() / 1000;
    char ub[12];
    snprintf(ub, sizeof(ub), "%02lu:%02lu:%02lu", up / 3600, (up / 60) % 60, up % 60);
    drawVal(208, String(ub), COL_WHITE);

    drawVal(224, String(gFps) + "f " + String(gLastPushUs / 1000) + "ms",
            gFps >= 30 ? COL_GREEN : COL_WHITE);
    markDirty(22, 210);
}

void updateCurrentPageContent() {
    if (currentPage == PAGE_HUD) {
        updateHud();
    } else if (currentPage == PAGE_DASH) {
        updateSpeed();
        updateStatPanel();
        updateTemps();
        updateRange();
    } else if (currentPage == PAGE_POWER) {
        updatePower();
    } else if (currentPage == PAGE_TRIP) {
        updateTrip();
    } else if (currentPage == PAGE_SETTINGS) {
        updateSettings();
    } else if (currentPage == PAGE_SYSTEM) {
        updateSystem();
    } else if (currentPage == PAGE_GRAPHS) {
        updateGraphs();
    } else if (currentPage == PAGE_LOGS) {
        updateLogs();
    }
}

// ==========================================
// SAFETY: alerts + overlay banners
// ==========================================
static const char* faultName(int f) {
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
    if (systemMode == MODE_BRIDGE_CONFIRM) return 5;
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
                ? "MOTOR " + String((int)round(currentMotorTemp)) + "C"
                : "ESC " + String((int)round(currentEscTemp)) + "C";
    } else if (state == 5) {
        line1 = "START BRIDGE?";
        line2 = "L=YES    R=NO";
    } else {
        line1 = "LOW BATTERY";
        line2 = "STOP & CHARGE";
        line3 = String(currentBatteryPercent) + "%   " + String(currentVoltage, 1) + " V";
    }

    String textKey = line1 + "|" + line2 + "|" + line3;
    if (state != lastState || textKey != lastText || gRedrawAll) {
        uint16_t bgColor = (state == 5) ? COL_ACCENT : COL_RED;
        uint16_t fgColor = (state == 5) ? COL_BG : COL_WHITE;
        GFX->fillRect(X0 + 8, by, UI_W - 16, bh, bgColor);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(fgColor);
        if (crit) {
            // LOW BATTERY: big line1 in Bebas40, line2 in Bebas24, line3 (pct/V) in Bebas18
            GFX->setFont(&BebasNeue40pt7b);
            GFX->drawString(line1, X0 + UI_W / 2, by + 10);
            GFX->setFont(&BebasNeue24pt7b);
            GFX->drawString(line2, X0 + UI_W / 2, by + 52);
            GFX->setFont(&BebasNeue18pt7b);
            GFX->drawString(line3, X0 + UI_W / 2, by + 76);
        } else if (state == 5) {
            // BRIDGE CONFIRM: already Bebas
            GFX->setFont(&BebasNeue24pt7b);
            GFX->drawString(line1, X0 + UI_W / 2, by + 12);
            GFX->setFont(&BebasNeue18pt7b);
            GFX->drawString(line2, X0 + UI_W / 2, by + 46);
        } else {
            // FAULT / LINK LOST / HOT: line1 big, line2 medium
            GFX->setFont(&BebasNeue40pt7b);
            GFX->drawString(line1, X0 + UI_W / 2, by + 14);
            GFX->setFont(&BebasNeue24pt7b);
            GFX->drawString(line2, X0 + UI_W / 2, by + 48);
        }
        lastText = textKey;
        markDirty(by, bh);
    }
    lastState = state;
}
