#include "bridge.h"
#include "esk8os.h"
#include "wifi_bridge.h"
#include "ble_bridge.h"
#include "webexport.h"
#include "logging/sessionlog.h"
#include "transports/VescUartTransport.h"
#if ESK8OS_FULL_UI
#include "ui/BebasNeue18.h"
#include "ui/BebasNeue24.h"
#endif

// VESC Tool BRIDGE MODE coordinator. Bridge mode lets VESC Tool configure the
// ESC wirelessly through the display. This file owns the mode itself — the
// on-screen UI and the enter/exit transitions — and orchestrates the transport
// backends: WiFi-TCP for desktop (wifi_bridge.{h,cpp}) and BLE NUS for mobile
// (ble_bridge.{h,cpp}). Each loop it pumps both transports, reads the ESC UART
// once, and fans the reply out to whichever transport(s) are connected.

static const char* BRIDGE_BLE_NAME = "ESK8-BLE";   // name mobile VESC Tool scans for
static String      bridgeStatus    = "WAITING";
static unsigned long bridgeLastActive = 0;

static void updateBridgeStatus(const char* status) {
    bridgeStatus = status;
#if ESK8OS_FULL_UI
    GFX->fillRect(X0 + 8, 212, UI_W - 16, 38, COL_BG);
    GFX->drawRect(X0 + 8, 212, UI_W - 16, 38, COL_BORDER);
    uint16_t c = COL_DIM;
    if (strcmp(status, "CONNECTED") == 0 || strcmp(status, "TRAFFIC") == 0) c = COL_GREEN;
    else if (strcmp(status, "ERROR") == 0) c = COL_RED;
    GFX->setTextDatum(MC_DATUM);
    GFX->setTextColor(c);
    GFX->setFont(&BebasNeue24pt7b);
    GFX->drawString(status, X0 + UI_W / 2, 232);
    GFX->setFont(&fonts::Font0);
    pushCanvasFull();
#else
    Serial.print("[bridge] ");
    Serial.println(status);
#endif
}

// Live throughput per transport, just under the status box. The BLE line is a
// diagnostic: RX climbs as the phone's requests reach the ESC, TX as replies are
// forwarded back — so you can see exactly where a bridge session stalls.
static void updateBridgeStats() {
#if ESK8OS_FULL_UI
    GFX->fillRect(X0 + 8, 254, UI_W - 16, 26, COL_BG);
    GFX->setFont(&fonts::Font0);
    GFX->setTextDatum(MC_DATUM);

    GFX->setTextColor(COL_DIM);
    String wln = "WiFi RX " + String(wifiBridgeRxBytes() / 1024) + "K TX " +
                 String(wifiBridgeTxBytes() / 1024) + "K STA " + String(wifiBridgeStationNum());
    GFX->drawString(wln, X0 + UI_W / 2, 258);

    GFX->setTextColor(bleBridgeConnected() ? COL_GREEN : COL_DIM);
    String bln = String("BLE ") + (bleBridgeConnected() ? "ON" : "--") +
                 "  RX " + String(bleBridgeRxBytes()) + " TX " + String(bleBridgeTxBytes());
    GFX->drawString(bln, X0 + UI_W / 2, 270);
    pushCanvasFull();
#endif
}

static void drawBridgeScreen() {
#if ESK8OS_FULL_UI
    GFX->fillScreen(COL_BG);

    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_ACCENT);
    GFX->setFont(&BebasNeue24pt7b);
    GFX->drawString("BRIDGE MODE", X0 + UI_W / 2, 18);

    GFX->setFont(&fonts::Font0);
    GFX->setTextColor(COL_LABEL);
    GFX->drawString("VESC TOOL CONFIG", X0 + UI_W / 2, 56);

    GFX->setTextDatum(TL_DATUM);
    GFX->setTextColor(COL_DIM);  GFX->drawString("WiFi:",  X0 + 12, 80);
    GFX->setTextColor(COL_WHITE); GFX->drawString(wifiBridgeSsid(), X0 + 46, 80);
    GFX->setTextColor(COL_DIM);  GFX->drawString("pass:",  X0 + 12, 94);
    GFX->setTextColor(COL_WHITE); GFX->drawString(wifiBridgePass(), X0 + 46, 94);
    GFX->setTextColor(COL_DIM);  GFX->drawString("TCP:",   X0 + 12, 108);
    GFX->setTextColor(COL_WHITE);
    GFX->drawString(wifiBridgeIpPort(), X0 + 40, 108);
    GFX->setTextColor(COL_DIM);  GFX->drawString("BLE:",   X0 + 12, 122);
    GFX->setTextColor(COL_WHITE); GFX->drawString(BRIDGE_BLE_NAME, X0 + 40, 122);

    GFX->setTextColor(COL_DIM);
    GFX->drawString("Desktop: TCP connection", X0 + 12, 146);
    GFX->drawString("Mobile: scan BLE in app", X0 + 12, 158);

    // Ride-log download over the same AP.
    GFX->setTextColor(COL_DIM);   GFX->drawString("logs:", X0 + 12, 174);
    GFX->setTextColor(COL_WHITE); GFX->drawString("http://192.168.4.1", X0 + 44, 174);

    GFX->setTextDatum(TC_DATUM);
    GFX->setTextColor(COL_DIM);
    GFX->drawString("hold L+R to exit", X0 + UI_W / 2, 300);

    updateBridgeStatus(bridgeStatus.c_str());
    updateBridgeStats();                        // pushes the finished frame
