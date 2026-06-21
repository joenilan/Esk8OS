#pragma once
#include <Arduino.h>

// ============================================================================
// Detailed ride logging to LittleFS — one CSV file per ride, sampled at 1 Hz
// while moving. This is the FULL time-series (speed/volts/watts/amps/duty/temps/
// battery) you can pull off the board for analysis, complementing the compact
// per-ride SUMMARIES kept in NVS (telemetry.cpp) that the LOGS page shows.
//
// Storage: the board's default 16MB partition scheme already exposes a ~3.4MB
// "spiffs" data partition; LittleFS mounts it by default, so no partition-table
// change is needed (NVS odometer/settings are untouched). Oldest ride files are
// pruned automatically when the partition runs low.
// ============================================================================

void ridelogBegin();       // mount LittleFS; call once in setup()
void ridelogStartRide();   // open a fresh CSV for the current ride + header row
void ridelogEndRide();     // flush + close the current ride file
void ridelogTick();        // append a 1 Hz sample while moving; call each loop
void ridelogDeleteAll();   // close current ride, delete every CSV, start fresh
bool ridelogReady();       // true if LittleFS mounted (logging available)

// Runtime control + status (master switch for debugging, low-space failsafe).
void   ridelogSetEnabled(bool on);  // turn logging on/off at runtime
bool   ridelogEnabled();            // false if turned off via the switch
bool   ridelogFull();               // true if logging auto-stopped (partition low)
size_t ridelogFreeBytes();          // free space on the log partition
