#pragma once
#include <Arduino.h>

// ============================================================================
// Board session logging to LittleFS.
//
// A session log is the board's black-box CSV for this power-on dashboard session:
// it starts at boot, records at 1 Hz while the dashboard is running, and is not
// reset by TRIP_RESET or phone trip recording. The current ride/trip values are
// still included as columns so app GPS trips can be compared against board data.
// ============================================================================

void sessionLogBegin();       // mount LittleFS; call once in setup()
void sessionLogStart();       // open a fresh CSV for this board-on session
void sessionLogFlush();       // flush active CSV, keeping the same session open
void sessionLogEnd();         // flush + close the active session file
void sessionLogTick();        // append a 1 Hz dashboard sample
void sessionLogMark(const char* event); // append a one-off event row
void sessionLogNote(const char* text);  // append free-text '#' comment lines (flushed) — forensics
void sessionLogDeleteAll();   // close current session, delete CSVs, start fresh
bool sessionLogReady();       // true if LittleFS mounted (logging available)

// Runtime control + status (master switch for debugging, low-space failsafe).
void   sessionLogSetEnabled(bool on);
bool   sessionLogEnabled();
bool   sessionLogFull();
size_t sessionLogFreeBytes();
