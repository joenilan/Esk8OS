#pragma once
#include <Arduino.h>

struct TelemetrySample {
    float speedKmh, volts, watts, batteryAmps, motorAmps, duty, motorTemp, escTemp;
    int   batteryPct;
};

struct RideLog {
    uint32_t durationSec;
    float distanceKm, maxSpeedKmh, avgSpeedKmh, whUsed, whRegen, minVoltage, maxWatts;
};

const int HIST_N = 180;       // 3 minutes at 1 sample/sec
const int RIDE_LOG_MAX = 10;

extern float currentSpeedKmh;
extern float currentSpeedMph;
extern float currentVoltage;
extern int   currentBatteryPercent;
extern float currentMotorTemp;
extern float currentBatteryTemp;
extern float currentEscTemp;
extern float currentAmps;
extern float currentMotorAmps;
extern float currentDuty;
extern float currentWatts;
extern float peakWatts;
extern float currentWattHours;
extern float currentWhRegen;
extern float maxSpeedKmh;
extern float avgSpeedKmh;
extern float maxWattsSession;
extern float minVoltageSession;
extern float maxMotorAmpsSession;

extern int   vescFault;
extern bool  vescLinkOk;
extern unsigned long lastVescOkMs;

extern unsigned long rideStartMs;
extern float sessionTripStartKm;
extern float tripDistanceKm;
extern float totalDistanceKm;
extern uint32_t tripMovingSec;       // trip moving time (seconds rolling); persisted, board-authoritative
extern unsigned long lastMovedMs;    // millis() of last rolling sample; drives the parked auto-reset
extern float estimatedRangeKm;
extern float remainingRangeKm;
extern float avgWhPerKm;
extern float rideStartVescWh;
extern float rideStartVescWhRegen;
extern bool  rideEnergyBaselineSet;
extern bool  rangeEstimateReady;

extern int motorHealthPct;
extern int batteryHealthPct;
extern int escHealthPct;

extern TelemetrySample history[];
extern int histHead;
extern int histCount;
