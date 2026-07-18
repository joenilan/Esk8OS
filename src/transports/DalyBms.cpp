#include "transports/DalyBms.h"

extern bool gDemoMode;   // Settings.cpp (global scope) — drives the bench simulation

namespace Esk8OS {
namespace Transports {

// Defined unconditionally so console.cpp and the display can link against it and
// simply show "link down" on a build without the BMS. The poll task below is the
// only part gated by the flag.
BmsData gBms;

// Guards the publish of gBms (poll task, core 0) against reads on the UI/BLE core.
// Created in beginDalyBms; stays null on a non-BMS build, where gBms never changes
// and an unlocked copy is safe. (F-2)
static SemaphoreHandle_t gBmsMutex = nullptr;

bool getBmsData(BmsData* out) {
    if (gBmsMutex && xSemaphoreTake(gBmsMutex, portMAX_DELAY) == pdTRUE) {
        *out = gBms;
        xSemaphoreGive(gBmsMutex);
    } else {
        *out = gBms;   // pre-begin, or non-BMS build: no concurrent writer
    }
    return out->linkOk;
}

#if ESK8OS_BMS_DALY

#ifndef BMS_RX_PIN
#define BMS_RX_PIN 44          // ESP RX  <- BMS TX
#endif
#ifndef BMS_TX_PIN
#define BMS_TX_PIN 43          // ESP TX  -> BMS RX
#endif
#ifndef BMS_BAUD
#define BMS_BAUD 9600
#endif

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

static HardwareSerial* sPort = nullptr;

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// One request frame: A5 40 CMD 08 00×8 CHK, checksum = sum of the first 12 & 0xFF.
static void sendRequest(uint8_t cmd) {
    uint8_t f[13] = { DALY_START, DALY_HOST, cmd, DALY_LEN, 0,0,0,0, 0,0,0,0, 0 };
    uint16_t sum = 0;
    for (int i = 0; i < 12; i++) sum += f[i];
    f[12] = (uint8_t)(sum & 0xFF);
    while (sPort->available()) sPort->read();   // drop anything stale first
    sPort->write(f, 13);
    sPort->flush();
}

// Read one validated 13-byte response for `cmd`. Rescans on a bad start byte,
// wrong command, or bad checksum — a corrupt frame is never partially used.
static bool readFrame(uint8_t cmd, uint8_t out[13], uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (!sPort->available()) { delay(1); continue; }
        if ((uint8_t)sPort->read() != DALY_START) continue;

        out[0] = DALY_START;
        int got = 1;
        while (got < 13 && (millis() - start) < timeoutMs) {
            if (sPort->available()) out[got++] = (uint8_t)sPort->read();
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
static bool query(uint8_t cmd, uint8_t data[8], uint32_t timeoutMs = 120) {
    sendRequest(cmd);
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

static void readMinMaxV(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_MINMAX_V, d)) return;
    b.maxCellmV = be16(&d[0]); b.maxCellNo = d[2];
    b.minCellmV = be16(&d[3]); b.minCellNo = d[5];
    b.cellDeltamV = (b.maxCellmV >= b.minCellmV) ? (b.maxCellmV - b.minCellmV) : 0;
}

static void readMinMaxT(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_MINMAX_T, d)) return;
    b.tempMax = (int8_t)((int)d[0] - 40);
    b.tempMin = (int8_t)((int)d[2] - 40);
}

static void readMos(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_MOS, d)) return;
    b.chargeMos    = d[1] != 0;
    b.dischargeMos = d[2] != 0;
    const uint32_t mah = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) |
                         ((uint32_t)d[6] << 8)  | d[7];
    b.remainingAh = mah / 1000.0f;
}

static void readStatus(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_STATUS, d)) return;
    if (d[0] > 0 && d[0] <= BMS_MAX_CELLS) b.cellCount = d[0];
    if (d[1] <= BMS_MAX_TEMPS)             b.tempCount = d[1];
    b.cycles = be16(&d[5]);
}

