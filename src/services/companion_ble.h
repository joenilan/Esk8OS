#pragma once
#include <Arduino.h>

// ============================================================================
// Companion BLE service — see docs/companion_api_spec.md.
//
// Exposes a custom BLE service (base UUID 5043697A-…) that streams display-ready
// JSON telemetry to a phone app, accepts settings reads/writes, and accepts
// ASCII command strings. Unlike the VESC-Tool bridge (a raw NUS byte pipe), this
// is the ESP32 acting as the master: it parses the VESC, runs the math, and
// broadcasts the finished values.
//
// OWNERSHIP: this module owns the entire NimBLE stack. companionBleBegin() inits
// NimBLE once in setup() and the Companion Telemetry service advertises 100% of
// the time. The VESC-Tool NUS bridge (ble_bridge.{h,cpp}) is registered onto the
// SAME server here, so entering VESC Bridge mode just flips the bridge's
// forwarding flag — both services co-exist on one server, no init/deinit churn.
//
// THREADING: BLE characteristic callbacks run in the NimBLE task. They only stash
// incoming writes; companionBleTick() (called from the UI loop) applies settings,
// dispatches commands, and pushes telemetry — so all GFX/flash work stays on the
// UI thread.
//
// Real on hardware when BLE_BRIDGE_ENABLED is defined; no-ops otherwise (Wokwi).
// ============================================================================

void companionBleBegin();   // init NimBLE, build companion + bridge services, advertise
void companionBleTick();    // call from the dashboard loop: 5 Hz telemetry + apply queued writes

// The BLE settings machinery, exposed for the serial console so `set`/`json`
// share ONE code path with the app (same keys, same validation, spec §4).
void companionApplySettingsJson(const char* json);       // apply a (partial) settings JSON
void companionSettingsJson(char* out, size_t cap);       // current settings as JSON
void companionBaseConfJson(char* out, size_t cap);       // VESC base + provenance as JSON
