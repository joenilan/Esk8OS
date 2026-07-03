#include "app/App.h"
#include "esk8os.h"
#include "ui/ui.h"
#include "ui/UiRenderer.h"
#if ESK8OS_FULL_UI
#include "ui/BebasNeue18.h"
#include "ui/BebasNeue24.h"
#endif
#include "board/BoardLilyGoTDisplayS3.h"
#include "board/StatusLed.h"
#include "services/bridge.h"
#include "services/companion_ble.h"
#include "services/webexport.h"
#include "util/console.h"
#include "transports/VescUartTransport.h"
#include "telemetry/telemetry.h"
#include "logging/sessionlog.h"

// Variables from main.cpp
bool lastLeftBtn = HIGH, lastRightBtn = HIGH;
unsigned long lastLeftPress = 0, lastRightPress = 0;
unsigned long gLastInteractionMs = 0;   // last button activity — keeps the screensaver awake

int currentPage = PAGE_HUD;
SystemMode systemMode = MODE_DASHBOARD;
int settingsCursor = 0;
bool settingsEditing = false;   // SETTINGS: selected row is in edit mode (taps change its value)

unsigned long lastDataPoll = 0;

void applyBrightness() {
    Esk8OS::Board::setBacklight((uint8_t)(gBrightnessPct * 255 / 100));
    Esk8OS::UiRenderer::applyDisplayBrightness();
}

static void toggleUnits() {
    useMph = !useMph;
    prefs.putBool("mph", useMph);
    drawStaticFrame();
    gRedrawAll = true;
}

static void cycleHudFace() {
    if (gHudFace == HUD_FACE_SPEED) {
        gHudFace = HUD_FACE_BATTERY;
        gBatteryFocus = BATTERY_FOCUS_PERCENT;
    } else if (gHudFace == HUD_FACE_BATTERY && gBatteryFocus == BATTERY_FOCUS_PERCENT) {
        gBatteryFocus = BATTERY_FOCUS_VOLTS;
    } else if (gHudFace == HUD_FACE_BATTERY) {
        gHudFace = HUD_FACE_WATTS;
    } else if (gHudFace == HUD_FACE_WATTS) {
        gHudFace = HUD_FACE_SAFETY;
    } else {
        gHudFace = HUD_FACE_SPEED;
        gBatteryFocus = BATTERY_FOCUS_PERCENT;
    }
    prefs.putInt("hudFace", gHudFace);
    prefs.putInt("batFocus", gBatteryFocus);
    drawStaticFrame();
    gRedrawAll = true;
    if (gHudFace == HUD_FACE_BATTERY && gBatteryFocus == BATTERY_FOCUS_VOLTS) showToast("HUD VOLTS");
    else if (gHudFace == HUD_FACE_BATTERY) showToast("HUD BATTERY");
    else if (gHudFace == HUD_FACE_WATTS) showToast("HUD WATTS");
    else if (gHudFace == HUD_FACE_SAFETY) showToast("HUD SAFETY");
    else showToast("HUD SPEED");
}

// ---- Settings editing + page-order helpers (used by checkButtons) ----

// Custom page order for LEFT/RIGHT paging: ride/glance pages first, config last,
// so paging at a stop stays on useful screens and you reach config deliberately.
static const int PAGE_ORDER[] = {
    PAGE_HUD, PAGE_DASH, PAGE_POWER, PAGE_TRIP, PAGE_GRAPHS,
    PAGE_SETTINGS, PAGE_SYSTEM, PAGE_LOGS,
};
static const int PAGE_ORDER_N = sizeof(PAGE_ORDER) / sizeof(PAGE_ORDER[0]);

static int pageOrderIndex() {
    for (int i = 0; i < PAGE_ORDER_N; i++)
        if (PAGE_ORDER[i] == currentPage) return i;
    return 0;
}

// Step to the previous/next page in PAGE_ORDER (dir = -1 / +1), wrapping around.
static void gotoPageRel(int dir) {
    int i = (pageOrderIndex() + dir + PAGE_ORDER_N) % PAGE_ORDER_N;
    currentPage = PAGE_ORDER[i];
    settingsCursor = 0;
    settingsEditing = false;
    drawStaticFrame();
    gRedrawAll = true;
}

