#include "transports/DalyBms.h"
#include "transports/DalyModbus.h"
#include "esk8os.h"
#include "services/remote_link.h"

extern bool gDemoMode;   // Settings.cpp (global scope) — drives the bench simulation

#if ESK8OS_BMS_DALY
#include <NimBLEDevice.h>
#endif

namespace Esk8OS {
namespace Transports {

// Defined unconditionally so console.cpp and the display can link against it and
// simply show "link down" on a build without the BMS. The poll task below is the
// only part gated by the flag.
BmsData gBms;
static portMUX_TYPE sTargetMux = portMUX_INITIALIZER_UNLOCKED;
static DalyTarget sTarget;
static uint32_t sPublishedTargetRevision = 0; // guarded by gBmsMutex

DalyTarget getDalyTarget() {
    portENTER_CRITICAL(&sTargetMux);
    const DalyTarget target = sTarget;
    portEXIT_CRITICAL(&sTargetMux);
    return target;
}

// Guards the publish of gBms (poll task, core 0) against reads on the UI/BLE core.
// Created in beginDalyBms; stays null on a non-BMS build, where gBms never changes
// and an unlocked copy is safe. (F-2)
static SemaphoreHandle_t gBmsMutex = nullptr;

bool getDalyBmsData(BmsData* out) {
    uint32_t revision;
    if (gBmsMutex && xSemaphoreTake(gBmsMutex, portMAX_DELAY) == pdTRUE) {
        *out = gBms;
        revision = sPublishedTargetRevision;
        xSemaphoreGive(gBmsMutex);
    } else {
        *out = gBms;   // pre-begin, or non-BMS build: no concurrent writer
        revision = sPublishedTargetRevision;
    }
    // A target change invalidates every field immediately, even if an old poll
    // is still finishing. Never present one pack's cells as another pack's data.
    if (revision != getDalyTarget().revision) *out = BmsData{};
    return out->linkOk;
}

#if ESK8OS_BMS_DALY

void loadDalyTarget() {
    const String stored = prefs.getString("dalyAddr", "");
    char normalized[18] = {};
    normalizeDalyAddress(stored.c_str(), normalized); // corrupt -> unconfigured
    portENTER_CRITICAL(&sTargetMux);
    memcpy(sTarget.address, normalized, sizeof(normalized));
    ++sTarget.revision;
    portEXIT_CRITICAL(&sTargetMux);
}

DalyTargetResult setDalyTarget(const char* address) {
    char normalized[18] = {};
    if (!normalizeDalyAddress(address, normalized)) return DalyTargetResult::Invalid;
    if (!dalyTargetChangeAllowed(systemMode == MODE_DASHBOARD, gDemoMode,
            currentSpeedKmh, Esk8OS::RemoteLink::blocksRadioAndUart()))
        return DalyTargetResult::Busy;
    if (!strcmp(getDalyTarget().address, normalized)) return DalyTargetResult::Saved;
    // NVS is written only by the main task. A failed write leaves the old target
    // intact in RAM as well; other settings, ride history and calibration stay put.
    const bool stored = normalized[0]
        ? prefs.putString("dalyAddr", normalized) == strlen(normalized)
        : (!prefs.isKey("dalyAddr") || prefs.remove("dalyAddr"));
    if (!stored) return DalyTargetResult::StorageError;
    portENTER_CRITICAL(&sTargetMux);
    memcpy(sTarget.address, normalized, sizeof(normalized));
    ++sTarget.revision;
    portEXIT_CRITICAL(&sTargetMux);
    return DalyTargetResult::Saved;
}

static DalyTarget sActiveTarget; // only the BMS poll task owns the live connection

// Fallback until 0x94 tells us the real count — you said 10S, so a first pass of
// 0x95 (which needs a frame count) has something sane to work with.
#ifndef BMS_CELL_COUNT_DEFAULT
#define BMS_CELL_COUNT_DEFAULT 10
#endif

// Considered offline after this long without a good frame.
static const uint32_t BMS_STALE_MS = 3000;

static const uint8_t DALY_START = 0xA5;
static const uint8_t DALY_HOST  = 0x40;   // "this is the host talking"
static const uint8_t DALY_LEN   = 0x08;

// Command ids (Daly UART protocol V1.2).
enum {
    CMD_SOC        = 0x90,   // pack V / current / SOC
    CMD_MINMAX_V   = 0x91,   // min/max cell voltage + which cell
    CMD_MINMAX_T   = 0x92,   // min/max temperature
    CMD_MOS        = 0x93,   // charge/discharge FET state + remaining mAh
    CMD_STATUS     = 0x94,   // cell count, temp-sensor count, cycles
    CMD_CELL_V     = 0x95,   // per-cell voltages (multi-frame)
    CMD_CELL_T     = 0x96,   // per-sensor temps (multi-frame)
    CMD_BALANCE    = 0x97,   // per-cell balancing bitmap
    CMD_FAULT      = 0x98,   // protection / alarm flags
};

// ============================================================================
// BLE TRANSPORT — verified 2026-07-28 against the physical Daly "UART K"
// module advertising as ZOMBIESK8-BMS (D0:18:04:01:39:98).
//
// Service FFF0 exposes FFF1 notify ("spss_tx_desc", module -> central) and FFF2
// write/write-no-response ("spss_rx_desc", central -> module). A raw 0x90 Daly
// request written to FFF2 produced a checksum-valid 13-byte 0x90 notification
// on FFF1 without pairing or an application handshake.
//
// NimBLE is initialized once by companionBleBegin(). This module adds a central
// connection to that same stack while the companion phone service remains a
// peripheral/server. Reconnects never stop or rebuild phone advertising.
// ============================================================================
static const NimBLEUUID DALY_SERVICE_UUID((uint16_t)0xFFF0);
static const NimBLEUUID DALY_NOTIFY_UUID ((uint16_t)0xFFF1);
static const NimBLEUUID DALY_WRITE_UUID  ((uint16_t)0xFFF2);

static NimBLEClient*               sClient = nullptr;
static NimBLERemoteCharacteristic* sNotify = nullptr;
static NimBLERemoteCharacteristic* sWrite  = nullptr;
static volatile bool               sTransportReady = false;
static volatile bool               sDisconnected = false;

static const uint32_t RECONNECT_MIN_MS = 2000;
static const uint32_t RECONNECT_MAX_MS = 60000;
static const uint32_t DALY_SCAN_SECONDS = 4;
static const uint32_t DALY_COMMAND_GAP_MS = 100;
static uint32_t sReconnectAtMs = 0;
static uint32_t sReconnectBackoffMs = RECONNECT_MIN_MS;
static uint32_t sLastCommandAtMs = 0;
static bool     sSettingsReadPending = true;
static uint32_t sNextSettingsReadMs = 0;
static const uint32_t SETTINGS_INITIAL_DELAY_MS = 1500;
static const uint32_t SETTINGS_RETRY_MS = 30000;

// NimBLE notifications arrive on the host task while the Daly poller reads on
// core 0. Treat the characteristic as a byte stream: the physical unit sent one
// 13-byte frame per notification, but accepting split/coalesced notifications
// costs almost nothing and avoids coupling the decoder to ATT packet boundaries.
static const size_t RX_CAPACITY = 256;
static uint8_t      sRx[RX_CAPACITY];
static size_t       sRxHead = 0;
static size_t       sRxTail = 0;
static portMUX_TYPE sRxMux = portMUX_INITIALIZER_UNLOCKED;

static void clearRx() {
    portENTER_CRITICAL(&sRxMux);
    sRxHead = sRxTail = 0;
    portEXIT_CRITICAL(&sRxMux);
}

static bool popRx(uint8_t* out) {
    bool have = false;
    portENTER_CRITICAL(&sRxMux);
    if (sRxTail != sRxHead) {
        *out = sRx[sRxTail];
        sRxTail = (sRxTail + 1) % RX_CAPACITY;
        have = true;
    }
    portEXIT_CRITICAL(&sRxMux);
    return have;
}

static void notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data,
                           size_t length, bool) {
    portENTER_CRITICAL(&sRxMux);
    for (size_t i = 0; i < length; i++) {
        const size_t next = (sRxHead + 1) % RX_CAPACITY;
        if (next == sRxTail) {
            // Keep the newest bytes; readFrame() will resync at the next 0xA5.
            sRxTail = (sRxTail + 1) % RX_CAPACITY;
        }
        sRx[sRxHead] = data[i];
        sRxHead = next;
    }
    portEXIT_CRITICAL(&sRxMux);
}

class DalyClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*) override {
        sTransportReady = false;
        sDisconnected = true;
    }
};
static DalyClientCallbacks sClientCallbacks;

