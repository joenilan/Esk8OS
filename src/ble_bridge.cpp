#include "ble_bridge.h"

#if defined(BLE_BRIDGE_ENABLED) && !defined(WOKWI_SIMULATION)
// ---------------------------------------------------------------------------
// Real implementation (NimBLE — far lighter on flash/RAM than Bluedroid).
// ---------------------------------------------------------------------------
#include <NimBLEDevice.h>

// Nordic UART Service — the de-facto "serial over BLE" service VESC Tool uses.
static const char* NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write : app -> ESC
static const char* NUS_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify: ESC -> app

static NimBLEServer*         g_server = nullptr;
static NimBLECharacteristic* g_tx     = nullptr;
static volatile bool         g_connected = false;
static unsigned long         g_rx = 0;   // app -> ESC bytes
static unsigned long         g_tx_bytes = 0;   // ESC -> app bytes

// App writes to the RX characteristic -> push straight out the ESC UART.
class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (!v.empty()) {
            Serial1.write((const uint8_t*)v.data(), v.size());
            g_rx += v.size();
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override { g_connected = true; }
    void onDisconnect(NimBLEServer*) override {
        g_connected = false;
        NimBLEDevice::startAdvertising();   // let the next client reconnect
    }
};

void bleBridgeStart(const char* name) {
    if (g_server) return;                   // already running
    g_rx = 0; g_tx_bytes = 0; g_connected = false;

    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(517);              // request a large MTU for throughput

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());

    NimBLEService* svc = g_server->createService(NUS_SERVICE);
    g_tx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = svc->createCharacteristic(
        NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallbacks());
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    adv->start();
}

void bleBridgeStop() {
    if (!g_server) return;
    NimBLEDevice::deinit(true);             // stop + free the stack
    g_server = nullptr;
    g_tx = nullptr;
    g_connected = false;
}

void bleBridgeNotify(const uint8_t* data, size_t len) {
    if (!g_tx || !g_connected || len == 0) return;
    // Notify payloads are MTU-limited; chunk so long VESC replies get through.
    const size_t chunk = 180;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        g_tx->setValue(data + off, n);
        g_tx->notify();
        g_tx_bytes += n;
    }
}

void bleBridgePoll() { /* disconnect re-advertise handled in callback */ }
bool bleBridgeConnected() { return g_connected; }
unsigned long bleBridgeRxBytes() { return g_rx; }
unsigned long bleBridgeTxBytes() { return g_tx_bytes; }

#else
// ---------------------------------------------------------------------------
// No-op stubs (BLE disabled, or Wokwi build) so callers need no #ifdefs.
// ---------------------------------------------------------------------------
void bleBridgeStart(const char*) {}
void bleBridgeStop() {}
void bleBridgePoll() {}
void bleBridgeNotify(const uint8_t*, size_t) {}
bool bleBridgeConnected() { return false; }
unsigned long bleBridgeRxBytes() { return 0; }
unsigned long bleBridgeTxBytes() { return 0; }
#endif