// Change the selected Settings value by one step. dir = +1 (up) / -1 (down). Binary
// rows just toggle; numeric rows wrap (matches the old increment-only behavior).
static void changeSetting(int dir) {
    switch (settingsCursor) {
        case SET_PROFILE:
            activeWheelProfile = (activeWheelProfile + 1) % 2;
            prefs.putInt("wheelprof", activeWheelProfile);
            gWheelDiameterMm = 0;                 // new preset -> drop the size override
            prefs.putInt("wheelmm", 0);
            break;
        case SET_UNITS:
            useMph = !useMph;
            prefs.putBool("mph", useMph);
            break;
        case SET_DEMO:
            gDemoMode = !gDemoMode;
            prefs.putBool("demo", gDemoMode);
            drawStaticFrame();   // the DEMO MODE badge lives in the static top bar
            rideEnergyBaselineSet = false;
            if (!gDemoMode) {
                lastVescOkMs = 0;
                vescLinkOk = false;
                telemetryLive = false;
                currentSpeedKmh = 0.0f;
                currentSpeedMph = 0.0f;
                currentAmps = 0.0f;
                currentMotorAmps = 0.0f;
                currentDuty = 0.0f;
                currentWatts = 0.0f;
                peakWatts = 0.0f;
                gPpmConnected = false;
                gPpmDecoded = 0.0f;
                gPpmPulseMs = 0.0f;
            }
            break;
        case SET_BRIGHT: {
            const int STEPS[] = { 25, 50, 75, 100 };
            int i = 0;
            while (i < 4 && STEPS[i] != gBrightnessPct) i++;
            gBrightnessPct = STEPS[(i + dir + 4) % 4];
            prefs.putInt("bright", gBrightnessPct);
            applyBrightness();
            break;
        }
        case SET_THEME:
            gThemeIdx = (gThemeIdx + dir + THEME_COUNT) % THEME_COUNT;
            prefs.putInt("theme", gThemeIdx);
            applyTheme(gThemeIdx);
            drawStaticFrame();
            break;
        case SET_HUD_FACE:
            gHudFace = (gHudFace + dir + HUD_FACE_COUNT) % HUD_FACE_COUNT;
            prefs.putInt("hudFace", gHudFace);
            drawStaticFrame();
            break;
        case SET_BATT_FOCUS:
            gBatteryFocus = (gBatteryFocus + dir + BATTERY_FOCUS_COUNT) % BATTERY_FOCUS_COUNT;
            prefs.putInt("batFocus", gBatteryFocus);
            drawStaticFrame();
            break;
        case SET_CELLS:
            BATTERY_CELLS_COUNT += dir;
            if (BATTERY_CELLS_COUNT > 14) BATTERY_CELLS_COUNT = 6;
            if (BATTERY_CELLS_COUNT < 6)  BATTERY_CELLS_COUNT = 14;
            prefs.putInt("cells", BATTERY_CELLS_COUNT);
            recalcBatteryBounds();
            currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
            minVoltageSession = BATTERY_MAX_V;
            minVoltageUnderLoadSession = BATTERY_MAX_V;
            loadedCellVoltage = currentVoltage / max(1, BATTERY_CELLS_COUNT);
            updateRangeEstimate();
            drawStaticFrame();
            break;
        case SET_PACK_AH:
            BATTERY_EFFECTIVE_CAPACITY_AH += dir * 0.5f;
            if (BATTERY_EFFECTIVE_CAPACITY_AH > 40.0f) BATTERY_EFFECTIVE_CAPACITY_AH = 4.0f;
            if (BATTERY_EFFECTIVE_CAPACITY_AH < 4.0f)  BATTERY_EFFECTIVE_CAPACITY_AH = 40.0f;
            prefs.putFloat("packAh", BATTERY_EFFECTIVE_CAPACITY_AH);
            updateRangeEstimate();
            break;
        case SET_STOP_CELL:
            BATTERY_STOP_CELL_V += dir * 0.05f;
            if (BATTERY_STOP_CELL_V > 3.60f) BATTERY_STOP_CELL_V = 3.00f;
            if (BATTERY_STOP_CELL_V < 3.00f) BATTERY_STOP_CELL_V = 3.60f;
            prefs.putFloat("stopCell", BATTERY_STOP_CELL_V);
            recalcBatteryBounds();
            currentVoltage = constrain(currentVoltage, BATTERY_MIN_V, BATTERY_MAX_V);
            loadedCellVoltage = currentVoltage / max(1, BATTERY_CELLS_COUNT);
            updateRangeEstimate();
            drawStaticFrame();
            break;
        case SET_WHMI:
            RANGE_DEFAULT_WH_PER_MILE += dir * 0.1f;
            if (RANGE_DEFAULT_WH_PER_MILE > 40.0f) RANGE_DEFAULT_WH_PER_MILE = 14.0f;
            if (RANGE_DEFAULT_WH_PER_MILE < 14.0f) RANGE_DEFAULT_WH_PER_MILE = 40.0f;
            prefs.putFloat("whmi", RANGE_DEFAULT_WH_PER_MILE);
            updateRangeEstimate();
            break;
    }
    gRedrawAll = true;
}

