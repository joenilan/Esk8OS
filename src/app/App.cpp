#include "app/App.h"
#include "esk8os.h"
#include "ui/ui.h"
#include "ui/BebasNeue18.h"
#include "board/BoardLilyGoTDisplayS3.h"
#include "services/bridge.h"
#include "util/console.h"
#include "transports/VescUartTransport.h"
#include "transports/EspNowTransport.h"
#include "telemetry/telemetry.h"
#include "logging/ridelog.h"

// Variables from main.cpp
bool lastLeftBtn = HIGH, lastRightBtn = HIGH;
unsigned long lastLeftPress = 0, lastRightPress = 0;

int currentPage = PAGE_HUD;
SystemMode systemMode = MODE_DASHBOARD;
int settingsCursor = 0;

unsigned long lastDataPoll = 0;

void applyBrightness() { 
    Esk8OS::Board::setBacklight((uint8_t)(gBrightnessPct * 255 / 100)); 
}

// Defined in main.cpp, used by App
void pollVescData();

namespace Esk8OS {
namespace App {

void checkButtons() {
    bool left = Esk8OS::Board::buttonA() ? LOW : HIGH;
    bool right = Esk8OS::Board::buttonB() ? LOW : HIGH;

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
            if (DEMO_DATA) {
                currentBatteryPercent = 100;
                currentVoltage = BATTERY_MAX_V;
                currentMotorTemp = 0;
                currentEscTemp = 0;
                currentBatteryTemp = 0;
                peakWatts = 0;
            }
            saveOdo();
            ridelogStartRide();   
            leftHandled = true;
            drawStaticFrame();
            gRedrawAll = true;
            showToast(DEMO_DATA ? "RECHARGED" : "TRIP RESET");
        }
    } else if (lastLeftBtn == LOW && !leftHandled && millis() - leftDownAt > 30) {
        if (currentPage == PAGE_SETTINGS && settingsCursor < SETTINGS_COUNT - 1) {
            settingsCursor++;
            drawStaticFrame();                               
            gRedrawAll = true;
        } else {
            settingsCursor = 0;
            currentPage = (currentPage + 1) % PAGE_COUNT;    
            drawStaticFrame();
            gRedrawAll = true;
        }
    }
    lastLeftBtn = left;

    // RIGHT button
    if (right == LOW && lastRightBtn == HIGH && millis() - lastRightPress > 200) {
        if (currentPage == PAGE_SETTINGS) {
            switch (settingsCursor) {
                case SET_PROFILE:
                    activeWheelProfile = (activeWheelProfile + 1) % 2;
                    prefs.putInt("wheelprof", activeWheelProfile);
                    break;
                case SET_UNITS:
                    useMph = !useMph;
                    prefs.putBool("mph", useMph);
                    break;
                case SET_DEMO:
                    gDemoMode = !gDemoMode;
                    prefs.putBool("demo", gDemoMode);
                    rideEnergyBaselineSet = false;   
                    break;
                case SET_BRIGHT: {
                    int i = 0;
                    // HARDCODED BRIGHTNESS STEPS TO AVOID IMPORT ISSUES FOR NOW
                    const int BRIGHTNESS_STEPS[] = { 25, 50, 75, 100 };
                    while (i < 4 && BRIGHTNESS_STEPS[i] != gBrightnessPct) i++;
                    gBrightnessPct = BRIGHTNESS_STEPS[(i + 1) % 4];
                    prefs.putInt("bright", gBrightnessPct);
                    applyBrightness();
                    break;
                }
                case SET_THEME:
                    gThemeIdx = (gThemeIdx + 1) % THEME_COUNT;
                    prefs.putInt("theme", gThemeIdx);
                    applyTheme(gThemeIdx);     
                    drawStaticFrame();         
                    break;
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
            gRedrawAll = true;                 
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
        ridelogTick();          
        lastDataPoll = now;
    }

    int alert = alertState();
    updateClock();

    if (alert == 0 || gRedrawAll) {   
        updateCurrentPageContent();
    }

    updateBatteryCells();   
    updateBottomBar();
    updateOverlays(alert);

    gRedrawAll = false;   

    static bool toastWasUp = false;
    bool toastUp = (long)(gToastUntil - millis()) > 0;
    if (toastUp) {
        GFX->setFont(&BebasNeue18pt7b);
        int tw = GFX->textWidth(gToastMsg) + 28;
        int tx = X0 + (UI_W - tw) / 2;
        GFX->fillRect(tx, 150, tw, 30, COL_GREEN);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_BG);
        GFX->drawString(gToastMsg, X0 + UI_W / 2, 165);
        GFX->setFont(&fonts::Font0);
        markDirty(150, 30);
    } else if (toastWasUp) {
        drawStaticFrame();
        updateCurrentPageContent();
        updateBatteryCells();
        updateBottomBar();
    }
    toastWasUp = toastUp;

    if (currentPage == PAGE_SYSTEM) markDirty(0, gCanvasH);
    pushCanvasFull();

    delay(2);   
}

void loop() {
    consolePoll();      
    checkButtons();

    if (systemMode == MODE_VESC_BRIDGE) {
        bridgeLoop();
        delay(1);
        return;
    }

    dashboardLoop();
}

} // namespace App
} // namespace Esk8OS
