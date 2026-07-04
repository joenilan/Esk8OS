#include "webexport.h"
#include "wifi_bridge.h"
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include "esk8os.h"
#include "logging/sessionlog.h"
#include "util/console.h"

// Instantiate OTA state globals
bool gOtaInProgress = false;
int  gOtaProgressPct = 0;

static WebServer   server(80);
static const char* SESSIONS_DIR = "/sessions";
static bool        g_running = false;

// Standalone web-service state (the AP-owning variant; see webServiceStart).
static bool          g_service       = false;
static unsigned long g_svcLastActive = 0;
static const unsigned long WEB_SVC_TIMEOUT_MS = 10UL * 60 * 1000;  // auto-drop AP after 10 min idle

// Index: a small dark page listing every board session CSV with a download link + size.
static void handleIndex() {
    String h;
    h.reserve(1024);
    h += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>ESK8OS session logs</title><style>"
           "body{font-family:system-ui,sans-serif;background:#1a1a1a;color:#eee;margin:0;padding:18px}"
           ".btn{display:inline-block;padding:10px 16px;background:#b950d7;color:#fff;border-radius:4px;text-decoration:none;border:none;cursor:pointer;font-size:16px;font-weight:bold;}"
           "h2{color:#b950d7;margin:0 0 12px}ul{list-style:none;padding:0}"
           "li{padding:10px 0;border-bottom:1px solid #333}a{color:#b950d7;text-decoration:none;font-size:18px}"
           ".s{color:#888;font-size:13px;margin-left:8px}.f{color:#888;margin-top:14px;font-size:13px}"
           ".s{color:#888;font-size:13px;margin-left:8px}.f{color:#888;margin-top:14px;font-size:13px}"
           "</style></head><body><h2>ESK8OS Update</h2>"
           "<form method='POST' action='/update' enctype='multipart/form-data' onsubmit='document.getElementById(\"sb\").value=\"Updating...\"'>"
           "<input type='file' name='update' accept='.bin' style='margin-bottom:12px'><br>"
           "<input type='submit' value='Update Firmware' class='btn' id='sb'></form>"
           "<hr style='border:1px solid #333;margin:20px 0'><h2>Board session logs</h2><ul>");

    int n = 0;
    File dir = LittleFS.open(SESSIONS_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String name = f.name();
            if (name.endsWith(".csv")) {
                h += "<li><a href='/dl?f=" + name + "'>" + name + "</a>"
                     "<span class=s>" + String(f.size()) + " B</span></li>";
                n++;
            }
            f.close();
        }
        dir.close();
    }
    if (n == 0) h += F("<li><span class=s>no sessions yet</span></li>");

    h += "</ul><div class=f>" + String((unsigned)(LittleFS.usedBytes() / 1024)) + " / " +
         String((unsigned)(LittleFS.totalBytes() / 1024)) + " KB used</div></body></html>";
    server.send(200, "text/html", h);
}

// Stream a single session CSV as a download. Name is constrained to /sessions.
static void handleDownload() {
    if (!server.hasArg("f")) { server.send(400, "text/plain", "missing f"); return; }
    String name = server.arg("f");
    if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0 || !name.endsWith(".csv")) {
        server.send(400, "text/plain", "bad name");
        return;
    }
    File f = LittleFS.open(String(SESSIONS_DIR) + "/" + name, "r");
    if (!f) { server.send(404, "text/plain", "not found"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=" + name);
    server.streamFile(f, "text/csv");
    f.close();
}

static void handleUpdateComplete() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "Update Failed" : "Update Success! Rebooting...");
    if (!Update.hasError()) {
        saveOdo();          // don't lose up to 60s of odometer/trip across the reboot
        sessionLogEnd();
        delay(1000);
        ESP.restart();
    }
    // Failed update: drop the UPDATING overlay so the dashboard (and the bridge
    // idle-timeout / web-service AP timeout) resume normally.
    gOtaInProgress = false;
    gRedrawAll = true;
}