static void scheduleReconnect() {
    if (sReconnectAtMs != 0 && (int32_t)(millis() - sReconnectAtMs) < 0) return;
    sReconnectAtMs = millis() + sReconnectBackoffMs;
    sReconnectBackoffMs = min(sReconnectBackoffMs * 2, RECONNECT_MAX_MS);
}

static bool connectTransport() {
    if (!NimBLEDevice::getInitialized() || !sActiveTarget.address[0]) return false;
    if (sReconnectAtMs != 0 && (int32_t)(millis() - sReconnectAtMs) < 0) return false;

    sTransportReady = false;
    sNotify = nullptr;
    sWrite = nullptr;
    clearRx();

    // Resolve the selected address from a live advertisement (public or random). Besides
    // avoiding blind connection attempts at an out-of-range module, passing the
    // advertised device to NimBLE preserves the address type reported on-air.
    // Scanning shares the controller with the companion peripheral role; it does
    // not stop advertising or disconnect an attached companion phone.
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    scan->setInterval(90);
    scan->setWindow(45);
    scan->setMaxResults(20);

    Serial.printf("[bms] scanning %lu s for %s\n",
                  (unsigned long)DALY_SCAN_SECONDS, sActiveTarget.address);
    NimBLEScanResults results = scan->start(DALY_SCAN_SECONDS, false);
    NimBLEAdvertisedDevice* advertised = nullptr;
    for (auto* candidate : results) {
        if (candidate->getAddress().toString() == sActiveTarget.address) {
            advertised = candidate;
            break;
        }
    }
    if (!advertised) {
        Serial.printf("[bms] target not seen among %d devices; retry in %lu s\n",
                      results.getCount(),
                      (unsigned long)(sReconnectBackoffMs / 1000));
        scan->clearResults();
        scheduleReconnect();
        return false;
    }

    Serial.printf("[bms] found %s, RSSI %d dBm, address type %u\n",
                  advertised->haveName() ? advertised->getName().c_str() : "(unnamed)",
                  advertised->getRSSI(),
                  (unsigned)advertised->getAddressType());

    if (!sClient) {
        sClient = NimBLEDevice::createClient();
        if (!sClient) {
            Serial.println("[bms] NimBLE client allocation failed");
            scan->clearResults();
            scheduleReconnect();
            return false;
        }
        sClient->setClientCallbacks(&sClientCallbacks, false);
        sClient->setConnectTimeout(5);
    }

    Serial.printf("[bms] connecting %s\n", sActiveTarget.address);
    // NimBLE 1.4.x: the second argument is deleteAttributes (services are
    // re-discovered every connect anyway, since sNotify/sWrite are re-resolved
    // below). NimBLE 2.x renames this parameter to asyncConnect — re-check the
    // signature before any library bump or the GATT setup silently breaks.
    if (!sClient->connect(advertised, false)) {
        const int error = sClient->getLastError();
        Serial.printf("[bms] connect failed: %d (%s); retry in %lu s "
                      "(Daly app may own the link)\n",
                      error, NimBLEUtils::returnCodeToString(error),
                      (unsigned long)(sReconnectBackoffMs / 1000));
        scan->clearResults();
        scheduleReconnect();
        return false;
    }
    scan->clearResults();

    NimBLERemoteService* service = sClient->getService(DALY_SERVICE_UUID);
    if (service) {
        sNotify = service->getCharacteristic(DALY_NOTIFY_UUID);
        sWrite  = service->getCharacteristic(DALY_WRITE_UUID);
    }
    if (!sNotify || !sWrite || !sNotify->canNotify() ||
        (!sWrite->canWrite() && !sWrite->canWriteNoResponse())) {
        const int error = sClient->getLastError();
        Serial.printf("[bms] verified FFF0/FFF1/FFF2 GATT path not found: %d (%s)\n",
                      error, NimBLEUtils::returnCodeToString(error));
        sClient->disconnect();
        scheduleReconnect();
        return false;
    }
    if (!sNotify->subscribe(true, notifyCallback, false)) {
        const int error = sClient->getLastError();
        Serial.printf("[bms] FFF1 notification subscribe failed: %d (%s)\n",
                      error, NimBLEUtils::returnCodeToString(error));
        sClient->disconnect();
        scheduleReconnect();
        return false;
    }

    // The UART bridge needs a short quiet interval after CCCD setup before its
    // first serial request. Without it, a quick reconnect can succeed at GAP/
    // GATT level but silently discard the first Daly command.
    delay(500);
    // The link can die inside that settle window (the Daly app stealing the
    // connection, module reset). Re-check before declaring ready, so a dead
    // link goes through the normal backoff instead of an un-backoffed scan loop.
    if (!sClient->isConnected()) {
        sTransportReady = false;
        sDisconnected = false;
        scheduleReconnect();
        return false;
    }
    sDisconnected = false;
    sTransportReady = true;
    sReconnectAtMs = 0;
    sSettingsReadPending = true;
    sNextSettingsReadMs = millis() + SETTINGS_INITIAL_DELAY_MS;
    Serial.printf("[bms] BLE ready: %s FFF0 / FFF1 notify / FFF2 write\n",
                  sClient->getPeerAddress().toString().c_str());
    return true;
}

