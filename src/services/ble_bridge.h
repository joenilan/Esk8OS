#pragma once
#include <Arduino.h>

// ============================================================================
// BLE bridge backend for the mobile VESC Tool app.
//
// Mobile VESC Tool connects to VESC hardware over Bluetooth LE using the Nordic
// UART Service (NUS), the same service the official VESC BLE modules expose.
// This backend exposes that service and forwards bytes to/from Serial1 so the
// phone app can configure the ESC wirelessly through the display.
//
// SHARED-SERVER MODEL:
//   The NimBLE stack is owned by companion_ble.{h,cpp}, which initializes it once
//   in setup() and keeps the Companion Telemetry service advertising at all times.
//   This bridge does NOT init/deinit NimBLE. Instead companion_ble calls
//   bleBridgeRegister() during startup to build the NUS service onto that same
//   server (so the GATT table is assembled atomically before advertising). The
//   bridge then just toggles a forwarding flag when VESC Bridge mode is entered/
//   exited — the companion service and the VESC bridge co-exist on one server.
//
// Real on hardware when BLE_BRIDGE_ENABLED is defined; compiles to no-ops
// otherwise (e.g. Wokwi), so callers don't need #ifdefs.
// ============================================================================

class NimBLEServer;
class NimBLEAdvertising;

void          bleBridgeRegister(NimBLEServer* server, NimBLEAdvertising* adv); // build NUS on the shared server (call once at startup)
const char*   bleBridgeServiceUuid();              // NUS service UUID (for advertising)
void          bleBridgeStart();                    // begin forwarding (bridge mode entered)
void          bleBridgeStop();                     // stop forwarding (bridge mode exited)
void          bleBridgePoll();                     // housekeeping; call each loop
void          bleBridgeNotify(const uint8_t* data, size_t len);  // ESC -> app
bool          bleBridgeConnected();                // a client is connected AND forwarding is active
unsigned long bleBridgeRxBytes();                  // app -> ESC, this session
unsigned long bleBridgeTxBytes();                  // ESC -> app, this session
