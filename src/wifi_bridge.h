#pragma once
#include <Arduino.h>

// ============================================================================
// WiFi-TCP bridge transport for desktop VESC Tool.
//
// Brings up a softAP + a TCP server that desktop VESC Tool connects to, and
// forwards raw bytes between the TCP client and the ESC UART (Serial1). This is
// one of the transports the bridge-mode coordinator (bridge.{h,cpp}) drives;
// the BLE transport (ble_bridge.{h,cpp}) is the mobile counterpart.
//
// app -> ESC is pumped here in wifiBridgePoll(); ESC -> app is sent via
// wifiBridgeNotify() by the coordinator, which reads the UART once and fans the
// reply out to every connected transport (so the two never double-read Serial1).
// ============================================================================

void          wifiBridgeStart();   // bring up the AP + TCP server
void          wifiBridgeStop();    // tear down the server + AP
bool          wifiBridgePoll();    // accept clients + pump app->ESC; true if forwarded
void          wifiBridgeNotify(const uint8_t* data, size_t len);  // ESC -> app
bool          wifiBridgeConnected();
unsigned long wifiBridgeRxBytes(); // app -> ESC, bytes this session
unsigned long wifiBridgeTxBytes(); // ESC -> app, bytes this session

// Connection details shown on the bridge screen:
const char*   wifiBridgeSsid();
const char*   wifiBridgePass();
String        wifiBridgeIpPort();  // e.g. "192.168.4.1:65102"
int           wifiBridgeStationNum();
