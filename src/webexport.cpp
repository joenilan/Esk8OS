#include "webexport.h"
#include <WebServer.h>
#include <LittleFS.h>

static WebServer   server(80);
static const char* RIDES_DIR = "/rides";
static bool        g_running = false;

// Index: a small dark page listing every ride CSV with a download link + size.
static void handleIndex() {
    String h;
    h.reserve(1024);
    h += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>ESK8OS ride logs</title><style>"
           "body{font-family:system-ui,sans-serif;background:#1a1a1a;color:#eee;margin:0;padding:18px}"
           "h2{color:#b950d7;margin:0 0 12px}ul{list-style:none;padding:0}"
           "li{padding:10px 0;border-bottom:1px solid #333}a{color:#b950d7;text-decoration:none;font-size:18px}"
           ".s{color:#888;font-size:13px;margin-left:8px}.f{color:#888;margin-top:14px;font-size:13px}"
           "</style></head><body><h2>ESK8OS ride logs</h2><ul>");

    int n = 0;
    File dir = LittleFS.open(RIDES_DIR);
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
    if (n == 0) h += F("<li><span class=s>no rides yet</span></li>");

    h += "</ul><div class=f>" + String((unsigned)(LittleFS.usedBytes() / 1024)) + " / " +
         String((unsigned)(LittleFS.totalBytes() / 1024)) + " KB used</div></body></html>";
    server.send(200, "text/html", h);
}

// Stream a single ride CSV as a download. Name is constrained to /rides.
static void handleDownload() {
    if (!server.hasArg("f")) { server.send(400, "text/plain", "missing f"); return; }
    String name = server.arg("f");
    if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0 || !name.endsWith(".csv")) {
        server.send(400, "text/plain", "bad name");
        return;
    }
    File f = LittleFS.open(String(RIDES_DIR) + "/" + name, "r");
    if (!f) { server.send(404, "text/plain", "not found"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=" + name);
    server.streamFile(f, "text/csv");
    f.close();
}

void webExportStart() {
    if (g_running) return;
    server.on("/", handleIndex);
    server.on("/dl", handleDownload);
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
