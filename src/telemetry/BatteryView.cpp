#include "telemetry/BatteryView.h"

#include "esk8os.h"
#include "telemetry/telemetry.h"

namespace Esk8OS {
namespace Telemetry {

const char* batterySourceName(BatterySource source) {
    switch (source) {
        case BATTERY_SOURCE_VESC:  return "vesc";
        case BATTERY_SOURCE_DALY:  return "daly";
        case BATTERY_SOURCE_FUSED: return "fused";
        case BATTERY_SOURCE_DEMO:  return "demo";
        default:                   return "none";
    }
}

BatteryView getBatteryView() {
    using namespace Esk8OS::Transports;

    BatteryView out;
    out.vescLive = telemetryLive && (gDemoMode || vescLinkOk);

    BmsData bms;
    getBmsData(&bms);
    out.bmsLive = !gDemoMode && bms.linkOk &&
                  bmsFieldFresh(bms, BMS_FIELD_PACK);
    out.bmsFreshMask = bmsFreshMask(bms);
    out.alarmLevel = bmsAlarmLevel(bms);

    if (gDemoMode) out.source = BATTERY_SOURCE_DEMO;
    else if (out.vescLive && out.bmsLive) out.source = BATTERY_SOURCE_FUSED;
    else if (out.vescLive) out.source = BATTERY_SOURCE_VESC;
    else if (out.bmsLive) out.source = BATTERY_SOURCE_DALY;

    out.live = out.source != BATTERY_SOURCE_NONE;

    // Keep the rider-facing pack voltage on one sensor across a drivetrain
    // on/off transition. Daly's pack value is coherent with its individual-cell
    // sum; VESC voltage remains untouched for cutoffs, sag, power and diagnostics.
    if (out.bmsLive) {
        out.voltageValid = true;
        out.packVoltage = bms.packVoltage;
    } else if (out.vescLive) {
        out.voltageValid = currentVoltage > 0.0f;
        out.packVoltage = currentVoltage;
    }
    if (out.vescLive) {
        out.currentValid = true;
        out.currentA = currentAmps;
    } else if (out.bmsLive) {
        // The 2026-07-30 joint ride established that this module reports
        // negative discharge and positive regen/charge. Normalize it to the
        // dashboard convention without changing any drivetrain safety input.
        out.currentValid = true;
        out.currentA = -bms.current;
    }

    out.cellCount = out.bmsLive && bms.cellCount ? bms.cellCount
                                                 : (uint8_t)BATTERY_CELLS_COUNT;
    bool freshBmsCells = false;
    float weakestBmsCellV = 0.0f;
    if (out.bmsLive && bmsFieldFresh(bms, BMS_FIELD_CELLS)) {
        freshBmsCells = bms.cellCount > 0;
        for (int c = 0; c < bms.cellCount && c < BMS_MAX_CELLS; c++) {
            if (!cellFresh(bms, c)) { freshBmsCells = false; break; }
        }
        if (freshBmsCells) {
            // Range is ultimately limited by the first group to reach the BMS
            // floor, so use the weakest fresh group for provisional usable SOC.
            // The loop above ensures one stale group invalidates the estimate.
            weakestBmsCellV = bms.minCellmV / 1000.0f;
            out.cellVoltageValid = true;
            out.cellVoltage = bms.minCellmV / 1000.0f;
            out.minCellNo = bms.minCellNo;
            out.minCellmV = bms.minCellmV;
            out.maxCellmV = bms.maxCellmV;
            out.deltaMv = bms.cellDeltamV;
        }
    }
    if (!out.cellVoltageValid && out.vescLive && BATTERY_CELLS_COUNT > 0) {
        out.cellVoltageValid = true;
        out.cellVoltage = currentVoltage / BATTERY_CELLS_COUNT;
    }

    // The 2026-07-30 joint ride discharged the Daly counter from 32.00 Ah/100%
    // to 20.19 Ah/63%. Its 11.81 Ah decrement agreed with integrated current
    // (11.64 Ah) to about 1.4%, so a fresh, in-range counter is the best primary
    // SOC source. Voltage SOC remains a deliberately provisional fallback.
    if (out.bmsLive) {
        out.bmsReportedSocValid = bms.soc >= 0.0f && bms.soc <= 100.0f;
        out.bmsReportedSocPct = bms.soc;
        if (bmsFieldFresh(bms, BMS_FIELD_MOS)) {
            out.remainingAhValid = bms.remainingAh >= 0.0f;
            out.remainingAh = bms.remainingAh;
        }
    }
    if (out.bmsReportedSocValid) {
        out.socValid = true;
        out.socPct = constrain((int)lroundf(out.bmsReportedSocPct), 0, 100);
    } else if (freshBmsCells) {
        out.socValid = true;
        out.socProvisional = true;
        out.socPct = out.vescLive
            ? estimateLiIonSocFromLoadedCellVoltage(
                  weakestBmsCellV, currentAmps, bms.cellCount)
            : estimateLiIonSocFromCellVoltage(weakestBmsCellV);
    } else if (out.vescLive) {
        out.socValid = true;
        out.socProvisional = true;
        out.socPct = constrain(currentBatteryPercent, 0, 100);
    }

    if (out.bmsLive &&
        (bmsFieldFresh(bms, BMS_FIELD_TEMPS) ||
         bmsFieldFresh(bms, BMS_FIELD_MINMAX_T))) {
        out.temperatureValid = true;
        out.batteryTempC = bms.tempMax;
    } else if (out.vescLive && currentBatteryTemp != 0.0f) {
        out.temperatureValid = true;
        out.batteryTempC = currentBatteryTemp;
    }

    if (out.socValid) {
        out.rangeValid = true;
        if (out.bmsLive) {
            out.homeRangeKm = estimateRemainingRangeForSoc(out.socPct, false);
            out.limpRangeKm = estimateRemainingRangeForSoc(out.socPct, true);
        } else {
            out.homeRangeKm = remainingRangeKm;
            out.limpRangeKm = remainingLimpRangeKm;
        }
    }

    return out;
}

}  // namespace Telemetry
}  // namespace Esk8OS