#else
    Serial.println("[bridge] mode active");
    Serial.print("[bridge] wifi ");
    Serial.print(wifiBridgeSsid());
    Serial.print(" ");
    Serial.println(wifiBridgeIpPort());
#endif
}

static void bridgeStart() {
    bridgeStatus = "WAITING";
    bridgeLastActive = millis();
    wifiBridgeStart();
    // Enable BLE byte forwarding for mobile VESC Tool. The NUS service is already
    // live on the shared NimBLE server (built at boot by companion_ble); this just
    // flips its forwarding flag on. No-op on builds without BLE_BRIDGE_ENABLED.
    bleBridgeStart();
    // Serve board session CSVs for download on the same AP (http://192.168.4.1/).
    webExportStart();
}

static void bridgeStop() {
    webExportStop();
    wifiBridgeStop();
    bleBridgeStop();
}

// Pump both transports + fan ESC replies out to whichever are connected.
void bridgeLoop() {
    bool traffic = wifiBridgePoll();    // accept clients + app->ESC (WiFi)

    // ESC -> app: read the UART once and fan out to every connected transport, so
    // a BLE-only client still gets ESC replies (and we never double-read Serial1).
    bool wifiConn = wifiBridgeConnected();
    if (Serial1.available() > 0 && (wifiConn || bleBridgeConnected())) {
        uint8_t buf[256];
        int sa = Serial1.available();
        if (sa > (int)sizeof(buf)) sa = sizeof(buf);
        int r = Serial1.readBytes(buf, sa);
        if (r > 0) {
            wifiBridgeNotify(buf, r);   // no-ops internally if WiFi not connected
            bleBridgeNotify(buf, r);    // no-ops internally if BLE not connected
            traffic = true;
        }
    }
    bleBridgePoll();
    webExportHandle();          // serve any pending session-log download requests

    static unsigned long lastTrafficShown = 0;
    if (traffic && millis() - lastTrafficShown > 300) {
        lastTrafficShown = millis();
        updateBridgeStatus("TRAFFIC");
    }
    static bool wasConn = false;
    bool nowConn = wifiBridgeConnected() || bleBridgeConnected();
    if (nowConn && !wasConn) updateBridgeStatus("CONNECTED");
    if (wasConn && !nowConn) updateBridgeStatus("WAITING");
    wasConn = nowConn;

    // Refresh the throughput / station-count line a couple times a second.
    static unsigned long lastStats = 0;
    if (millis() - lastStats > 500) { lastStats = millis(); updateBridgeStats(); }

    if (gOtaInProgress) {
#if ESK8OS_FULL_UI
        static unsigned long lastOta = 0;
        if (millis() - lastOta > 100) {
            lastOta = millis();
            GFX->fillRect(X0 + 8, 140, UI_W - 16, 60, COL_ACCENT);
            GFX->setTextDatum(MC_DATUM);
            GFX->setTextColor(COL_BG);
            GFX->setFont(&BebasNeue24pt7b);
            GFX->drawString("UPDATING...", X0 + UI_W / 2, 154);
            GFX->setFont(&BebasNeue18pt7b);
            GFX->drawString(String(gOtaProgressPct) + "%", X0 + UI_W / 2, 184);
            pushCanvasFull();
        }
#endif
        bridgeLastActive = millis(); // Prevent timeout
        return;
    }

    // Auto-timeout if idle for 3 minutes
    if (nowConn || traffic) bridgeLastActive = millis();

    if (millis() - bridgeLastActive > 3 * 60 * 1000) {
        exitBridgeMode();
    }
}

void enterBridgeMode() {
    if (!DEMO_DATA && currentSpeedKmh > 1.0) {   // live safety: only when stopped
#if ESK8OS_FULL_UI
        GFX->fillRect(X0 + 8, 140, UI_W - 16, 40, COL_RED);
        GFX->setTextDatum(MC_DATUM);
        GFX->setTextColor(COL_WHITE);
        GFX->setFont(&BebasNeue24pt7b);
        GFX->drawString("STOP BOARD FIRST", X0 + UI_W / 2, 160);
        GFX->setFont(&fonts::Font0);
        pushCanvasFull();
        delay(1200);
        drawStaticFrame();
        gRedrawAll = true;
#else
        Serial.println("[bridge] blocked: stop board first");
#endif
        return;
    }
    // If the standalone log/OTA web service is up, drop it first — bridge mode
    // raises its own AP and serves the same pages, and two APs must not co-exist.
    if (webServiceActive()) webServiceStop();
    saveOdo();
    sessionLogMark("bridge_enter");
    sessionLogFlush();          // keep this power session as one CSV, but make it downloadable
    while (Serial1.available()) Serial1.read();   // drop stale VESC poll bytes so the
                                                  // first bridged packet starts clean
    Esk8OS::Transports::setVescPollPaused(true);  // STOP the background polling task
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
    sessionLogMark("bridge_exit");
    drawStaticFrame();
    gRedrawAll = true;
    Esk8OS::Transports::setVescPollPaused(false); // RESUME background polling
}
