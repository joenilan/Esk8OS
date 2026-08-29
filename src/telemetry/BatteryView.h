#pragma once

#include <Arduino.h>
#include "transports/DalyBms.h"

namespace Esk8OS {
namespace Telemetry {

enum BatterySource : uint8_t {
    BATTERY_SOURCE_NONE = 0,
    BATTERY_SOURCE_VESC,
    BATTERY_SOURCE_DALY,
    BATTERY_SOURCE_FUSED,
    BATTERY_SOURCE_DEMO
};

// A read-only presentation snapshot. It deliberately does not overwrite the
// VESC telemetry globals: ride safety, sag detection, energy integration,
// bridge ownership, and adaptive learning continue to consume VESC-aligned
// values. UI/companion/diagnostics can still show a real battery while the
// drivetrain is powered down.
struct BatteryView {
    bool live = false;
    bool vescLive = false;
    bool bmsLive = false;
    BatterySource source = BATTERY_SOURCE_NONE;

    bool voltageValid = false;
    float packVoltage = 0.0f;

    bool socValid = false;
    int socPct = 0;
    // True only when SOC had to fall back to the voltage curve. A fresh,
    // in-range Daly coulomb counter is the primary SOC source.
    bool socProvisional = false;
    bool bmsReportedSocValid = false;
    float bmsReportedSocPct = 0.0f;

    // Normalized ride convention (+ discharge, - regen/charge). VESC remains
    // authoritative while live; verified Daly current fills parked/no-VESC use.
    bool currentValid = false;
    float currentA = 0.0f;

    bool temperatureValid = false;
    float batteryTempC = 0.0f;

    bool remainingAhValid = false;
    float remainingAh = 0.0f;

    bool cellVoltageValid = false;
    float cellVoltage = 0.0f;   // minimum fresh Daly cell, else VESC pack/cells
    uint8_t cellCount = 0;
    uint8_t minCellNo = 0;
    uint16_t minCellmV = 0;
    uint16_t maxCellmV = 0;
    uint16_t deltaMv = 0;

    bool rangeValid = false;
    float homeRangeKm = 0.0f;
    float limpRangeKm = 0.0f;

    uint16_t bmsFreshMask = 0;
    Esk8OS::Transports::BmsAlarmLevel alarmLevel =
        Esk8OS::Transports::BMS_ALARM_STALE;
};

BatteryView getBatteryView();
const char* batterySourceName(BatterySource source);

}  // namespace Telemetry
}  // namespace Esk8OS
