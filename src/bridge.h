#pragma once
// VESC Tool bridge mode (WiFi-TCP for desktop + BLE for mobile). Forwards raw
// bytes between VESC Tool and the ESC UART so the ESC can be configured
// wirelessly through the display. Public entry points used by main.cpp.

void enterBridgeMode();   // stop dashboard polling, start WiFi+BLE, show screen
void exitBridgeMode();    // tear down, return to the dashboard
void bridgeLoop();        // pump byte forwarding; call each loop while bridging
