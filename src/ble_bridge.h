#pragma once
#include <Arduino.h>

// ============================================================================
// BLE bridge backend for the mobile VESC Tool app.
//
// The existing bridge forwards bytes between desktop VESC Tool (WiFi-TCP) and
// the ESC UART. Mobile VESC Tool doesn't do WiFi-TCP — it connects to VESC
// hardware over Bluetooth LE using the Nordic UART Service (NUS), the same
// service the official VESC BLE modules expose. This backend makes the display
// advertise that service and forward bytes to/from Serial1, so the phone app
// can configure the ESC wirelessly through the display.
//
// Runs ALONGSIDE the WiFi-TCP backend (both can be active in bridge mode; in
// practice one client connects at a time). Real on hardware when
// BLE_BRIDGE_ENABLED is defined; compiles to no-ops otherwise (e.g. Wokwi), so
// callers don't need #ifdefs.
//
// STATUS: scaffolded, NOT yet tested end-to-end — that needs a live ESC on the
// UART (see the integration notes in main.cpp / README). The NUS UUIDs and the
// chunked-notify path match what VESC Tool expects; verify throughput + that
// VESC Tool can read/write the config once the replacement ESC is wired in.
// ============================================================================

void          bleBridgeStart(const char* name);   // begin advertising NUS
void          bleBridgeStop();                     // tear down + free the stack
void          bleBridgePoll();                     // housekeeping; call each loop
void          bleBridgeNotify(const uint8_t* data, size_t len);  // ESC -> app
bool          bleBridgeConnected();
unsigned long bleBridgeRxBytes();                  // app -> ESC, this session
unsigned long bleBridgeTxBytes();                  // ESC -> app, this session