static bool ensureTransport() {
    if (sTransportReady && sClient && sClient->isConnected()) return true;
    if (sDisconnected) {
        sDisconnected = false;
        sNotify = nullptr;
        sWrite = nullptr;
        clearRx();
        Serial.printf("[bms] BLE disconnected; retry in %lu s\n",
                      (unsigned long)(sReconnectBackoffMs / 1000));
        scheduleReconnect();
    }
    return connectTransport();
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// The poll task is the only caller. Pace both legacy and read-only Modbus
// requests like the UART behind FFF2, and give each request sole ownership of
// the notification byte stream until its response is complete.
static bool sendBytes(const uint8_t* data, size_t length) {
    if (!sTransportReady || !sWrite || !sClient || !sClient->isConnected()) return false;

    const uint32_t sinceLastCommand = millis() - sLastCommandAtMs;
    if (sinceLastCommand < DALY_COMMAND_GAP_MS) {
        delay(DALY_COMMAND_GAP_MS - sinceLastCommand);
    }

    // Although FFF2 advertises both forms, the physical UART K firmware only
    // produced Daly replies for write-without-response. Pace those writes like
    // the UART they bridge instead of trusting ATT acknowledgements for flow.
    const bool requestAttResponse = !sWrite->canWriteNoResponse();
    if (!sWrite->writeValue(data, length, requestAttResponse)) {
        sTransportReady = false;
        sClient->disconnect();
        return false;
    }
    // Every query owns the byte stream from the moment its request is on the
    // air. Clearing only after a successful write means a late reply from the
    // PREVIOUS command (one that outlived its own read timeout) is discarded
    // instead of being consumed as this command's response.
    clearRx();
    sLastCommandAtMs = millis();
    return true;
}

// One request frame: A5 40 CMD 08 00×8 CHK, checksum = sum of the first 12 & 0xFF.
static bool sendRequest(uint8_t cmd) {
    uint8_t f[13] = { DALY_START, DALY_HOST, cmd, DALY_LEN, 0,0,0,0, 0,0,0,0, 0 };
    uint16_t sum = 0;
    for (int i = 0; i < 12; i++) sum += f[i];
    f[12] = (uint8_t)(sum & 0xFF);
    return sendBytes(f, sizeof(f));
}

// Read one validated 13-byte response for `cmd`. Rescans on a bad start byte,
// wrong command, or bad checksum — a corrupt frame is never partially used.
static bool readFrame(uint8_t cmd, uint8_t out[13], uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        uint8_t byte = 0;
        if (!popRx(&byte)) { delay(1); continue; }
        if (byte != DALY_START) continue;

        out[0] = DALY_START;
        int got = 1;
        while (got < 13 && (millis() - start) < timeoutMs) {
            if (popRx(&byte)) out[got++] = byte;
            else delay(1);
        }
        if (got < 13) return false;

        if (out[1] != 0x01 || out[2] != cmd) continue;   // not the reply we asked for
        uint16_t sum = 0;
        for (int i = 0; i < 12; i++) sum += out[i];
        if ((uint8_t)(sum & 0xFF) != out[12]) continue;   // bad checksum, keep scanning
        return true;
    }
    return false;
}

