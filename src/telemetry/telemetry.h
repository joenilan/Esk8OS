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
