***

### 🚀 Agent Handoff: Esk8OS Firmware & Companion App

**Current State**
*   **Repository:** `Esk8OS`
*   **Active Branch:** `next-dev` (All changes are committed and pushed to `origin/next-dev`)
*   **Build Status:** Compiling successfully (PlatformIO environment: `tdisplay_s3_debug_usb`).

**What Was Just Accomplished**
1.  **VESC TCP/BLE Bridge Fixed:** Resolved a UART concurrency bug where the dashboard's background polling task was stealing bytes from the Bridge Mode. Bridge Mode now strictly pauses the background dashboard task.
2.  **BLE Buffer Overflow Fixed:** Added packet-pacing (`delay(2)`) to the BLE notification loop. VESC Tool/Floaty can now successfully pull massive payloads (like Motor Configs) without overflowing the ESP32's NimBLE queue.
3.  **UI & Font Overhaul:** Completely purged `FreeSans` from the UI. The entire interface now uses `BebasNeue` scaling. Fixed bounding-box math on critical overlays (`! FAULT`, `LOW BATTERY`) by utilizing `TC_DATUM` anchoring.
4.  **Dynamic Low Power Mode:** Added an automatic backlight dimmer to the main `App.cpp` loop. If the board battery drops to `<= 15%`, the screen gracefully dims to 10% brightness to save wattage.

**Next Steps / Directives for the Next Agent**
The user wants to do one of two things next:

*   **Option A: Merge to Main.** Review the changes in `next-dev` and handle the pull request / merge into the `main` branch.
*   **Option B: Start the Android Companion App.** 
    *   **CRITICAL:** Before writing any Android code, read the file at `docs/companion_api_spec.md`.
    *   This document outlines a custom BLE API specifically designed to stream pre-calculated JSON telemetry from the ESP32 to the phone (bypassing the complex VESC protocol entirely). 
    *   *Note to Agent:* The API Specification is drafted in the `docs/` folder, but the actual C++ implementation of this custom service (`companion_ble.cpp` + `ArduinoJson`) has **not** been built into the firmware yet. If you are starting the Companion App, you will need to implement both the Kotlin/Flutter side *and* hook up the C++ ESP32 side according to the spec!