// A single-frame command: request, read one frame, hand back the 8 data bytes.
static bool query(uint8_t cmd, uint8_t data[8], uint32_t timeoutMs = 300) {
    if (!sendRequest(cmd)) return false;
    uint8_t f[13];
    if (!readFrame(cmd, f, timeoutMs)) return false;
    memcpy(data, &f[4], 8);
    return true;
}

// ---- decoders -------------------------------------------------------------

static bool readPack(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_SOC, d)) return false;
    b.packVoltage = be16(&d[0]) / 10.0f;
    b.current     = ((int)be16(&d[4]) - 30000) / 10.0f;   // 30000 offset, 0.1 A
    b.soc         = be16(&d[6]) / 10.0f;
    return true;
}

static bool readMinMaxV(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_MINMAX_V, d)) return false;
    b.maxCellmV = be16(&d[0]); b.maxCellNo = d[2];
    b.minCellmV = be16(&d[3]); b.minCellNo = d[5];
    b.cellDeltamV = (b.maxCellmV >= b.minCellmV) ? (b.maxCellmV - b.minCellmV) : 0;
    return true;
}

// Read the five official discharge-overcurrent registers in one Modbus RTU
// function-03 request. Expected response:
//   51 03 0A [five big-endian uint16 registers] CRC_LO CRC_HI
// The fixed, validated length also keeps a malformed byte count from consuming
// arbitrary notification data.
static bool readDischargeOvercurrentSettings(BmsData& b) {
    using namespace DalyModbus;
    uint8_t request[READ_REQUEST_SIZE];
    buildDischargeOvercurrentRead(request);
    if (!sendBytes(request, sizeof(request))) return false;

    uint8_t frame[DISCHARGE_OC_RESPONSE_SIZE];
    size_t got = 0;
    const uint32_t started = millis();
    while (millis() - started < 500) {
        uint8_t byte = 0;
        if (!popRx(&byte)) {
            delay(1);
            continue;
        }
        if (got == 0 && byte != RESPONSE_ADDRESS) continue;
        frame[got++] = byte;

        // A valid exception response is five bytes: address, function|0x80,
        // exception code, CRC. Consume it, report it, and leave telemetry live.
        if (got == 2 && frame[1] == (READ_HOLDING_REGISTERS | 0x80)) {
            while (got < 5 && millis() - started < 500) {
                if (popRx(&byte)) frame[got++] = byte;
                else delay(1);
            }
            if (got == 5) {
                const uint16_t expected = crc16(frame, 3);
                const uint16_t received =
                    (uint16_t)(frame[3] | ((uint16_t)frame[4] << 8));
                if (expected == received) {
                    Serial.printf("[bms] settings read Modbus exception 0x%02X\n",
                                  frame[2]);
                }
            }
            return false;
        }

        if (got == 2 && frame[1] != READ_HOLDING_REGISTERS) {
            got = 0;   // wrong protocol/function; resume at the next 0x51
            continue;
        }
        if (got == 3 && frame[2] != DISCHARGE_OC_REGISTER_COUNT * 2) {
            return false;
        }
        if (got < sizeof(frame)) continue;

        DischargeOvercurrentRegisters regs;
        if (!decodeDischargeOvercurrentResponse(frame, sizeof(frame), &regs)) {
            return false;
        }
        b.dischargeOcWarningA = dischargeCurrentAmps(regs.warningRaw);
        b.dischargeOcProtection1A = dischargeCurrentAmps(regs.protection1Raw);
        b.dischargeOcProtection1DelayMs = regs.protection1DelayMs;
        b.dischargeOcProtection2A = dischargeCurrentAmps(regs.protection2Raw);
        b.dischargeOcProtection2DelayMs = regs.protection2DelayMs;
        b.dischargeOcSettingsSeenMs = millis();
        b.dischargeOcSettingsValid = true;
        return true;
    }
    return false;
}

