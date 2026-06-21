#include "ble_bridge.h"

#if defined(BLE_BRIDGE_ENABLED) && !defined(WOKWI_SIMULATION)
// ---------------------------------------------------------------------------
// Real implementation (NimBLE — far lighter on flash/RAM than Bluedroid).
// The NimBLE stack itself is owned by companion_ble.cpp; this file only adds
// the Nordic-UART (NUS) service onto that shared server and forwards bytes.
// ---------------------------------------------------------------------------
#include <NimBLEDevice.h>

// Nordic UART Service — the de-facto "serial over BLE" service VESC Tool uses.
static const char* NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write : app -> ESC
static const char* NUS_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify: ESC -> app

static NimBLEServer*         g_server     = nullptr;   // shared, owned by companion_ble
static NimBLECharacteristic* g_tx         = nullptr;
static volatile bool         g_forwarding = false;     // true only while VESC Bridge mode is active
static unsigned long         g_rx         = 0;         // app -> ESC bytes
static unsigned long         g_tx_bytes   = 0;         // ESC -> app bytes

// App writes to the RX characteristic -> push straight out the ESC UART. Ignored
// unless bridge mode owns the UART, so a stray write can't fight the dashboard's
// background VESC polling task.
class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!g_forwarding) return;
        std::string v = c->getValue();
        if (!v.empty()) {
            Serial1.write((const uint8_t*)v.data(), v.size());
            g_rx += v.size();
        }
    }
};

void bleBridgeRegister(NimBLEServer* server, NimBLEAdvertising* /*adv*/) {
    if (!server || g_server) return;            // already registered
    g_server = server;

    NimBLEService* svc = server->createService(NUS_SERVICE);
    g_tx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallbacks());
    svc->start();
    // Advertising (incl. this UUID in the scan response) is configured by
    // companion_ble after registration, so all GATT exists before adv starts.
}

const char* bleBridgeServiceUuid() { return NUS_SERVICE; }

void bleBridgeStart() {                          // VESC Bridge mode entered
    g_rx = 0; g_tx_bytes = 0;
    g_forwarding = true;
}

void bleBridgeStop() {                           // VESC Bridge mode exited
    g_forwarding = false;
}

void bleBridgeNotify(const uint8_t* data, size_t len) {
    if (!g_forwarding || !g_tx || !g_server || g_server->getConnectedCount() == 0 || len == 0) return;
    // Chunk at 20 bytes — the universally safe BLE notify size. A client that
    // doesn't negotiate a larger ATT_MTU caps notify payloads at MTU-3 = 20, so
    // anything bigger SILENTLY TRUNCATES and VESC Tool / Floaty sees corrupt
    // packets. VESC's own BLE modules chunk at 20 for exactly this reason; the
    // VESC protocol is a byte stream, so the app's parser reassembles the chunks.
    const size_t chunk = 20;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        g_tx->setValue(data + off, n);
        g_tx->notify();
        g_tx_bytes += n;
        delay(2); // give the BLE stack time to dispatch; avoids queue-overflow drops
    }
}

void bleBridgePoll() { /* re-advertise on disconnect handled by companion_ble */ }
bool bleBridgeConnected() { return g_forwarding && g_server && g_server->getConnectedCount() > 0; }
unsigned long bleBridgeRxBytes() { return g_rx; }
unsigned long bleBridgeTxBytes() { return g_tx_bytes; }

#else
// ---------------------------------------------------------------------------
// No-op stubs (BLE disabled, or Wokwi build) so callers need no #ifdefs.
// ---------------------------------------------------------------------------
void bleBridgeRegister(NimBLEServer*, NimBLEAdvertising*) {}
const char* bleBridgeServiceUuid() { return ""; }
void bleBridgeStart() {}
void bleBridgeStop() {}
void bleBridgePoll() {}
void bleBridgeNotify(const uint8_t*, size_t) {}
bool bleBridgeConnected() { return false; }
unsigned long bleBridgeRxBytes() { return 0; }
unsigned long bleBridgeTxBytes() { return 0; }
#endif
