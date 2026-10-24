// ============================================================================
// Daly Smart BMS 40A "K" client.
//
// The plan this serves: ESK8OS replaces the Daly phone app — reading everything
// the Daly app shows and re-exposing it through ESK8OS's own display and
// companion BLE. The connection is BLUETOOTH: ESK8OS talks to the Daly's BLE
// dongle wirelessly, nothing is wired.
//
// The physical "UART K" module was verified as BLE GATT: service FFF0, FFF1
// notify (module -> ESP32), FFF2 write (ESP32 -> module), with raw 13-byte Daly
// 0x90–0x98 request/reply frames and no observed application handshake.
// DalyBms.cpp is a NimBLE central sharing the stack with the companion server.
//
// The value the VESC cannot give you: the individual cell voltages. A single
// lagging cell is how a DIY pack strands you or catches fire, and it is invisible
// from the pack terminals. This is the whole reason to read the BMS at all.
//
// Compiled out entirely unless -DESK8OS_BMS_DALY. Daly frames are 13 bytes:
// 0xA5 start, 0x40 host, command, 8-byte payload, sum checksum.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "transports/DalyTarget.h"

namespace Esk8OS {
namespace Transports {

// Room for the largest packs this firmware targets; a 10S skate pack uses 10.
static const int BMS_MAX_CELLS = 16;
static const int BMS_MAX_TEMPS = 8;

// Each legacy Daly command is independent. A good 0x90 pack reply proves the
// transport is alive, but it does not prove that a later MOS/fault/cell request
// in the same poll succeeded. Keep a timestamp per response group so consumers
// can distinguish a live value from a carried-forward diagnostic value.
enum BmsField : uint8_t {
    BMS_FIELD_PACK = 0,
    BMS_FIELD_MINMAX_V,
    BMS_FIELD_MINMAX_T,
    BMS_FIELD_MOS,
    BMS_FIELD_STATUS,
    BMS_FIELD_CELLS,
    BMS_FIELD_TEMPS,
    BMS_FIELD_BALANCE,
    BMS_FIELD_FAULT,
    BMS_FIELD_COUNT
};

enum BmsAlarmLevel : uint8_t {
    BMS_ALARM_NONE = 0,
    BMS_ALARM_WARNING = 1,   // only physically verified Level-1 warning bits
    BMS_ALARM_ALERT = 2,     // another/unknown alarm bit: do not downplay it
    BMS_ALARM_STALE = 3      // the 0x98 response itself is not fresh
};

enum BmsFaultKind : uint8_t {
    BMS_FAULT_NONE = 0,
    BMS_FAULT_HIGH_VOLTAGE_L1,
    BMS_FAULT_DISCHARGE_OVERCURRENT_WARNING,
    BMS_FAULT_DISCHARGE_OVERCURRENT_PROTECTION,
    BMS_FAULT_OTHER
};

// Everything ESK8OS knows about the pack from the BMS. Filled by the poll task
// (core 0), read by the console / display / companion BLE (core 1). The task
// builds a local and publishes it as one unit; readers MUST go through
// getBmsData() so they copy it atomically and never see a torn mix of old and
// new fields (a bare read of the shared struct can tear — the whole point of
// per-cell data is catching one lagging cell, and a torn read could hide it).
struct BmsData {
    bool     linkOk   = false;   // BMS answered within the staleness window
    uint32_t lastOkMs = 0;
    uint32_t fieldSeenMs[BMS_FIELD_COUNT] = {0};

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
    // A protection can clear before the rider reaches the BMS page. Preserve
    // the most recent non-zero 0x98 frame for the rest of this boot.
    uint8_t  lastFaultBytes[7] = {0};
    uint32_t lastFaultMs = 0;