// 0x95 — per-cell voltages, three cells per frame, byte 0 of the data is the
// 1-based frame number. Read ceil(cells/3) frames.
static void readCellVoltages(BmsData& b) {
    const int cells = b.cellCount ? b.cellCount : BMS_CELL_COUNT_DEFAULT;
    const int frames = (cells + 2) / 3;

    sendRequest(CMD_CELL_V);
    for (int fi = 0; fi < frames; fi++) {
        uint8_t f[13];
        if (!readFrame(CMD_CELL_V, f, 150)) break;
        const int frameNo = f[4];
        if (frameNo < 1) continue;
        const int base = (frameNo - 1) * 3;
        for (int i = 0; i < 3; i++) {
            const int cell = base + i;
            if (cell >= BMS_MAX_CELLS || cell >= cells) break;
            b.cellmV[cell] = be16(&f[5 + i * 2]);
            b.cellSeenMs[cell] = millis();   // stamp freshness so a later dropped
                                             // frame leaves this cell visibly stale (F-3)
        }
    }
}

// 0x96 — temperatures, up to seven per frame, each with a 40 offset.
static void readCellTemps(BmsData& b) {
    const int temps = b.tempCount ? b.tempCount : 1;
    const int frames = (temps + 6) / 7;

    sendRequest(CMD_CELL_T);
    for (int fi = 0; fi < frames; fi++) {
        uint8_t f[13];
        if (!readFrame(CMD_CELL_T, f, 150)) break;
        const int frameNo = f[4];
        if (frameNo < 1) continue;
        const int base = (frameNo - 1) * 7;
        for (int i = 0; i < 7; i++) {
            const int t = base + i;
            if (t >= BMS_MAX_TEMPS || t >= temps) break;
            b.temps[t] = (int8_t)((int)f[5 + i] - 40);
        }
    }
}

static void readBalance(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_BALANCE, d)) return;
    const int cells = b.cellCount ? b.cellCount : BMS_CELL_COUNT_DEFAULT;
    for (int c = 0; c < cells && c < BMS_MAX_CELLS; c++) {
        b.balancing[c] = (d[c / 8] >> (c % 8)) & 0x01;
    }
}

static void readFaults(BmsData& b) {
    uint8_t d[8];
    if (!query(CMD_FAULT, d)) return;
    bool any = false;
    for (int i = 0; i < 7; i++) { b.faultBytes[i] = d[i]; if (d[i]) any = true; }
    b.hasFault = any;
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
    for (int i = 0; i < 7; i++) b.faultBytes[i] = 0;
    b.linkOk = true;
    b.lastOkMs = now;
}

// ---- poll task ------------------------------------------------------------

static void bmsPollTask(void*) {
    // Local scratch so a half-finished poll never publishes a torn mix of old and
    // new; the finished frame is swapped into gBms under gBmsMutex (F-2).
    for (;;) {
        BmsData b = gBms;   // carry forward last topology (cell/temp counts)

        if (gDemoMode) {
            simulateBms(b);
        } else if (readPack(b)) {
            readStatus(b);       // counts first — the cell/temp reads need them
            readMinMaxV(b);
            readMinMaxT(b);
            readMos(b);
            readCellVoltages(b);
            readCellTemps(b);
            readBalance(b);
            readFaults(b);
            b.linkOk = true;
            b.lastOkMs = millis();
        } else if (millis() - b.lastOkMs > BMS_STALE_MS) {
            b.linkOk = false;
        }

        if (gBmsMutex && xSemaphoreTake(gBmsMutex, portMAX_DELAY) == pdTRUE) {
            gBms = b;
            xSemaphoreGive(gBmsMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(500));   // 2 Hz; cell voltages do not move fast
    }
}

void beginDalyBms() {
    gBmsMutex = xSemaphoreCreateMutex();
    static HardwareSerial bmsSerial(2);   // UART2, its own pins, off the VESC bus
    sPort = &bmsSerial;
    sPort->begin(BMS_BAUD, SERIAL_8N1, BMS_RX_PIN, BMS_TX_PIN);

    // Low priority on core 0: the BMS is not time-critical, and if EVEE Link is
    // present its throttle task (high priority, same core) must always win.
    xTaskCreatePinnedToCore(bmsPollTask, "DalyBMS", 4096, nullptr, 1, nullptr, 0);
}

#else   // !ESK8OS_BMS_DALY

void beginDalyBms() {}   // stub; gBms stays linkOk = false

#endif

}  // namespace Transports
}  // namespace Esk8OS
