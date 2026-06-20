#include "bridge.h"
#include "esk8os.h"
#include "ble_bridge.h"
#include <WiFi.h>

// VESC Tool WiFi-TCP bridge state. Forwards raw bytes between a TCP client
// (desktop VESC Tool > TCP connection) and Serial1 (the ESC UART).
static WiFiServer    bridgeServer(65102);
static WiFiClient    bridgeClient;
static const char*   BRIDGE_SSID     = "ESK8-BRIDGE";
static const char*   BRIDGE_PASS     = "esk8bridge";   // must be >= 8 chars
static const char*   BRIDGE_BLE_NAME = "ESK8-BLE";     // name mobile VESC Tool scans for
static String        bridgeStatus    = "WAITING";
static unsigned long bridgeRxBytes   = 0;   // VESC Tool -> ESC, bytes this session
static unsigned long bridgeTxBytes   = 0;   // ESC -> VESC Tool, bytes this session

static void updateBridgeStatus(const char* status) {
    bridgeStatus = status;
    GFX->fillRect(X0 + 8, 212, UI_W - 16, 38, COL_BG);
    GFX->drawRect(X0 + 8, 212, UI_W - 16, 38, COL_BORDER);
    uint16_t c = COL_DIM;
    if (strcmp(status, "CONNECTED") == 0 || strcmp(status, "TRAFFIC") == 0) c = COL_GREEN;
    else if (strcmp(status, "ERROR") == 0) c = COL_RED;
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(c);
    GFX->setFont(&fonts::FreeSansBold12pt7b);
    GFX->drawString(status, X0 + UI_W / 2, 232);
    GFX->setFont(&fonts::Font0);
    pushCanvasFull();
}

// Live throughput + connected-station count, just under the status box.
static void updateBridgeStats() {
    GFX->fillRect(X0 + 8, 254, UI_W - 16, 12, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(COL_DIM);
    String s = "RX " + String(bridgeRxBytes / 1024) + "K  TX " + String(bridgeTxBytes / 1024) +
               "K  STA " + String(WiFi.softAPgetStationNum());
    GFX->drawString(s, X0 + UI_W / 2, 260);
    pushCanvasFull();
}

static void drawBridgeScreen() {
    GFX->fillScreen(COL_BG);

    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_ACCENT);
    GFX->setFont(&fonts::FreeSansBold12pt7b);
    GFX->drawString("BRIDGE MODE", X0 + UI_W / 2, 18);

    GFX->setFont(&fonts::Font0);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString("VESC TOOL CONFIG", X0 + UI_W / 2, 56);

    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);  GFX->drawString("WiFi:",  X0 + 12, 80);
    GFX->setTextColor(COL_WHITE); GFX->drawString(BRIDGE_SSID, X0 + 46, 80);
    GFX->setTextColor(COL_DIM);  GFX->drawString("pass:",  X0 + 12, 94);
    GFX->setTextColor(COL_WHITE); GFX->drawString(BRIDGE_PASS, X0 + 46, 94);
    GFX->setTextColor(COL_DIM);  GFX->drawString("TCP:",   X0 + 12, 108);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString(WiFi.softAPIP().toString() + ":65102", X0 + 40, 108);
    GFX->setTextColor(COL_DIM);  GFX->drawString("BLE:",   X0 + 12, 122);
    GFX->setTextColor(COL_WHITE); GFX->drawString(BRIDGE_BLE_NAME, X0 + 40, 122);

    GFX->setTextColor(COL_DIM);
    GFX->drawString("Desktop: TCP connection", X0 + 12, 146);
    GFX->drawString("Mobile: scan BLE in app", X0 + 12, 158);

    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("hold L+R to exit", X0 + UI_W / 2, 300);

    updateBridgeStatus(bridgeStatus.c_str());
    updateBridgeStats();                        // pushes the finished frame
}

static void bridgeStart() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(BRIDGE_SSID, BRIDGE_PASS);
    bridgeServer.begin();
    bridgeServer.setNoDelay(true);
    bridgeStatus = "WAITING";
    bridgeRxBytes = 0;
    bridgeTxBytes = 0;
    // Also advertise the BLE (Nordic UART) backend for mobile VESC Tool. No-op
    // on builds without BLE_BRIDGE_ENABLED. Runs alongside the WiFi backend.
    bleBridgeStart(BRIDGE_BLE_NAME);
}

static void bridgeStop() {
    if (bridgeClient) bridgeClient.stop();
    bridgeServer.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    bleBridgeStop();
}

// Raw byte forwarding between the TCP client (VESC Tool) and Serial1 (FSESC).
void bridgeLoop() {
    if (!bridgeClient || !bridgeClient.connected()) {
        WiFiClient nc = bridgeServer.available();
        if (nc) { bridgeClient = nc; updateBridgeStatus("CONNECTED"); }
    }

    uint8_t buf[256];
    bool traffic = false;

    // app -> ESC (WiFi). The BLE app->ESC path runs in the BLE write callback.
    if (bridgeClient && bridgeClient.connected()) {
        int n = bridgeClient.available();
        if (n > 0) {
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            int r = bridgeClient.read(buf, n);
            if (r > 0) { Serial1.write(buf, r); bridgeRxBytes += r; traffic = true; }
        }
    }
    // ESC -> app: read the UART once and fan out to whichever backend is live, so
    // a BLE-only client still gets ESC replies (and we never double-read Serial1).
    bool wifiConn = bridgeClient && bridgeClient.connected();
    int sa = Serial1.available();
    if (sa > 0 && (wifiConn || bleBridgeConnected())) {
        if (sa > (int)sizeof(buf)) sa = sizeof(buf);
        int r = Serial1.readBytes(buf, sa);
        if (r > 0) {
            if (wifiConn) { bridgeClient.write(buf, r); bridgeTxBytes += r; }
            bleBridgeNotify(buf, r);
            traffic = true;
        }
    }
    bleBridgePoll();

    static unsigned long lastTrafficShown = 0;
    if (traffic && millis() - lastTrafficShown > 300) {
        lastTrafficShown = millis();
        updateBridgeStatus("TRAFFIC");
    }
    static bool wasConn = false;
    bool nowConn = (bridgeClient && bridgeClient.connected()) || bleBridgeConnected();
    if (nowConn && !wasConn) updateBridgeStatus("CONNECTED");
    if (wasConn && !nowConn) updateBridgeStatus("WAITING");
    wasConn = nowConn;

    // Refresh the throughput / station-count line a couple times a second.
    static unsigned long lastStats = 0;
    if (millis() - lastStats > 500) { lastStats = millis(); updateBridgeStats(); }
}

void enterBridgeMode() {
    if (!DEMO_DATA && currentSpeedKmh > 1.0) {   // live safety: only when stopped
        GFX->fillRect(X0 + 8, 140, UI_W - 16, 40, COL_RED);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_WHITE);
        GFX->setFont(&fonts::FreeSans12pt7b);
        GFX->drawString("STOP BOARD FIRST", X0 + UI_W / 2, 160);
        GFX->setFont(&fonts::Font0);
        pushCanvasFull();
        delay(1200);
        drawStaticFrame();
        gRedrawAll = true;
        return;
    }
    saveOdo();
    systemMode = MODE_VESC_BRIDGE;
    bridgeStart();
    drawBridgeScreen();
}

void exitBridgeMode() {
    bridgeStop();
    while (Serial1.available()) Serial1.read();   // flush stale VESC replies
    systemMode = MODE_DASHBOARD;
    lastVescOkMs = millis();
    currentPage = PAGE_HUD;
    drawStaticFrame();
    gRedrawAll = true;
}
