#include "telemetry.h"
#include "transports/VescUartTransport.h"

// ============================================================================
// Telemetry data layer. Reads the VESC over UART (or a demo simulation), then
// integrates trip/odometer + energy, maintains the range estimate, the 3-minute
// RAM history buffer, and the persisted odometer + ride-summary logs. All state
// lives in the shared globals declared in esk8os.h.
// ============================================================================

// ---- RAM history buffer -----------------------------------------------------
void recordHistorySample() {
    static unsigned long lastSampleMs = 0;
    if (millis() - lastSampleMs < 1000) return;
    lastSampleMs = millis();

    history[histHead] = {
        currentSpeedKmh,
        currentVoltage,
        currentWatts,
        currentAmps,
        currentMotorAmps,
        currentDuty,
        currentMotorTemp,
        currentEscTemp,
        currentBatteryPercent
    };

    histHead = (histHead + 1) % HIST_N;
    if (histCount < HIST_N) histCount++;
}

TelemetrySample getHistorySample(int ageIndex) {
    // ageIndex 0 = oldest sample, histCount - 1 = newest sample
    int idx = (histHead - histCount + ageIndex + HIST_N) % HIST_N;
    return history[idx];
}

// ---- persisted ride logs ----------------------------------------------------
void saveRideSummaryLog() {
    // Skip tiny accidental bench/test resets.
    if (tripDistanceKm < 0.05f && currentWattHours < 1.0f) return;

    RideLog log;
    log.durationSec = (millis() - rideStartMs) / 1000;
    log.distanceKm = tripDistanceKm;
    log.maxSpeedKmh = maxSpeedKmh;
    log.avgSpeedKmh = avgSpeedKmh;
    log.whUsed = currentWattHours;
    log.whRegen = currentWhRegen;
    log.minVoltage = minVoltageSession;
    log.maxWatts = maxWattsSession;

    uint8_t head = prefs.getUChar("logHead", 0);
    uint8_t count = prefs.getUChar("logCount", 0);

    char key[12];
    snprintf(key, sizeof(key), "ride%02u", (unsigned int)head);
    prefs.putBytes(key, &log, sizeof(log));

    head = (head + 1) % RIDE_LOG_MAX;
    if (count < RIDE_LOG_MAX) count++;

    prefs.putUChar("logHead", head);
    prefs.putUChar("logCount", count);
}

// ---- persisted odometer/trip ------------------------------------------------
void saveOdo() {
    prefs.putFloat("odo", totalDistanceKm);
    prefs.putFloat("trip", tripDistanceKm);
}

// ---- demo simulation --------------------------------------------------------
// Fake telemetry for bench testing (Wokwi or gDemoMode on hardware). Drives the
// SAME state a real VESC poll would: a speed ramp, load-proportional draw with
// regen on braking, integrated energy (Wh used/regen), and first-order thermal
// models for motor/ESC/battery — so every page animates as if wired to the ESC.
static void simulateTelemetry() {
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

// ---- range estimate ---------------------------------------------------------
static float configuredNominalPackWh() {
    return BATTERY_EFFECTIVE_CAPACITY_AH * BATTERY_CELLS_COUNT * BATTERY_NOMINAL_CELL_V;
}

static float configuredUsablePackWh() {
    // Scale nominal pack Wh by the configured usable voltage window. This keeps
    // range conservative and aligned with the voltage where the dashboard says
    // to stop, instead of estimating all the way to a fully depleted cell.
    float usableWindow = max(0.1f, BATTERY_FULL_CELL_V - BATTERY_STOP_CELL_V);
    float nominalWindow = max(0.1f, BATTERY_FULL_CELL_V - 3.0f);
    return configuredNominalPackWh() * constrain(usableWindow / nominalWindow, 0.0f, 1.0f);
}

static float defaultWhPerKm() {
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

// ---- one poll cycle ---------------------------------------------------------
void pollVescData() {
    bool useSim = true;
    #ifndef WOKWI_SIMULATION
    useSim = gDemoMode;
    if (!useSim) {
        Esk8OS::Transports::RawVescData raw;
        if (Esk8OS::Transports::getLatestVescData(&raw)) {
            float wheelRPM = (raw.rpm / profilePolePairs()) * profileGearRatio();
            currentSpeedKmh = (wheelRPM * profileCircumfM() * 60.0) / 1000.0;
            currentSpeedMph = currentSpeedKmh * 0.621371;

            currentVoltage = raw.inpVoltage;
            float pct = ((currentVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) * 100.0;
            currentBatteryPercent = constrain((int)pct, 0, 100);

            currentMotorTemp = raw.tempMotor;
            currentEscTemp = raw.tempMosfet;
            currentAmps = raw.avgInputCurrent;
            currentMotorAmps = raw.avgMotorCurrent;
            currentDuty = raw.dutyCycleNow * 100.0;
            if (!rideEnergyBaselineSet) {
                rideStartVescWh = raw.wattHours;
                rideStartVescWhRegen = raw.wattHoursCharged;
                rideEnergyBaselineSet = true;
            }
            currentWattHours = max(0.0f, raw.wattHours - rideStartVescWh);
            currentWhRegen = max(0.0f, raw.wattHoursCharged - rideStartVescWhRegen);
            currentWatts = currentVoltage * currentAmps;
            vescFault = raw.error;
            lastVescOkMs = millis();
        }
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
