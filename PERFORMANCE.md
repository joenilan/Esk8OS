# Performance & Rendering Notes

This document records how the ESK8 OS display renders, the optimizations applied
so far, the hardware realities we hit, and the roadmap for squeezing more out of
the LilyGo T-Display-S3. It's here so other DIYers can understand the tradeoffs
and build on them.

## Hardware

- **MCU:** ESP32-S3 (dual-core, 240 MHz), 512 KB internal SRAM, 8 MB PSRAM
- **Panel:** ST7789, 170×320, driven over an **8-bit parallel (i80) bus** — not SPI
- **Library:** TFT_eSPI 2.5.x (`TFT_PARALLEL_8_BIT`)

## Rendering architecture

Every dashboard widget draws into a single off-screen frame buffer
(`TFT_eSprite canvas`, full 170×320, 16-bit). The finished frame is blitted to
the panel in one `pushSprite()`. The global `GFX` pointer is the active draw
target: the canvas on hardware, or `&tft` directly if the buffer can't be
allocated (e.g. the Wokwi simulator, which has no PSRAM).

Why: the original code drew each widget straight to the panel as
`fillRect(background)` then `drawString(value)`. For one frame the cleared
region showed bare background before the text landed — visible flicker on every
number change. Rendering off-screen and pushing the completed frame eliminates
it.

**Invariant:** never call `tft.draw*` / `tft.fill*` in a dashboard, splash, or
bridge path — it writes straight to the panel and the next canvas blit erases
it. Always draw through `GFX->` and set `gCanvasDirty = true`.

Pushes are **change-gated**: the buffer is only blitted when a widget actually
changed (`gCanvasDirty`). When the numbers are static, the bus is idle. The
SYSTEM page is the one exception — it pushes every loop so its live FPS readout
reflects the real achievable rate.

## The DMA question (important, and a corrected claim)

An earlier note here said DMA was "impossible on this hardware." **That was
wrong on the reason.** The accurate picture:

- The ESP32-S3's **LCD_CAM peripheral can DMA an 8-bit parallel bus** — the
  silicon supports it.
- **TFT_eSPI does not implement DMA for parallel mode.** Its own source emits
  `#warning >>>>------>> DMA is not supported in parallel mode`, and calling
  `initDMA()` / `dmaWait()` / `pushImageDMA()` fails to link under
  `TFT_PARALLEL_8_BIT`. TFT_eSPI's DMA is SPI-only on the S3
  (see [TFT_eSPI issue #3414](https://github.com/Bodmer/TFT_eSPI/issues/3414)).
- **LovyanGFX does support DMA on the S3 parallel bus** (it uses LCD_CAM).

So DMA *is* achievable on this board — but only by porting the whole rendering
layer off TFT_eSPI onto LovyanGFX. That's a large rewrite for a benefit that
only matters during the blit itself, so it's deferred (see roadmap). The
takeaway: it's "not possible with our current library," not "not possible on
this hardware."

## Optimizations applied

| Change | Effect |
|---|---|
| Off-screen canvas + single blit | Eliminates per-widget flicker |
| Change-gated pushes | Bus idle when nothing changes; no wasted full-frame redraws |
| **Canvas in internal SRAM** (`setAttribute(PSRAM_ENABLE, false)`) | TFT_eSPI defaults large sprites to PSRAM; streaming 108 KB/frame out of PSRAM to the parallel bus per pixel is the main FPS bottleneck. Internal SRAM read is far faster. Falls back to PSRAM if 108 KB won't fit. |
| Loop delay 10 ms → 2 ms | More responsive buttons; lets the SYSTEM page report true FPS |

The canvas costs ~108 KB (170×320×2). In internal SRAM that drops free heap from
~280 KB to ~170 KB — still plenty, including for WiFi during bridge mode. The
SYSTEM page shows where the canvas landed (`canvas: SRAM` / `PSRAM`) and the live
FPS, so you can confirm the win on-device.

## Roadmap (ordered by impact / effort)

1. **Dirty-rectangle blits.** Track the changed vertical band (`yMin..yMax`) per
   frame and push only those rows via `tft.pushImage(0, yMin, W, h, buf + yMin*W)`
   (full-width bands stay contiguous in the buffer). A speed-only change touches
   ~73 of 320 rows → ~4× less data per push, on top of the SRAM win. Main risk:
   a mis-tracked band leaves a widget stale, so it needs on-device verification.
2. **Per-widget sprites for the hot path.** The big speed number is the most
   frequent change; a dedicated small sprite for just that region would make its
   update tiny regardless of the rest of the frame.
3. **LovyanGFX migration (big).** Unlocks true parallel DMA so the CPU is free
   during the blit. Only worth it if 1–2 prove insufficient.
4. **Smarter poll cadence.** Telemetry is polled every 100 ms; decouple the
   render rate from the poll rate so motion (speed) can interpolate smoothly
   between polls without faster UART traffic.

## Intuitiveness roadmap (UX)

- On-screen control legend / first-boot help overlay (buttons aren't obvious).
- Page indicator dots so riders know where they are in the cycle.
- Configurable units/brand/pack without recompiling (settings persisted in NVS).
- Optional large "glance mode" layout (speed + battery only) for at-speed reading.

## How to measure

Open the **SYSTEM** page (cycle Left to page 5). It shows live FPS, free/min
heap, internal temperature, and whether the canvas is in `SRAM` or `PSRAM`.
Compare FPS before/after changes there.