static bool readMinMaxT(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_MINMAX_T, d)) return false;
    b.tempMax = (int8_t)((int)d[0] - 40);
    b.tempMin = (int8_t)((int)d[2] - 40);
    return true;
}

static bool readMos(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_MOS, d)) return false;
    b.chargeMos    = d[1] != 0;
    b.dischargeMos = d[2] != 0;
    const uint32_t mah = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                         ((uint32_t)d[6] << 8)  | d[7];
    b.remainingAh = mah / 1000.0f;
    return true;
}

static bool readStatus(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_STATUS, d)) return false;
    if (d[0] > 0 && d[0] <= BMS_MAX_CELLS) b.cellCount = d[0];
    if (d[1] <= BMS_MAX_TEMPS)             b.tempCount = d[1];
    b.cycles = be16(&d[5]);
    return true;
}

// 0x95 — per-cell voltages, three cells per frame, byte 0 of the data is the
// 1-based frame number. Read ceil(cells/3) frames.
static bool readCellVoltages(BmsData& b) {
    const int cells = b.cellCount ? b.cellCount : BMS_CELL_COUNT_DEFAULT;
    const int frames = (cells + 2) / 3;
    uint32_t seenMask = 0;

    if (!sendRequest(CMD_CELL_V)) return false;
    for (int fi = 0; fi < frames; fi++) {
        uint8_t f[13];
        if (!readFrame(CMD_CELL_V, f, 300)) return false;
        const int frameNo = f[4];
        if (frameNo < 1) continue;
        const int base = (frameNo - 1) * 3;
        for (int i = 0; i < 3; i++) {
            const int cell = base + i;
            if (cell >= BMS_MAX_CELLS || cell >= cells) break;
            b.cellmV[cell] = be16(&f[5 + i * 2]);
            b.cellSeenMs[cell] = millis();   // stamp freshness so a later dropped
                                             // frame leaves this cell visibly stale (F-3)
            seenMask |= (1UL << cell);
        }
    }

    const uint32_t expectedMask = cells >= 32 ? 0xFFFFFFFFUL : ((1UL << cells) - 1UL);
    if ((seenMask & expectedMask) != expectedMask) return false;

    // Prefer one coherent 0x95 set for the display extrema. The separate 0x91
    // reply is sampled earlier in the poll and can legitimately differ by a few
    // millivolts while charging; deriving here keeps cell number/value/delta
    // internally consistent.
    uint16_t mn = 0xFFFF, mx = 0;
    uint8_t mnNo = 0, mxNo = 0;
    for (int c = 0; c < cells; c++) {
        if (b.cellmV[c] < mn) { mn = b.cellmV[c]; mnNo = (uint8_t)(c + 1); }
        if (b.cellmV[c] > mx) { mx = b.cellmV[c]; mxNo = (uint8_t)(c + 1); }
    }
    b.minCellmV = mn; b.minCellNo = mnNo;
    b.maxCellmV = mx; b.maxCellNo = mxNo;
    b.cellDeltamV = mx >= mn ? (uint16_t)(mx - mn) : 0;
    return true;
}

