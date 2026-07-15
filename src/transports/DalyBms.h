// ============================================================================
// Daly smart BMS client, over the BMS's UART port.
//
// The plan this serves: ESK8OS replaces the Daly phone app. You pull the Daly's
// BLE dongle, wire the ESP32 into that same UART port, and ESK8OS becomes the
// BMS master — reading everything the Daly app shows and re-exposing it through
// ESK8OS's own display and companion BLE. There is no dual-Bluetooth and no port
// contention, because the dongle is gone and we are the only thing on the wire.
//
// The value the VESC cannot give you: the individual cell voltages. A single
// lagging cell is how a DIY pack strands you or catches fire, and it is invisible
// from the pack terminals. This is the whole reason to read the BMS at all.
//
// Compiled out entirely unless -DESK8OS_BMS_DALY. Protocol: 13-byte frames,
// 0xA5 start, 0x40 host address, 9600 baud, one-byte sum checksum.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace Esk8OS {
namespace Transports {

// Room for the largest packs this firmware targets; a 10S skate pack uses 10.
static const int BMS_MAX_CELLS = 16;
static const int BMS_MAX_TEMPS = 8;

// Everything ESK8OS knows about the pack from the BMS. Filled by the poll task,
// read by the console / display / companion BLE. Single-writer (the task), so
// readers see at worst a one-cycle-stale field, never a corrupt struct.
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

// Starts the BMS UART and its poll task. A no-op (leaving gBms.linkOk false)
// unless built with -DESK8OS_BMS_DALY.
void beginDalyBms();

}  // namespace Transports
}  // namespace Esk8OS