// Defined in main.cpp, used by App
void pollVescData();

namespace Esk8OS {
namespace App {

// BLE PAGE_NEXT/PREV step through the same PAGE_ORDER the buttons use, so the
// app's remote paging visits pages in the order the rider knows.
void pageRel(int dir) { gotoPageRel(dir); }

// Zero the current ride/trip metrics and repaint. Shared by the LEFT long-press and
// the companion app's TRIP_RESET command — both run on the UI thread.
void resetTrip() {
    saveRideSummaryLog();
    tripDistanceKm = 0;
    tripMovingSec = 0;
    sessionMovingStartSec = 0;
    lastMovedMs = 0;
    sessionTripStartKm = 0;
    rideStartMs = millis();
    avgSpeedKmh = 0;
    maxSpeedKmh = 0;
    currentWattHours = 0;
    currentWhRegen = 0;
    avgWhPerKm = 0;
    estimatedRangeKm = 0;
    remainingRangeKm = 0;
    estimatedLimpRangeKm = 0;
    remainingLimpRangeKm = 0;
    rangeEstimateReady = false;
    rideEnergyBaselineSet = false;
    maxWattsSession = 0;
    minVoltageSession = BATTERY_MAX_V;
    minVoltageUnderLoadSession = BATTERY_MAX_V;
    maxMotorAmpsSession = 0;
    maxBatteryAmpsSession = 0;
    homeVoltageSecondsSession = 0;
    limpVoltageSecondsSession = 0;
    sagEventsSession = 0;
    rangeAlertState = 0;
    if (DEMO_DATA) {
        currentBatteryPercent = 100;
        currentVoltage = BATTERY_MAX_V;
        currentMotorTemp = 0;
        currentEscTemp = 0;
        currentBatteryTemp = 0;
        peakWatts = 0;
    }
    saveOdo();
    drawStaticFrame();
    gRedrawAll = true;
    showToast(DEMO_DATA ? "RECHARGED" : "TRIP RESET");
}

void checkButtons() {
    bool left = Esk8OS::Board::buttonA() ? LOW : HIGH;
    bool right = Esk8OS::Board::buttonB() ? LOW : HIGH;
    unsigned long now = millis();
    if (left == LOW || right == LOW) gLastInteractionMs = now;  // any press wakes the screensaver

    // ---- BOTH held 2s: enter/exit bridge mode (suppresses single actions) ----
    static unsigned long bothDownAt = 0;
    static bool bothHandled = false;
    static bool lockSingles = false;
    if (left == LOW && right == LOW) {
        lockSingles = true;
        if (bothDownAt == 0) { bothDownAt = millis(); bothHandled = false; }
        if (!bothHandled && millis() - bothDownAt > 2000) {
            bothHandled = true;
            if (systemMode == MODE_DASHBOARD) {
                systemMode = MODE_BRIDGE_CONFIRM;
                gRedrawAll = true;
            } else if (systemMode == MODE_VESC_BRIDGE) {
                exitBridgeMode();
            }
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

    // BRIDGE CONFIRMATION logic
    if (systemMode == MODE_BRIDGE_CONFIRM) {
        if (left == LOW && lastLeftBtn == HIGH) {
            enterBridgeMode();
        } else if (right == LOW && lastRightBtn == HIGH) {
            systemMode = MODE_DASHBOARD;
            gRedrawAll = true;
        }
        lastLeftBtn = left; lastRightBtn = right;
        return;
    }

    // WIFI CONFIRMATION: the app requested the log/OTA AP over BLE; the AP only
    // rises after a physical L press here, so a nearby attacker with a BLE radio
    // can't raise it unattended. R (or the 30s timeout in dashboardLoop) cancels.
    if (systemMode == MODE_WIFI_CONFIRM) {
        if (left == LOW && lastLeftBtn == HIGH) {
            systemMode = MODE_DASHBOARD;
            webServiceStart();
            showToast("WIFI EXPORT");
            drawStaticFrame();
            gRedrawAll = true;
        } else if (right == LOW && lastRightBtn == HIGH) {
            systemMode = MODE_DASHBOARD;
            drawStaticFrame();
            gRedrawAll = true;
        }
        lastLeftBtn = left; lastRightBtn = right;
        return;
    }

    // TRIP RESET CONFIRMATION: a fresh L press confirms, R cancels. Requires the
    // hold-L to be released first (needs a HIGH->LOW edge), so it can't auto-confirm.
    if (systemMode == MODE_TRIP_RESET_CONFIRM) {
        if (left == LOW && lastLeftBtn == HIGH) {
            systemMode = MODE_DASHBOARD;
            resetTrip();                 // repaints + toasts on its own
        } else if (right == LOW && lastRightBtn == HIGH) {
            systemMode = MODE_DASHBOARD;
            drawStaticFrame();
            gRedrawAll = true;
        }
        lastLeftBtn = left; lastRightBtn = right;
        return;
    }

    // ============================================================================
    // Two-button nav: taps = navigation, holds = the contextual action.
    //   tap LEFT/RIGHT = previous / next page
    //   hold LEFT = MPH<->KM/H        hold RIGHT = HUD cycle face | TRIP reset
    //   SETTINGS: tap = move cursor (spills to neighbor page at the ends);
    //             hold = enter/exit EDIT mode; while editing, tap -/+ changes value.
    // Taps fire on release so a press can still become a hold.
    // ============================================================================
    const unsigned long HOLD_MS = 400;

    // ---- LEFT ----
    static unsigned long leftDownAt = 0;
    static int leftHoldCount = 0;
    if (left == LOW) {
        if (lastLeftBtn == HIGH) { leftDownAt = now; leftHoldCount = 0; }
        if (now - leftDownAt >= HOLD_MS && leftHoldCount == 0) {            // hold
            if (currentPage == PAGE_SETTINGS) { settingsEditing = !settingsEditing; drawStaticFrame(); gRedrawAll = true; }  // redraw static chrome so the edit-mode highlight + footer actually repaint
            else                                toggleUnits();             // MPH <-> KM/H
            leftHoldCount++;
        }
    } else if (lastLeftBtn == LOW && leftHoldCount == 0 && now - leftDownAt >= 30) {  // tap
        if (currentPage == PAGE_SETTINGS) {
            if (settingsEditing)           changeSetting(-1);              // value down
            else if (settingsCursor > 0) { settingsCursor--; drawStaticFrame(); gRedrawAll = true; }
            else                           gotoPageRel(-1);                // spill to prev page
        } else {
            gotoPageRel(-1);                                               // previous page
        }
    }
    lastLeftBtn = left;

    // ---- RIGHT ----
    static unsigned long rightDownAt = 0;
    static int rightHoldCount = 0;
    if (right == LOW) {
        if (lastRightBtn == HIGH) { rightDownAt = now; rightHoldCount = 0; }
        if (now - rightDownAt >= HOLD_MS && rightHoldCount == 0) {          // hold
            if (currentPage == PAGE_SETTINGS)   { settingsEditing = !settingsEditing; drawStaticFrame(); gRedrawAll = true; }  // redraw static chrome so the edit-mode highlight + footer actually repaint
            else if (currentPage == PAGE_HUD)     cycleHudFace();          // Speed/Battery/Watts/Safety
            else if (currentPage == PAGE_TRIP)  { systemMode = MODE_TRIP_RESET_CONFIRM; gRedrawAll = true; }
            rightHoldCount++;
        }
    } else if (lastRightBtn == LOW && rightHoldCount == 0 && now - rightDownAt >= 30) {  // tap
        if (currentPage == PAGE_SETTINGS) {
            if (settingsEditing)                          changeSetting(+1);   // value up
            else if (settingsCursor < SETTINGS_COUNT-1) { settingsCursor++; drawStaticFrame(); gRedrawAll = true; }
            else                                          gotoPageRel(+1);     // spill to next page
        } else {
            gotoPageRel(+1);                                                   // next page
        }
    }
    lastRightBtn = right;
}

void dashboardLoop() {
    unsigned long now = millis();

    // A confirm modal left unanswered (rider away from the board, or an app
    // request they never saw) auto-cancels after 30s instead of parking the UI
    // on the question forever.
    static unsigned long confirmSinceMs = 0;
    if (systemMode == MODE_BRIDGE_CONFIRM || systemMode == MODE_WIFI_CONFIRM ||
        systemMode == MODE_TRIP_RESET_CONFIRM) {
        if (confirmSinceMs == 0) confirmSinceMs = now;
        if (now - confirmSinceMs > 30000UL) {
            systemMode = MODE_DASHBOARD;
            drawStaticFrame();
            gRedrawAll = true;
        }
    } else {
        confirmSinceMs = 0;
    }

    if (now - lastDataPoll > 100) {
        pollVescData();
        recordHistorySample();
        sessionLogTick();
        lastDataPoll = now;
    }

    // Auto-reset the trip after 6h parked: once the board has actually moved this
    // session (lastMovedMs set) and then sits still for 6h, zero the trip so the
    // next ride starts fresh. A power-off gap can't be measured (no RTC), so a
    // cold boot instead continues from NVS; manual TRIP_RESET still zeros anytime.
    static const unsigned long TRIP_PARK_RESET_MS = 6UL * 3600UL * 1000UL;
    if (!DEMO_DATA && lastMovedMs != 0 && tripDistanceKm > 0.0f &&
        now - lastMovedMs > TRIP_PARK_RESET_MS) {
        resetTrip();
    }

    // Stream telemetry to the companion app + apply any queued settings/commands.
    companionBleTick();
    // Serve the standalone log/OTA web service (if running) without leaving the
    // dashboard — telemetry/BLE keep flowing. No-op unless WIFI_EXPORT_START ran.
    webServiceTick();

    int alert = alertState();
    bool toastUp = (long)(gToastUntil - millis()) > 0;

    // -- Dynamic Low Power Mode --
    // Dim the backlight to 10% (25/255) when battery is low to squeeze out range.
    static bool lowPowerActive = false;
    // Only dim for a genuinely low battery — not for a stale 0% left over when no
    // VESC is linked (otherwise the bench/no-ESC screen sits needlessly dim).
    bool needsLowPower = telemetryLive && (currentBatteryPercent <= 15);
    if (needsLowPower && !lowPowerActive) {
        lowPowerActive = true;
        Esk8OS::Board::setBacklight(25);
    } else if (!needsLowPower && lowPowerActive) {
        lowPowerActive = false;
        applyBrightness(); // restore user's setting
    }

#if ESK8OS_FULL_UI
    static unsigned long tftIdleSinceMs = 0;
    // Idle when sitting still — OR when there's simply no VESC linked (bench, or
    // board powered with the ESC off). alert==2 is "VESC LINK LOST"; treat that as
    // idle too so the screen still saves instead of holding a static LINK-LOST
    // banner forever. (When the link is down, speed/amps are masked to 0, so the
    // calm checks pass on their own.)
    bool tftIdle = !gDemoMode && !gOtaInProgress && rangeAlertState == 0 &&
                   currentSpeedKmh < 0.5f && fabs(currentAmps) < 1.0f &&
                   (alert == 0 || alert == 2) &&
                   (now - gLastInteractionMs > 2000);   // a recent button press keeps the screen awake
    if (tftIdle) {
        if (tftIdleSinceMs == 0) tftIdleSinceMs = now;
    } else {
        tftIdleSinceMs = 0;
    }
    bool tftScreensaverActive = tftIdleSinceMs != 0 && now - tftIdleSinceMs > 30000UL;

    // Dim the panel while the screensaver is up (like the OLED path); on wake, go
    // back to the user's brightness — or the low-power level if that's active.
    static bool tftSaverWasActive = false;
    if (tftScreensaverActive != tftSaverWasActive) {
        if (tftScreensaverActive) {
            int dim = (int)(gBrightnessPct * 255 / 100) / 4;   // ~25% of the user's brightness
            if (dim < 8) dim = 8;
            if (lowPowerActive && dim > 25) dim = 25;          // don't out-bright the low-power dim
            Esk8OS::Board::setBacklight((uint8_t)dim);
        } else if (lowPowerActive) {
            Esk8OS::Board::setBacklight(25);
        } else {
            applyBrightness();                                 // restore the user's setting
        }
        tftSaverWasActive = tftScreensaverActive;
    }

    // While the saver (or the OTA banner) owns the screen, the page draws are
    // invisible — skip them entirely instead of painting frames the saver
    // immediately fills over. On wake, repaint the static chrome the saver
    // painted over (page value draws alone don't restore card borders etc).
    bool saverOwnsScreen = tftScreensaverActive && !toastUp;
    static bool saverOwnedScreen = false;
    if (saverOwnedScreen && !saverOwnsScreen) drawStaticFrame();
    saverOwnedScreen = saverOwnsScreen;
    bool skipPageDraw = saverOwnsScreen || gOtaInProgress;
#else
    const bool skipPageDraw = false;   // OLED/headless render is driven by
                                       // updateCurrentPageContent — never skip
#endif

    if (!skipPageDraw) {
        updateClock();
        if (
#if ESK8OS_FULL_UI
            alert == 0 || gRedrawAll
#else
            true
#endif
        ) {
            updateCurrentPageContent();
        }
        updateBatteryCells();
        updateBottomBar();
        updateOverlays(alert);
    }

    // OTA via the standalone web service: show progress over the dashboard the
    // same way bridge mode does (the upload itself blocks handleClient, so this
    // mainly covers the start/finish and a stuck-failed update). 100 ms cap —
    // the progress percent doesn't change faster than that anyway.
    if (gOtaInProgress) {
#if ESK8OS_FULL_UI
        static unsigned long lastOtaPaint = 0;
        if (now - lastOtaPaint > 100 || gRedrawAll) {
            lastOtaPaint = now;
            GFX->fillRect(X0 + 8, 140, UI_W - 16, 60, COL_ACCENT);
            GFX->setTextDatum(MC_DATUM);
            GFX->setTextColor(COL_BG);
            GFX->setFont(&BebasNeue24pt7b);
            GFX->drawString("UPDATING...", X0 + UI_W / 2, 154);
            GFX->setFont(&BebasNeue18pt7b);
            GFX->drawString(String(gOtaProgressPct) + "%", X0 + UI_W / 2, 184);
            GFX->setFont(&fonts::Font0);
            markDirty(140, 60);
        }
#endif
    }

    static bool toastWasUp = false;
    if (toastUp) {
#if ESK8OS_FULL_UI
        GFX->setFont(&BebasNeue18pt7b);
        int tw = GFX->textWidth(gToastMsg) + 28;
        int tx = X0 + (UI_W - tw) / 2;
        GFX->fillRect(tx, 150, tw, 30, COL_GREEN);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_BG);
        GFX->drawString(gToastMsg, X0 + UI_W / 2, 165);
        GFX->setFont(&fonts::Font0);
        markDirty(150, 30);
#endif
    } else if (toastWasUp) {
        drawStaticFrame();
        updateCurrentPageContent();
        updateBatteryCells();
        updateBottomBar();
    }
    toastWasUp = toastUp;

#if ESK8OS_FULL_UI
    if (saverOwnsScreen) {
        Esk8OS::UiRenderer::renderTftScreensaver();
    }
#endif

    gRedrawAll = false;

    pushCanvas();   // blit only the regions that changed this frame, not the whole panel

    // FPS now tracks the dashboard loop rate (responsiveness). Counting blits would
    // read ~0 on a static page now that we only push changed regions.
    static unsigned long fpsWindow = 0;
    static uint16_t fpsCount = 0;
    fpsCount++;
    if (now - fpsWindow >= 1000) { gFps = fpsCount; fpsCount = 0; fpsWindow = now; }

    delay(5);
}

void loop() {
    consolePoll();
    checkButtons();
    Esk8OS::StatusLed::tick();   // sole tick — dashboardLoop must not double it

    if (systemMode == MODE_VESC_BRIDGE) {
        companionBleTick();
        bridgeLoop();
        delay(1);
        return;
    }

    dashboardLoop();
}

} // namespace App
} // namespace Esk8OS
