#include "telemetry.h"
#include "transports/VescUartTransport.h"

// Speed above which the board is considered "rolling" — trip moving-time and the
// last-moved timestamp only advance past this, so parked/walking time isn't counted.
static const float TRIP_MOVING_MIN_KMH = 2.0f;

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

// ---- persisted compact ride summaries --------------------------------------
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
    prefs.putUInt("tripsec", tripMovingSec);   // persist trip moving-time alongside distance
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
    telemetryLive = true;
    vescFault = 0;                // ...and fault-free: clear any stale fault left by a
                                  // real (e.g. unpowered) VESC read before demo was enabled,
                                  // so the board page isn't frozen behind a fault banner.

    // Simulated remote + diagnostics so the throttle slider / DIAG view animate on
    // the bench with no real remote: throttle tracks the accel/brake ramp.
    gPpmConnected = true;
    gPpmDecoded = accel ? (currentSpeedKmh / 45.0f) : -(currentSpeedKmh / 45.0f) * 0.6f;
    gPpmPulseMs = 1.5f + gPpmDecoded * 0.5f;   // 1.0..2.0 ms, centered at 1.5
    gVescFwMajor = 6; gVescFwMinor = 2;
    gSlaveOnline = true;
    gMasterMotorAmps = currentMotorAmps * 0.5f;
    gSlaveMotorAmps  = currentMotorAmps * 0.5f;
    gMasterMotorTemp = gSlaveMotorTemp = currentMotorTemp;
    gMasterEscTemp = gSlaveEscTemp = currentEscTemp;

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

static int liionSocFromCellV(float v);

static float configuredUsablePackWh(float floorCellV) {
    // Scale nominal pack Wh by the configured usable voltage window. This keeps
    // range conservative and aligned with the voltage where the dashboard says
    // to stop, instead of estimating all the way to a fully depleted cell.
    float usableWindow = max(0.1f, BATTERY_FULL_CELL_V - floorCellV);
    float nominalWindow = max(0.1f, BATTERY_FULL_CELL_V - 3.0f);
    return configuredNominalPackWh() * constrain(usableWindow / nominalWindow, 0.0f, 1.0f);
}

static float remainingWhToFloor(float floorCellV) {
    int floorPct = liionSocFromCellV(floorCellV);
    float usablePctWindow = max(1.0f, 100.0f - floorPct);
    float remainingFrac = constrain((currentBatteryPercent - floorPct) / usablePctWindow, 0.0f, 1.0f);
    return configuredUsablePackWh(floorCellV) * remainingFrac;
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
    float homeFloorCell = max(BATTERY_HOME_CELL_V, BATTERY_STOP_CELL_V);
    float homePackWh = configuredUsablePackWh(homeFloorCell);
    float limpPackWh = configuredUsablePackWh(BATTERY_STOP_CELL_V);
    estimatedRangeKm = homePackWh / avgWhPerKm;
    remainingRangeKm = remainingWhToFloor(homeFloorCell) / avgWhPerKm;
    estimatedLimpRangeKm = limpPackWh / avgWhPerKm;
    remainingLimpRangeKm = remainingWhToFloor(BATTERY_STOP_CELL_V) / avgWhPerKm;
}

// ---- state-of-charge from a real Li-ion curve -------------------------------
// The old gauge mapped pack voltage to % LINEARLY over [stop..full]. Li-ion isn't
// linear (a long flat plateau, then a steep drop), so the linear map badly
// over-read near empty — "30%" was effectively dead. This piecewise OCV->SoC
// table (per-cell, resting) is interpolated instead.
static int liionSocFromCellV(float v) {
    static const float vlut[] = {4.20,4.10,4.00,3.90,3.85,3.80,3.75,3.70,3.65,3.60,3.55,3.50,3.45,3.40,3.35,3.30,3.20,3.00};
    static const float slut[] = {100,  92,  83,  72,  65,  58,  50,  42,  35,  28,  22,  17,  13,   9,   6,   3,   1,   0};
    const int n = sizeof(vlut) / sizeof(vlut[0]);
    if (v >= vlut[0])   return 100;
    if (v <= vlut[n-1]) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (v <= vlut[i] && v >= vlut[i + 1]) {
            float t = (v - vlut[i + 1]) / (vlut[i] - vlut[i + 1]);
            return (int)lroundf(slut[i + 1] + t * (slut[i] - slut[i + 1]));
        }
    }
    return 0;
}

// Pack internal resistance (ohms) used to undo voltage sag: V_open = V + I*R.
// Calibrated from ride r0074: regressing pack volts vs battery current gave
// ~0.23 ohm using the MASTER VESC current alone; the real pack sees both motors'
// current (~2x), so true R ~= 0.11 ohm. NOTE this is high for a 10s6p (healthy is
// ~0.04-0.05) — aged/high-resistance cells or resistive wiring/connectors, which
// itself shortens usable range. Re-tune if cells/wiring change.
static const float BATTERY_INTERNAL_R_OHM = 0.11f;