// 0x96 — temperatures, up to seven per frame, each with a 40 offset.
static bool readCellTemps(BmsData& b) {
    const int temps = b.tempCount ? b.tempCount : 1;
    const int frames = (temps + 6) / 7;
    uint16_t seenMask = 0;

    if (!sendRequest(CMD_CELL_T)) return false;
    for (int fi = 0; fi < frames; fi++) {
        uint8_t f[13];
        if (!readFrame(CMD_CELL_T, f, 300)) return false;
        const int frameNo = f[4];
        if (frameNo < 1) continue;
        const int base = (frameNo - 1) * 7;
        for (int i = 0; i < 7; i++) {
            const int t = base + i;
            if (t >= BMS_MAX_TEMPS || t >= temps) break;
            b.temps[t] = (int8_t)((int)f[5 + i] - 40);
            seenMask |= (1u << t);
        }
    }
    const uint16_t expectedMask = (uint16_t)((1u << temps) - 1u);
    return (seenMask & expectedMask) == expectedMask;
}

static bool readBalance(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_BALANCE, d)) return false;
    const int cells = b.cellCount ? b.cellCount : BMS_CELL_COUNT_DEFAULT;
    for (int c = 0; c < cells && c < BMS_MAX_CELLS; c++) {
        b.balancing[c] = (d[c / 8] >> (c % 8)) & 0x01;
    }
    return true;
}

static bool readFaults(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_FAULT, d)) return false;
    bool any = false;
    for (int i = 0; i < 7; i++) { b.faultBytes[i] = d[i]; if (d[i]) any = true; }
    b.hasFault = any;
    if (any) {
        for (int i = 0; i < 7; i++) b.lastFaultBytes[i] = d[i];
        b.lastFaultMs = millis();
    }
    return true;
}

