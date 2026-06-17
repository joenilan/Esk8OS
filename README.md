# Esk8 OS

A telemetry dashboard for an electric skateboard, running on a **LilyGo
T-Display S3** (ESP32-S3, 1.9" ST7789, 170×320). It reads live data from a
**VESC** over UART and renders speed, temperatures, range, and battery state
in an NZXT-CAM-inspired UI.

![Dashboard](image.png)

## Hardware

- **Board:** LilyGo T-Display S3 (ESP32-S3R8, 16MB flash / 8MB PSRAM)
- **Display:** ST7789 170×320, 8-bit parallel
- **Telemetry:** VESC over UART (RX 18 / TX 17)
- **Buttons:** GPIO0 (left), GPIO14 (right — toggles MPH/KM-H)

## Build & flash

Uses [PlatformIO](https://platformio.org/). Two environments:

| Environment | Target | Notes |
|---|---|---|
| `lilygo-t-display-s3` | Physical board | Real VESC over UART |
| `wokwi-simulator` | [Wokwi](https://wokwi.com/) | Generic ESP32 + ILI9341 stand-in, fake telemetry |

```bash
pio run -e lilygo-t-display-s3 -t upload     # flash the board
pio run -e wokwi-simulator                    # build for the simulator
```

> Wokwi has no 8-bit-parallel ST7789 support, so the simulator substitutes an
> SPI ILI9341 at 240×320. The UI is drawn in a centered 170px band, so what you
> see in that band matches the device.

## Design preview

`preview.py` renders the UI at the exact 170×320 panel resolution to a PNG, so
layout can be iterated without flashing hardware:

```bash
python preview.py            # dashboard
python preview.py --splash   # boot splash
python preview.py --kmh      # metric units
```

## Configuration

Rider name, product name, version, and default units live in the `USER CONFIG`
block at the top of `src/main.cpp`.

## Fonts

The UI uses [Bebas Neue](https://github.com/dharmatype/Bebas-Neue) by Dharma
Type, licensed under the SIL Open Font License 1.1 (see `OFL.txt`). The
`src/BebasNeue*.h` headers are GFX-format derivatives generated from
`BebasNeue.ttf`.
