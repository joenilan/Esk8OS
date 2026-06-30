# LILYGO T-Display-S3 (ESP32-S3) - Agent Instructions

This document contains critical instructions and quirks for working with the **LILYGO T-Display-S3 (1.9 inch ST7789 LCD)** board. Read this before attempting to modify or flash code to this device.

## 1. Hardware Specifications & Interfaces
* **Microcontroller:** ESP32-S3R8 (Dual-core LX7 microprocessor)
* **Memory:** 16MB Flash / 8MB PSRAM (Requires specific board configuration in PlatformIO to fully utilize)
* **Display:** 1.9-inch TFT LCD (ST7789 driver)
* **Display Interface:** **8-Bit Parallel** (Do NOT use standard SPI pins like MOSI/MISO/SCLK. This display uses `TFT_D0` through `TFT_D7`).

## 2. PlatformIO Configuration (`platformio.ini`)
The `TFT_eSPI` library must be configured directly via `build_flags` in `platformio.ini`. Using `-D USER_SETUP_LOADED=1` overrides the library's default `User_Setup.h`.

**CRITICAL FLAGS:**
* `-D ARDUINO_USB_MODE=1` and `-D ARDUINO_USB_CDC_ON_BOOT=1`: The ESP32-S3 uses native USB. If these flags are missing, the USB COM port will disappear after a successful flash, locking the user out.
* `-D CGRAM_OFFSET=1`: Required for the ST7789 driver on this specific display, otherwise pixels will be mapped out of bounds.
* `-D TFT_PARALLEL_8_BIT=1`: Required because this board uses parallel data lines instead of SPI.

**Full Pin Configuration:**
```ini
build_flags = 
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D USER_SETUP_LOADED=1
    -D ST7789_DRIVER=1
    -D CGRAM_OFFSET=1
    -D TFT_WIDTH=170
    -D TFT_HEIGHT=320
    -D TFT_RGB_ORDER=TFT_RGB
    -D TFT_INVERSION_ON=1
    -D TFT_PARALLEL_8_BIT=1
    -D TFT_CS=6
    -D TFT_DC=7
    -D TFT_RST=5
    -D TFT_WR=8
    -D TFT_RD=9
    -D TFT_D0=39
    -D TFT_D1=40
    -D TFT_D2=41
    -D TFT_D3=42
    -D TFT_D4=45
    -D TFT_D5=46
    -D TFT_D6=47
    -D TFT_D7=48
    -D TFT_BL=38
    -D TFT_BACKLIGHT_ON=HIGH
    -D LOAD_GLCD=1
    -D LOAD_FONT2=1
    -D LOAD_FONT4=1
    -D SMOOTH_FONT=1
```

## 3. C++ Initialization Requirements
If you get a "black screen" but the code seems to be running, it's a power issue.

1. **Display Power Enable (GPIO 15):** The T-Display-S3 requires GPIO 15 to be set `HIGH` to physically power the display rail.
2. **Backlight Enable (TFT_BL / GPIO 38):** The backlight must also be turned on explicitly. To avoid a brief white/glitch frame at boot, keep `TFT_BL` LOW until after `tft.init()`, `tft.setRotation(...)`, and an initial black `fillScreen(...)`.

Add this to the very top of `setup()`:
```cpp
// Power on the display (critical for T-Display-S3)
pinMode(15, OUTPUT);
digitalWrite(15, HIGH);

// Keep backlight off while the panel initializes/clears
pinMode(TFT_BL, OUTPUT);
digitalWrite(TFT_BL, LOW);

// Initialize TFT
tft.init();
tft.setRotation(1); // Landscape
tft.fillScreen(TFT_BLACK);

// Turn on the display backlight after the panel is clean
digitalWrite(TFT_BL, HIGH);
```

