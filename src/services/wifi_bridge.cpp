#include "wifi_bridge.h"
#include <WiFi.h>

// VESC Tool WiFi-TCP transport. Owns the softAP credentials + TCP server and
// forwards raw bytes between the TCP client (desktop VESC Tool) and Serial1 (the
// ESC UART). See wifi_bridge.h for how this slots under the bridge coordinator.

static const char*    WIFI_SSID = "ESK8-BRIDGE";
static const char*    WIFI_PASS = "esk8bridge";   // must be >= 8 chars
static const uint16_t WIFI_PORT = 65102;

static WiFiServer    g_server(WIFI_PORT);
static WiFiClient    g_client;
static unsigned long g_rx = 0;   // app -> ESC, bytes this session
static unsigned long g_tx = 0;   // ESC -> app, bytes this session

void wifiBridgeStart() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    g_server.begin();
    g_server.setNoDelay(true);
    g_rx = 0;
    g_tx = 0;
}

void wifiBridgeStop() {
    if (g_client) g_client.stop();
    g_server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

// Accept a pending client + pump app->ESC. Returns true if bytes were forwarded.
bool wifiBridgePoll() {
    if (!g_client || !g_client.connected()) {
        WiFiClient nc = g_server.available();
        if (nc) g_client = nc;
    }
    if (g_client && g_client.connected()) {
        int n = g_client.available();
        if (n > 0) {
            uint8_t buf[256];
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            int r = g_client.read(buf, n);
            if (r > 0) { Serial1.write(buf, r); g_rx += r; return true; }
        }
    }
    return false;
}

void wifiBridgeNotify(const uint8_t* data, size_t len) {
    if (!g_client || !g_client.connected() || len == 0) return;
    g_client.write(data, len);
    g_tx += len;
}

bool wifiBridgeConnected() { return g_client && g_client.connected(); }
unsigned long wifiBridgeRxBytes() { return g_rx; }
unsigned long wifiBridgeTxBytes() { return g_tx; }

const char* wifiBridgeSsid() { return WIFI_SSID; }
const char* wifiBridgePass() { return WIFI_PASS; }
String wifiBridgeIpPort() { return WiFi.softAPIP().toString() + ":" + String(WIFI_PORT); }
int wifiBridgeStationNum() { return WiFi.softAPgetStationNum(); }