// ---- bench simulation -----------------------------------------------------
// Synthetic pack so the whole BMS surface (display page, BLE, app) animates on
// the bench with no Daly wired — the same role simulateTelemetry() plays for the
// VESC. A 10S pack with ONE deliberately weak cell (cell 7, ~70 mV low) so the
// per-cell imbalance view — the reason this feature exists — is visible in demo.
static void simulateBms(BmsData& b) {
    const uint32_t now = millis();
    const float phase = (now % 60000) / 60000.0f;          // 0..1 over 60 s
    const float wave  = sinf(phase * 2.0f * PI);           // -1..1

    b.cellCount = 10;
    b.tempCount = 2;
    b.soc = 70.0f + 15.0f * wave;                          // breathe 55..85%

    const uint16_t nominal = (uint16_t)(3600 + (b.soc - 55.0f) / 30.0f * 400.0f);  // ~3600..4000 mV
    uint32_t sum = 0;
    for (int c = 0; c < b.cellCount; c++) {
        int16_t spread = (int16_t)((c * 7) % 15) - 7;       // deterministic +/-7 mV
        uint16_t mv = (uint16_t)(nominal + spread);
        if (c == 6) mv -= 70;                               // the weak cell (1-based #7)
        b.cellmV[c]     = mv;
        b.cellSeenMs[c] = now;                              // demo cells always fresh
        sum += mv;
    }
    b.packVoltage = sum / 1000.0f;

    uint16_t mn = 0xFFFF, mx = 0; uint8_t mnNo = 1, mxNo = 1;
    for (int c = 0; c < b.cellCount; c++) {
        if (b.cellmV[c] < mn) { mn = b.cellmV[c]; mnNo = (uint8_t)(c + 1); }
        if (b.cellmV[c] > mx) { mx = b.cellmV[c]; mxNo = (uint8_t)(c + 1); }
    }
    b.minCellmV = mn; b.maxCellmV = mx; b.minCellNo = mnNo; b.maxCellNo = mxNo;
    b.cellDeltamV = (uint16_t)(mx - mn);

    b.current     = -(10.0f + 8.0f * wave);                 // discharge (negative per decode)
    b.remainingAh = 10.0f * b.soc / 100.0f;
    b.cycles      = 42;
    b.temps[0] = (int8_t)(26 + (int)(4 * wave));
    b.temps[1] = (int8_t)(28 + (int)(3 * wave));
    b.tempMax  = max(b.temps[0], b.temps[1]);
    b.tempMin  = min(b.temps[0], b.temps[1]);
    b.chargeMos = true; b.dischargeMos = true;
    b.hasFault = false;
    for (int i = 0; i < 7; i++) {
        b.faultBytes[i] = 0;
        b.lastFaultBytes[i] = 0;
    }
    b.lastFaultMs = 0;
    b.linkOk = true;
    b.lastOkMs = now;
    for (int i = 0; i < BMS_FIELD_COUNT; i++) b.fieldSeenMs[i] = now;
}

// ---- poll task ------------------------------------------------------------

