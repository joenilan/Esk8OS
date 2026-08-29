#pragma once
#include "esk8os.h"

// Telemetry data layer: reads the VESC (or the demo simulation), integrates trip/
// energy, maintains the range estimate, the RAM history buffer, and persisted
// odometer + compact ride summaries. Operates on shared globals in esk8os.h.

void pollVescData();          // one poll cycle (real VESC or simulation)
void updateRangeEstimate();   // recompute estimated/remaining range + avg Wh
void recordHistorySample();   // append to the 3-minute RAM history (1/sec, self-gated)
TelemetrySample getHistorySample(int ageIndex);  // 0 = oldest, histCount-1 = newest
void saveRideSummaryLog();    // append a compact ride summary to NVS (on trip reset)
void telemetryPrintCal(Print& out);   // dump adaptive battery calibration (`cal`)
void telemetryResetCal();             // clear learned calibration back to seeds
// The configured range floors lifted by the learned typical sag — i.e. the
// LOADED per-cell voltage the board will actually reach limp/home at, which is
// what the estimate targets. Exposed so the app can show the real floor, not
// just the resting number the user set.
float effectiveHomeFloorCellV();
float effectiveLimpFloorCellV();
// Resting/open-circuit Li-ion estimate used when a coulomb counter is absent or
// invalid. This is a provisional fallback, not a replacement for fresh BMS SOC.
int estimateLiIonSocFromCellVoltage(float cellVolts);
// Same estimate for a loaded cell measurement. Uses the VESC-authoritative
// signed pack current and learned pack resistance to undo discharge sag (or
// charge lift). It remains VESC-current based so drivetrain calculations keep a
// single current authority even though Daly sign/scaling is now ride-verified.
int estimateLiIonSocFromLoadedCellVoltage(float cellVolts,
                                          float packCurrentA,
                                          int cellCount);
// Read-only range projection for an externally supplied SOC (for example the
// Daly BMS). Uses the same learned pack-energy/Wh-distance model and reserve
// floors as the VESC path without mutating its filtered SOC or learning state.
float estimateRemainingRangeForSoc(float socPct, bool toLimpFloor = false);
