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
2. **Backlight Enable (TFT_BL / GPIO 38):** The backlight must also be turned on explicitly.

Add this to the very top of `setup()`:
```cpp
// Power on the display (critical for T-Display-S3)
pinMode(15, OUTPUT);
digitalWrite(15, HIGH);

// Turn on the display backlight
pinMode(TFT_BL, OUTPUT);
digitalWrite(TFT_BL, HIGH);

// Initialize TFT
tft.init();
tft.setRotation(1); // Landscape
```

## 4. Recovering a Lost COM Port (Download Mode)
If the board is ever flashed without `ARDUINO_USB_CDC_ON_BOOT=1`, the COM port will vanish. Instruct the user to manually enter Download Mode:
1. Connect the board via USB.
2. Press and **hold** the **BOOT** button.
3. While holding BOOT, press and release the **RST** button.
4. Release the **BOOT** button.
The board will now appear as a USB Serial device and accept a new flash.
