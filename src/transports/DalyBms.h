// ============================================================================
// Daly Smart BMS 40A "K" client.
//
// The plan this serves: ESK8OS replaces the Daly phone app — reading everything
// the Daly app shows and re-exposing it through ESK8OS's own display and
// companion BLE. The connection is BLUETOOTH: ESK8OS talks to the Daly's BLE
// dongle wirelessly, nothing is wired.
//
// ⚠️ The transport in DalyBms.cpp is still the old UART placeholder and must be
// rebuilt as a BLE client (see the big note there). The Daly protocol decode
// (0x90–0x98), the BmsData model, and the demo sim are transport-agnostic and
// unaffected — only the byte send/receive layer changes.
//
// The value the VESC cannot give you: the individual cell voltages. A single
// lagging cell is how a DIY pack strands you or catches fire, and it is invisible
// from the pack terminals. This is the whole reason to read the BMS at all.
//
// Compiled out entirely unless -DESK8OS_BMS_DALY. Daly frame format (same over
// UART or the BLE dongle): 13-byte frames, 0xA5 start, 0x40 host, sum checksum.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace Esk8OS {
namespace Transports {

// Room for the largest packs this firmware targets; a 10S skate pack uses 10.
static const int BMS_MAX_CELLS = 16;
static const int BMS_MAX_TEMPS = 8;

// Everything ESK8OS knows about the pack from the BMS. Filled by the poll task
// (core 0), read by the console / display / companion BLE (core 1). The task
// builds a local and publishes it as one unit; readers MUST go through
// getBmsData() so they copy it atomically and never see a torn mix of old and
// new fields (a bare read of the shared struct can tear — the whole point of
// per-cell data is catching one lagging cell, and a torn read could hide it).
struct BmsData {
    bool     linkOk   = false;   // BMS answered within the staleness window
    uint32_t lastOkMs = 0;

    // 0x90 — pack-level.
    float    packVoltage = 0.0f; // V
    float    current     = 0.0f; // A, + = charging, - = discharging
    float    soc         = 0.0f; // %
    float    remainingAh = 0.0f; // Ah (from 0x93)

    // 0x94 — topology, learned from the BMS rather than assumed.
    uint8_t  cellCount = 0;
    uint8_t  tempCount = 0;
    uint16_t cycles    = 0;

    // 0x95 — the payload that matters. Per-cell millivolts.
    uint16_t cellmV[BMS_MAX_CELLS] = {0};
    // When each cell's mV was last actually refreshed. A dropped 0x95 frame
    // leaves that cell's old mV in place; comparing this against the pack's
    // lastOkMs tells a reader the value is stale so it can show "?" instead of
    // a lie. cellFresh() encapsulates the check.
    uint32_t cellSeenMs[BMS_MAX_CELLS] = {0};

    // 0x91 — extremes, reported directly by the BMS (not just derived here).
    uint16_t minCellmV = 0, maxCellmV = 0;
    uint8_t  minCellNo = 0, maxCellNo = 0;
    uint16_t cellDeltamV = 0;    // max - min; the pack's imbalance at a glance

    // 0x96 / 0x92 — temperatures, degrees C.
    int8_t   temps[BMS_MAX_TEMPS] = {0};
    int8_t   tempMax = 0, tempMin = 0;

    // 0x93 — the MOSFETs. These are what actually connect the pack.
    bool     chargeMos    = false;
    bool     dischargeMos = false;

    // 0x97 — which cells are actively bleeding to balance.
    bool     balancing[BMS_MAX_CELLS] = {false};

    // 0x98 — protection / alarm flags, raw. hasFault is set if any bit is on.
    uint8_t  faultBytes[7] = {0};
    bool     hasFault = false;
};

extern BmsData gBms;

// A cell's mV is fresh if it was refreshed within this window of the pack's last
// good poll. Wider than one 500 ms cycle so a single dropped frame doesn't flap
// the display, tight enough to catch a cell that has genuinely stopped reporting.
static const uint32_t BMS_CELL_STALE_MS = 2000;
inline bool cellFresh(const BmsData& b, int cell) {
    return b.linkOk && b.cellSeenMs[cell] != 0 &&
           (b.lastOkMs - b.cellSeenMs[cell]) < BMS_CELL_STALE_MS;
}

// Atomic snapshot of the pack state. The ONLY supported way to read the BMS from
// another task — copies the published struct under a lock so the reader gets a
// coherent frame. Returns false (and a zeroed *out) on a build without the BMS.
bool getBmsData(BmsData* out);

// Starts the BMS UART and its poll task. A no-op (leaving gBms.linkOk false)
// unless built with -DESK8OS_BMS_DALY.
void beginDalyBms();

}  // namespace Transports
}  // namespace Esk8OS
