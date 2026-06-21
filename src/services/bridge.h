#pragma once
// VESC Tool BRIDGE MODE coordinator. Bridge mode lets VESC Tool configure the
// ESC wirelessly through the display; this owns the mode (screen + transitions)
// and drives the two transports — WiFi-TCP for desktop (wifi_bridge.{h,cpp}) and
// BLE NUS for mobile (ble_bridge.{h,cpp}). Public entry points used by main.cpp.

void enterBridgeMode();   // stop dashboard polling, start WiFi+BLE, show screen
void exitBridgeMode();    // tear down, return to the dashboard
void bridgeLoop();        // pump byte forwarding; call each loop while bridging