## 4. Recovering a Lost COM Port (Download Mode)
If the board is ever flashed without `ARDUINO_USB_CDC_ON_BOOT=1`, the COM port will vanish. Instruct the user to manually enter Download Mode:
1. Connect the board via USB.
2. Press and **hold** the **BOOT** button.
3. While holding BOOT, press and release the **RST** button.
4. Release the **BOOT** button.
The board will now appear as a USB Serial device and accept a new flash.

## 5. COM Port Present But Serial Console Silent
If Windows shows the LilyGO as `USB Serial Device (COMx)` and esptool can identify/flash it, the ESP32 USB interface is probably not dead even if `sys`/`cfg` get no response.

Known root cause: older boot code blocked forever in `waitForBootReady()` when persisted `demo OFF` and no VESC UART response was available. Because `consolePoll()` only runs after `setup()` finishes, the board looked like serial was dead. This was fixed by adding a VESC boot-wait timeout in `src/ui/ui.cpp`; after about 3.5 seconds it continues with `NO VESC - BLE READY` and the console works.

Debug sequence:
1. Check ports with PowerShell: `[System.IO.Ports.SerialPort]::GetPortNames()`.
2. If a COM port exists, try flashing `tdisplay_s3_debug_usb` before assuming hardware failure.
3. Open serial at `115200` with DTR/RTS not asserted and send `sys` then `cfg`.
4. If opening with RTS asserted prints an `ESP-ROM` banner, USB reset/ROM output is alive.
5. If serial is silent only until the VESC wait timeout, inspect VESC UART wiring or set `demo on`.

Important: `tdisplay_s3_ride_release` intentionally removes USB CDC with `-UARDUINO_USB_CDC_ON_BOOT`. Use `tdisplay_s3_debug_usb` when Claude/Codex needs serial.

## 6. VESC / FSESC UART Wiring
The firmware expects the FSESC UART on:
* ESP32 `VESC_RX_PIN` = GPIO 18, connected to FSESC TX
* ESP32 `VESC_TX_PIN` = GPIO 17, connected to FSESC RX
* Shared GND is required
* The display can be powered from the comm port 5V pin if the port can supply enough current

Do not connect the display to SPI display pins for telemetry. The display uses its own 8-bit parallel TFT pins; the FSESC telemetry/config connection is only the UART pair above.

## 7. Dashboard vs VESC Tool Bridge Mode
The firmware now has two top-level modes:
* `MODE_DASHBOARD`: normal ride UI. This mode owns `VescUart` and may call `UART.getVescValues()`.
* `MODE_VESC_BRIDGE`: WiFi TCP bridge for desktop VESC Tool. This mode owns `Serial1` directly and forwards raw bytes between TCP and the FSESC.

Critical rule: never allow dashboard polling and bridge forwarding to run at the same time. `UART.getVescValues()` must not be called while bridge mode is active, because mixed packets can corrupt VESC Tool config reads/writes.

Current bridge controls:
* Hold both buttons for about 2 seconds to enter bridge mode.
* Hold both buttons again for about 2 seconds to exit bridge mode.
* Entry is blocked if `currentSpeedKmh > 1.0` during live telemetry; stop the board first. Demo/simulated telemetry does not block bridge entry.
* Bridge AP: `ESK8-BRIDGE`
* Bridge password: `esk8bridge`
* TCP endpoint: `192.168.4.1:65102`

Bridge mode is a first-step TCP proof for desktop VESC Tool. Phone/mobile VESC Tool compatibility will likely need a BLE backend later; do not assume the WiFi TCP bridge works with every mobile VESC Tool build.

## 8. Demo Mode and Wheel Profiles
`DEMO_MODE` in `src/main.cpp` makes the physical board use simulated telemetry for bench testing. Set it to `false` when testing live dashboard telemetry from the FSESC.

Wheel/gearing profiles are local display settings only. They affect the display's own speed and distance math and are saved in NVS, but they do not write VESC motor/app configuration. Use bridge mode plus official VESC Tool for actual FSESC config changes.