static void bmsPollTask(void*) {
    // Local scratch so a half-finished poll never publishes a torn mix of old and
    // new; the finished frame is swapped into gBms under gBmsMutex (F-2).
    uint8_t consecutivePackFailures = 0;
    bool wasDemo = false;
    for (;;) {
        BmsData b = gBms;   // carry forward last topology (cell/temp counts)
        const DalyTarget desired = getDalyTarget();
        if (desired.revision != sActiveTarget.revision) {
            // Only this task ever disconnects/reconfigures the BMS client.
            if (sClient && sClient->isConnected()) sClient->disconnect();
            sTransportReady = false;
            sDisconnected = false;
            sNotify = nullptr;
            sWrite = nullptr;
            clearRx();
            sActiveTarget = desired;
            sReconnectAtMs = 0;
            sReconnectBackoffMs = RECONNECT_MIN_MS;
            sSettingsReadPending = true;
            sNextSettingsReadMs = 0;
            consecutivePackFailures = 0;
            wasDemo = false;
            b = BmsData{};
        }

        if (gDemoMode) {
            simulateBms(b);
            consecutivePackFailures = 0;
            wasDemo = true;
        } else if (!sActiveTarget.address[0]) {
            b = BmsData{}; // unconfigured means no scanning or inherited demo data
            wasDemo = false;
        } else if (ensureTransport()) {
            if (wasDemo) {
                sSettingsReadPending = true;
                sNextSettingsReadMs = millis() + SETTINGS_INITIAL_DELAY_MS;
                wasDemo = false;
            }
            if (readPack(b)) {
                consecutivePackFailures = 0;
                // A GATT connection is not proof that the UART bridge is
                // healthy. Reset backoff only after checksum-valid telemetry.
                sReconnectBackoffMs = RECONNECT_MIN_MS;
                b.fieldSeenMs[BMS_FIELD_PACK] = millis();
                if (readStatus(b))       b.fieldSeenMs[BMS_FIELD_STATUS] = millis();
                if (readMinMaxV(b))      b.fieldSeenMs[BMS_FIELD_MINMAX_V] = millis();
                if (readMinMaxT(b))      b.fieldSeenMs[BMS_FIELD_MINMAX_T] = millis();
                if (readMos(b))          b.fieldSeenMs[BMS_FIELD_MOS] = millis();
                if (readCellVoltages(b)) b.fieldSeenMs[BMS_FIELD_CELLS] = millis();
                if (readCellTemps(b))    b.fieldSeenMs[BMS_FIELD_TEMPS] = millis();
                if (readBalance(b))      b.fieldSeenMs[BMS_FIELD_BALANCE] = millis();
                if (readFaults(b))       b.fieldSeenMs[BMS_FIELD_FAULT] = millis();
                if (sSettingsReadPending &&
                    (int32_t)(millis() - sNextSettingsReadMs) >= 0) {
                    if (readDischargeOvercurrentSettings(b)) {
                        sSettingsReadPending = false;
                        Serial.printf(
                            "[bms] read-only discharge O/C settings: "
                            "warn %.1fA, protect1 %.1fA/%ums, "
                            "protect2 %.1fA/%ums\n",
                            b.dischargeOcWarningA,
                            b.dischargeOcProtection1A,
                            (unsigned)b.dischargeOcProtection1DelayMs,
                            b.dischargeOcProtection2A,
                            (unsigned)b.dischargeOcProtection2DelayMs);
                    } else {
                        sNextSettingsReadMs = millis() + SETTINGS_RETRY_MS;
                        Serial.println(
                            "[bms] read-only discharge O/C settings unavailable; "
                            "telemetry continues, retry in 30 s");
                    }
                }
                b.linkOk = true;
                b.lastOkMs = millis();
            } else if (++consecutivePackFailures >= 6) {
                // A connected-but-silent UART bridge is just as dead as a GAP
                // disconnect. Tear it down after the same 3 s staleness window
                // so the normal exponential reconnect path can recover it.
                consecutivePackFailures = 0;
                Serial.println("[bms] six pack queries timed out; reconnecting");
                sTransportReady = false;
                if (sClient && sClient->isConnected()) sClient->disconnect();
            }
        }
        if (!gDemoMode && millis() - b.lastOkMs > BMS_STALE_MS) {
            b.linkOk = false;
        }

        if (gBmsMutex && xSemaphoreTake(gBmsMutex, portMAX_DELAY) == pdTRUE) {
            gBms = b;
            sPublishedTargetRevision = sActiveTarget.revision;
            xSemaphoreGive(gBmsMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(500));   // 2 Hz; cell voltages do not move fast
    }
}

void beginDalyBms() {
    gBmsMutex = xSemaphoreCreateMutex();
    if (!NimBLEDevice::getInitialized()) {
        Serial.println("[bms] NimBLE must be initialized before beginDalyBms()");
        return;
    }
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Low priority on core 0: the BMS is not time-critical, and if EVEE Link is
    // present its throttle task (high priority, same core) must always win.
    xTaskCreatePinnedToCore(bmsPollTask, "DalyBMS", 6144, nullptr, 1, nullptr, 0);
}

#else   // !ESK8OS_BMS_DALY

void beginDalyBms() {}   // stub; gBms stays linkOk = false
void loadDalyTarget() {} // preserve NVS when switching to a non-BMS build
DalyTargetResult setDalyTarget(const char*) { return DalyTargetResult::NotBuilt; }

#endif

}  // namespace Transports
}  // namespace Esk8OS
