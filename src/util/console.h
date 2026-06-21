#pragma once
#include <Arduino.h>

// ============================================================================
// USB-serial command console — ride-log management + diagnostics over the
// serial monitor, and the general "terminal" for poking the board without
// building dedicated screen UI for each action. The command backend here is
// reused by the WiFi HTTP export later.
//
// Commands: help | logs | cat <file> | rm <file|all> | free | odo
// Type them in the PlatformIO/Arduino serial monitor at 115200.
// ============================================================================

void consolePoll();   // read + dispatch a line if one is ready; call each loop