    // Read-only protection settings from DALY Modbus registers 0x145..0x149.
    // These are diagnostic evidence, never control inputs: ESK8OS does not
    // expose a BMS settings-write path.
    bool     dischargeOcSettingsValid = false;
    uint32_t dischargeOcSettingsSeenMs = 0;
    float    dischargeOcWarningA = 0.0f;
    float    dischargeOcProtection1A = 0.0f;
    uint16_t dischargeOcProtection1DelayMs = 0;
    float    dischargeOcProtection2A = 0.0f;
    uint16_t dischargeOcProtection2DelayMs = 0;
};

extern BmsData gBms;

// A cell's mV is fresh if it was refreshed within this window of the pack's last
// good poll. Wider than one 500 ms cycle so a single dropped frame doesn't flap
// the display, tight enough to catch a cell that has genuinely stopped reporting.
static const uint32_t BMS_CELL_STALE_MS = 2000;
static const uint32_t BMS_FIELD_STALE_MS = 3000;
inline bool cellFresh(const BmsData& b, int cell) {
    return b.linkOk && b.cellSeenMs[cell] != 0 &&
           (b.lastOkMs - b.cellSeenMs[cell]) < BMS_CELL_STALE_MS;
}
inline bool bmsFieldFresh(const BmsData& b, BmsField field) {
    return b.linkOk && b.fieldSeenMs[field] != 0 &&
           (b.lastOkMs - b.fieldSeenMs[field]) < BMS_FIELD_STALE_MS;
}
inline uint16_t bmsFreshMask(const BmsData& b) {
    uint16_t mask = 0;
    for (int i = 0; i < BMS_FIELD_COUNT; i++) {
        if (bmsFieldFresh(b, (BmsField)i)) mask |= (uint16_t)(1u << i);
    }
    return mask;
}

// These bits have been verified against this physical pack/captured settings
// and DALY's official K/M/S BMSTool V1.14.60:
//   byte 0 bit 0 = cell-high Level 1, byte 0 bit 4 = pack-high Level 1.
//   byte 2 bit 2 = discharge-overcurrent warning.
//   byte 2 bit 3 = discharge-overcurrent protection 1.
// Preserve every other bit as an undifferentiated alert until it is verified.
inline BmsFaultKind bmsFaultKind(const uint8_t bytes[7]) {
    if (bytes[2] & 0x08) return BMS_FAULT_DISCHARGE_OVERCURRENT_PROTECTION;
    if (bytes[2] & 0x04) return BMS_FAULT_DISCHARGE_OVERCURRENT_WARNING;
    bool any = false;
    bool onlyHighL1 = true;
    for (int i = 0; i < 7; i++) {
        any |= bytes[i] != 0;
        const uint8_t known = (i == 0) ? 0x11 : 0x00;
        onlyHighL1 &= (bytes[i] & (uint8_t)~known) == 0;
    }
    if (!any) return BMS_FAULT_NONE;
    return onlyHighL1 ? BMS_FAULT_HIGH_VOLTAGE_L1 : BMS_FAULT_OTHER;
}

inline BmsFaultKind bmsFaultKind(const BmsData& b) {
    return bmsFaultKind(b.faultBytes);
}

inline BmsFaultKind bmsLastFaultKind(const BmsData& b) {
    return bmsFaultKind(b.lastFaultBytes);
}

inline const char* bmsFaultName(BmsFaultKind kind) {
    switch (kind) {
        case BMS_FAULT_NONE: return "none";
        case BMS_FAULT_HIGH_VOLTAGE_L1: return "high-voltage Level-1 warning";
        case BMS_FAULT_DISCHARGE_OVERCURRENT_WARNING:
            return "discharge overcurrent warning";
        case BMS_FAULT_DISCHARGE_OVERCURRENT_PROTECTION:
            return "discharge overcurrent protection 1";
        default: return "unclassified BMS alert";
    }
}

inline BmsAlarmLevel bmsAlarmLevel(const BmsData& b) {
    if (!bmsFieldFresh(b, BMS_FIELD_FAULT)) return BMS_ALARM_STALE;
    bool any = false;
    bool unknown = false;
    for (int i = 0; i < 7; i++) {
        any |= b.faultBytes[i] != 0;
        const uint8_t known = (i == 0) ? 0x11 :
                              (i == 2) ? 0x0C : 0x00;
        unknown |= (b.faultBytes[i] & (uint8_t)~known) != 0;
    }
    if (!any) return BMS_ALARM_NONE;
    // Even the pre-trip overcurrent warning is a serious live riding alert;
    // the adjacent protection bit is what opened the discharge MOS on the
    // 2026-07-30 validation ride.
    if (b.faultBytes[2] & 0x0C) return BMS_ALARM_ALERT;
    return unknown ? BMS_ALARM_ALERT : BMS_ALARM_WARNING;
}

// Atomic snapshot of the pack state. The ONLY supported way to read the BMS from
// another task — copies the published struct under a lock so the reader gets a
// coherent frame. Returns false (and a zeroed *out) on a build without the BMS.
bool getBmsData(BmsData* out); // selected driver facade
bool getDalyBmsData(BmsData* out); // Daly adapter snapshot

// Starts the BMS BLE-client poll task after companionBleBegin() has initialized
// the shared NimBLE stack. A no-op unless built with -DESK8OS_BMS_DALY.
void beginDalyBms();

// Load once after prefs.begin(); mutations run on the UI/console task only.
void loadDalyTarget();
DalyTarget getDalyTarget();
DalyTargetResult setDalyTarget(const char* address);

}  // namespace Transports
}  // namespace Esk8OS