static void handleUpdateUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        gOtaInProgress = true;
        gOtaProgressPct = 0;
        gRedrawAll = true;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        } else if (server.header("Content-Length").length() > 0) {
            // Approximate: multipart overhead makes this read slightly low, which
            // is fine — it never divides by zero and never exceeds 100.
            long total = server.header("Content-Length").toInt();
            if (total > 0) gOtaProgressPct = (int)min(100UL, (upload.totalSize * 100UL) / (unsigned long)total);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            gOtaProgressPct = 100;
        } else {
            Update.printError(Serial);
            gOtaInProgress = false;   // failed flash: clear the overlay state
            gRedrawAll = true;
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        gOtaInProgress = false;
        gRedrawAll = true;
    }
}

// ---- wireless console (/cmd + /console) ------------------------------------
// The board can't be on USB while the vehicle powers it (one 5V source at a
// time), so the serial console is unreachable exactly when the ESC is awake.
// This is the SAME console over the export/bridge AP: per-device WPA2
// password, explicitly started, idle auto-stop. GET/POST /cmd?c=<line> →
// plain-text reply; /console is a minimal phone-browser terminal.
static void handleCmd() {
    String c = server.hasArg("c") ? server.arg("c") : server.arg("plain");
    c.trim();
    if (!c.length()) { server.send(400, "text/plain", "usage: /cmd?c=<console command>  (try: /cmd?c=help)\n"); return; }
    if (c.length() > 90) { server.send(400, "text/plain", "command too long\n"); return; }
    String out;
    out.reserve(1024);
    consoleRunCapture(c.c_str(), out);
    server.send(200, "text/plain; charset=utf-8", out.length() ? out : "(no output)\n");
}

static void handleConsolePage() {
    server.send(200, "text/html",
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>EVEE console</title>"
        "<body style='background:#0e0e10;color:#e8e8e8;font:14px/1.45 monospace;margin:12px'>"
        "<pre id=o style='white-space:pre-wrap;word-break:break-word;min-height:60vh'>EVEE wireless console — type `help`\n</pre>"
        "<form id=f style='display:flex;gap:6px;position:sticky;bottom:8px'>"
        "<input id=i style='flex:1;background:#1a1a1a;color:#e8e8e8;border:1px solid #3a3a3c;padding:9px' autofocus autocomplete=off autocapitalize=off>"
        "<button style='background:#b950d7;color:#0e0e10;border:0;padding:9px 15px;font-weight:bold'>run</button></form>"
        "<script>f.onsubmit=async e=>{e.preventDefault();const c=i.value.trim();if(!c)return;i.value='';"
        "o.textContent+='\\n> '+c+'\\n';try{const r=await fetch('/cmd?c='+encodeURIComponent(c));"
        "o.textContent+=await r.text()}catch(_){o.textContent+='(request failed)\\n'}"
        "scrollTo(0,document.body.scrollHeight)};</script>");
}

void webExportStart() {
    if (g_running) return;
    // Needed for the OTA progress percent: WebServer only exposes headers that
    // were explicitly collected before begin().
    static const char* hdrs[] = { "Content-Length" };
    server.collectHeaders(hdrs, 1);
    server.on("/", HTTP_GET, handleIndex);
    server.on("/dl", HTTP_GET, handleDownload);
    server.on("/update", HTTP_POST, handleUpdateComplete, handleUpdateUpload);
    server.on("/cmd", handleCmd);                       // any method
    server.on("/console", HTTP_GET, handleConsolePage);
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();
    g_running = true;
}

void webExportStop() {
    if (!g_running) return;
    server.stop();
    g_running = false;
}

void webExportHandle() {
    if (g_running) server.handleClient();
}

// ── Standalone web service ──────────────────────────────────────────────────
// Raises its own AP (so it works unbridged) and serves the same routes. Mutually
// exclusive with VESC bridge mode, which raises its own AP + serves these pages
// itself — enterBridgeMode() stops this first if it's running.
void webServiceStart() {
    if (g_service) return;
    wifiApStart();
    webExportStart();
    g_service = true;
    g_svcLastActive = millis();
}

void webServiceStop() {
    if (!g_service) return;
    webExportStop();
    wifiApStop();
    g_service = false;
}

bool webServiceActive() { return g_service; }

void webServiceTick() {
    if (!g_service) return;
    server.handleClient();
    // Keep the AP up while a client is joined or an update is mid-flight; drop it
    // after a stretch of idle so we don't burn battery advertising forever.
    if (wifiBridgeStationNum() > 0 || gOtaInProgress) g_svcLastActive = millis();
    if (millis() - g_svcLastActive > WEB_SVC_TIMEOUT_MS) webServiceStop();
}
