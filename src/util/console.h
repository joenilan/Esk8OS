#pragma once
#include <Arduino.h>

// ============================================================================
// USB-serial command console — session-log management + diagnostics over the
// serial monitor, and the general "terminal" for poking the board without
// building dedicated screen UI for each action. The command backend here is
// reused by the WiFi HTTP export later.
//
// Commands: help | logs | cat <file> | rm <file|all> | free | odo
// Type them in the PlatformIO/Arduino serial monitor at 115200.
// ============================================================================

void consolePoll();   // read + dispatch a line if one is ready; call each loop

// Run one command line for a wireless transport (the HTTP console at /cmd),
// capturing everything it prints into `out`. Same dispatch + confirm flow as
// typed serial input; output capped so a huge `cat` can't eat the heap.
void consoleRunCapture(const char* line, String& out);
