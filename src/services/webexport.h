#pragma once

// ============================================================================
// Ride-log web export — a tiny HTTP server that lists and serves the LittleFS
// ride CSVs for download over the bridge-mode WiFi AP. Join the ESK8-BRIDGE
// network and open http://192.168.4.1/ to see the rides + download links.
//
// Runs alongside the WiFi-TCP VESC bridge (port 80 here, 65102 there) and is
// driven by the bridge coordinator (bridge.cpp): started/stopped with the AP,
// pumped each loop while bridging. Download-only — log deletion stays on the
// device / serial console.
// ============================================================================

void webExportStart();    // register routes + begin serving (AP must be up)
void webExportStop();     // stop the server
void webExportHandle();   // pump pending HTTP requests; call each loop

// Standalone web service: bring up the AP + HTTP server (logs + OTA) WITHOUT
// entering VESC bridge mode, so the dashboard keeps running and telemetry/BLE
// stay live ("hybrid" transfer). In bridge mode the same pages are already
// served over the bridge AP, so these are for the unbridged case.
void webServiceStart();   // raise AP + start serving
void webServiceStop();    // stop serving + drop AP
bool webServiceActive();  // standalone service running?
void webServiceTick();    // pump HTTP + idle auto-timeout; call each dashboard loop