// Smoothed SoC so sag/regen transients don't swing the gauge. EMA across polls
// (~10 Hz); seeded on the first real reading so it doesn't ramp up from zero.
static float gSocFilt = -1.0f;

// ---- one poll cycle ---------------------------------------------------------
void pollVescData() {
    bool useSim = true;
    #ifndef WOKWI_SIMULATION
    useSim = gDemoMode;
    if (!useSim) {
        Esk8OS::Transports::RawVescData raw;
        // Ignore reads from a VESC that's talking but not actually powered (logic
        // alive over UART, power stage off): input voltage reads implausibly low
        // and it emits a bogus DRV fault. Dropping the read lets the link go stale
        // -> LINK LOST instead of a meaningless FAULT banner.
        if (Esk8OS::Transports::getLatestVescData(&raw) && raw.inpVoltage >= VESC_MIN_OPERATIONAL_V) {
            float wheelRPM = (raw.rpm / profilePolePairs()) * profileGearRatio();
            currentSpeedKmh = (wheelRPM * profileCircumfM() * 60.0) / 1000.0;
            currentSpeedMph = currentSpeedKmh * 0.621371;

            currentVoltage = raw.inpVoltage;
            // Sag-compensate to open-circuit voltage (discharge current is positive,
            // so this adds the sag back; regen is negative, which subtracts it), then
            // read SoC off the Li-ion curve and low-pass filter it.
            float vOpen = currentVoltage + raw.avgInputCurrent * BATTERY_INTERNAL_R_OHM;
            float vCell = vOpen / max(1, BATTERY_CELLS_COUNT);
            int rawSoc = liionSocFromCellV(vCell);
            if (gSocFilt < 0) gSocFilt = rawSoc;            // seed on first sample
            else {
                float delta = rawSoc - gSocFilt;
                float alpha = 0.05f;
                // A full pack often drops from charger/surface voltage to its real
                // loaded voltage in the first minute. Show that in volts, but don't
                // let percent visibly plunge from 100% to low-90s on a short pull.
                if (delta < 0 && gSocFilt > 85.0f && raw.avgInputCurrent > 2.0f) {
                    alpha = 0.012f;
                } else if (delta > 0 && raw.avgInputCurrent < 2.0f) {
                    alpha = 0.08f;
                }
                gSocFilt += delta * alpha;
            }
            currentBatteryPercent = constrain((int)lroundf(gSocFilt), 0, 100);

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
            if (raw.error != 0) gLastFault = raw.error;   // latch for the diagnostics view
            // Remote input + diagnostics passthrough.
            gPpmDecoded = raw.ppmDecoded;
            gPpmPulseMs = raw.ppmPulseMs;
            gPpmConnected = raw.ppmConnected;
            gVescFwMajor = raw.fwMajor;
            gVescFwMinor = raw.fwMinor;
            gSlaveOnline = raw.slaveOnline;
            gMasterMotorAmps = raw.masterMotorAmps;
            gSlaveMotorAmps = raw.slaveMotorAmps;
            gMasterMotorTemp = raw.masterTempMotor;
            gSlaveMotorTemp = raw.slaveTempMotor;
            gMasterEscTemp = raw.masterTempMosfet;
            gSlaveEscTemp = raw.slaveTempMosfet;
            lastVescOkMs = millis();
            telemetryLive = true;
        }
    }
    #endif
    // On entering demo, seed a clean full-charge, fault-free state so a stale
    // reading left by a real/unpowered VESC doesn't strand a low-battery or fault
    // banner over the simulation. Covers every enable path (console/board/BLE).
    static bool wasSim = false;
    if (useSim && !wasSim) {
        currentBatteryPercent = 100;
        currentVoltage = BATTERY_MAX_V;
        currentMotorTemp = currentEscTemp = currentBatteryTemp = 24.0f;
        vescFault = 0;
        gLastFault = 0;
        peakWatts = 0;
    }
    wasSim = useSim;
    if (useSim) simulateTelemetry();

    // Peak-hold: rise instantly to new peaks, ease back down (~2-3s) so the
    // spike is readable instead of flickering.
    if (currentWatts >= peakWatts) peakWatts = currentWatts;
    else peakWatts += (currentWatts - peakWatts) * 0.15f;
    if (peakWatts < 0) peakWatts = 0;   // regen can drive watts negative; peak-hold stays >= 0

    // Session extremes for the Power page SESSION card
    if (currentWatts > maxWattsSession)              maxWattsSession = currentWatts;
    if (currentVoltage < minVoltageSession)          minVoltageSession = currentVoltage;
    if (fabs(currentAmps) > maxBatteryAmpsSession)   maxBatteryAmpsSession = fabs(currentAmps);
    if (fabs(currentMotorAmps) > maxMotorAmpsSession) maxMotorAmpsSession = fabs(currentMotorAmps);

    // Battery safety readout. This intentionally uses loaded pack voltage, not
    // sag-compensated open-circuit voltage, because VESC cutoffs/BMS trips happen
    // under load. Evee reports it and warns; the VESC still owns current limiting.
    loadedCellVoltage = currentVoltage / max(1, BATTERY_CELLS_COUNT);
    bool discharging = currentAmps > 2.0f && currentWatts > 50.0f;
    bool belowHomeLoaded = discharging && loadedCellVoltage <= BATTERY_HOME_CELL_V;
    bool belowLimpLoaded = discharging && loadedCellVoltage <= (BATTERY_STOP_CELL_V + 0.03f);
    if (discharging && currentVoltage < minVoltageUnderLoadSession) {
        minVoltageUnderLoadSession = currentVoltage;
    }
    static bool wasBelowHomeLoaded = false;
    if (belowHomeLoaded && !wasBelowHomeLoaded) sagEventsSession++;
    wasBelowHomeLoaded = belowHomeLoaded;

    static uint32_t lastSafetySec = 0;
    uint32_t safetySec = millis() / 1000UL;
    if (safetySec != lastSafetySec) {
        lastSafetySec = safetySec;
        if (belowHomeLoaded) homeVoltageSecondsSession++;
        if (belowLimpLoaded) limpVoltageSecondsSession++;
    }

    // Accumulate trip + odometer from speed (km), then persist to flash on
    // every stop and every 60s so they survive a disconnect / power-off.
    // In DEMO mode the speed is synthetic, so the *lifetime odometer* must NOT
    // grow (and must never be persisted) — it is a real total. Trip still
    // accumulates so the demo shows live distance.
    static unsigned long lastDistMs = 0;
    static uint32_t movingMsRem = 0;          // sub-second carry for the moving-time accumulator
    unsigned long nowMs = millis();
    if (lastDistMs != 0) {
        unsigned long dtMs = nowMs - lastDistMs;
        float dKm = currentSpeedKmh * (dtMs / 3600000.0f);
        if (dKm > 0) {
            tripDistanceKm += dKm;
            if (!DEMO_DATA) totalDistanceKm += dKm;
        }
        // Trip TIME = moving time: accumulate seconds only while rolling, so
        // parked/walking/off gaps are never counted (self-correcting). This is the
        // board's authoritative trip clock; lastMovedMs feeds the 6h parked reset.
        if (currentSpeedKmh > TRIP_MOVING_MIN_KMH) {
            movingMsRem += (uint32_t)dtMs;
            while (movingMsRem >= 1000) { tripMovingSec++; movingMsRem -= 1000; }
            lastMovedMs = nowMs;
        }
    }
    lastDistMs = nowMs;

    // Persist on a moving->stopped edge (so a stop checkpoints the trip) and at
    // least every 60s while riding. The stop-save is debounced to once per 10s so
    // speed chattering across the 1 km/h line near a stop can't hammer NVS.
    static unsigned long lastSave = 0;
    static bool wasMoving = false;
    bool moving = currentSpeedKmh > 1.0;
    bool stopEdge = wasMoving && !moving && (millis() - lastSave > 10000);
    if (!DEMO_DATA && (stopEdge || (millis() - lastSave > 60000))) {
        saveOdo();
        lastSave = millis();
    }
    wasMoving = moving;

    // Telemetry link freshness, session max + average speed
    vescLinkOk = (lastVescOkMs != 0) && (millis() - lastVescOkMs < VESC_LINK_TIMEOUT_MS);
    if (!useSim && !vescLinkOk) {
        telemetryLive = false;
        currentSpeedKmh = 0.0f;
        currentSpeedMph = 0.0f;
        currentAmps = 0.0f;
        currentMotorAmps = 0.0f;
        currentDuty = 0.0f;
        currentWatts = 0.0f;
        vescFault = 0;          // a lost link has no live fault — show LINK LOST, not a stale FAULT
        gPpmConnected = false;
        gPpmDecoded = 0.0f;
        gPpmPulseMs = 0.0f;
    }
    if (currentSpeedKmh > maxSpeedKmh) maxSpeedKmh = currentSpeedKmh;
    // AVG is a true moving-average: session distance / session MOVING-time, so it
    // doesn't sag while parked (the trip clock and distance both pause together).
    uint32_t sessionMovingSec = tripMovingSec - sessionMovingStartSec;
    if (sessionMovingSec > 0) avgSpeedKmh = (tripDistanceKm - sessionTripStartKm) / (sessionMovingSec / 3600.0f);
    updateRangeEstimate();

    if (belowLimpLoaded || remainingLimpRangeKm <= RANGE_LIMP_HOME_KM) {
        rangeAlertState = 3;  // LIMP HOME
    } else if (belowHomeLoaded) {
        rangeAlertState = 2;  // VOLT SAG
    } else if (remainingRangeKm <= RANGE_TURN_HOME_KM &&
               (tripDistanceKm - sessionTripStartKm) > 0.3f) {
        rangeAlertState = 1;  // TURN HOME
    } else {
        rangeAlertState = 0;
    }
}
