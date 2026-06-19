# Performance & Rendering Notes

This document records how the ESK8 OS display renders, the optimizations applied
so far, the hardware realities we hit, and the roadmap for squeezing more out of
the LilyGo T-Display-S3. It's here so other DIYers can understand the tradeoffs
and build on them.

## Hardware

- **MCU:** ESP32-S3 (dual-core, 240 MHz), 512 KB internal SRAM, 8 MB PSRAM
- **Panel:** ST7789, 170×320, driven over an **8-bit parallel (i80) bus** — not SPI
- **Library (this branch):** **LovyanGFX** — drives the ESP32-S3 LCD_CAM
  peripheral with **DMA**, so a full-frame blit is ~5 ms instead of TFT_eSPI's
  ~38 ms. (`main` is still on TFT_eSPI; see the "LovyanGFX migration" section.)

> **Branch status:** the full dashboard is ported to LovyanGFX and builds for
> both envs, but is **pending hardware confirmation** (panel colors/orientation
> and the measured blit time on the SYSTEM page). Display config + a bring-up
> cheat sheet live in `src/LGFX_Config.h`.

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
| **Canvas in internal SRAM** (`setAttribute(PSRAM_ENABLE, false)`) | TFT_eSPI defaults large sprites to PSRAM. Internal SRAM read is faster. Falls back to PSRAM if 108 KB won't fit. |
| **Dirty-rectangle blits** | Push only the changed vertical bands via `pushSprite(0,y,0,y,W,h)` (full-width = fast contiguous block copy), not all 320 rows. |
| Loop delay 10 ms → 2 ms | More responsive buttons |

The canvas costs ~108 KB (170×320×2). In internal SRAM that drops free heap from
~280 KB to ~170 KB — still plenty, including for WiFi during bridge mode.

### Measured on hardware (LilyGo T-Display-S3)

- A **full-frame blit is ~38 ms (~25 fps)** — this is the TFT_eSPI 8-bit-parallel
  ceiling. The library bit-bangs the bus pixel-by-pixel with no DMA, so this is
  CPU-bound on GPIO writes, not bus-bound.
- Moving the canvas PSRAM → internal SRAM only gained ~5 fps (20 → 25). So PSRAM
  latency was a minor factor; **the GPIO write loop is the real ceiling.**
- **Dirty-rect is therefore the high-value win:** a speed-only change repaints
  ~73 of 320 rows (~9 ms) instead of the whole frame, so the dashboard's hot path
  is ~4× faster even though the full-frame worst case is unchanged.

The SYSTEM page is a deliberate worst-case benchmark — it forces a full-frame
blit every loop and shows `canvas: SRAM/PSRAM` plus a live `<fps>f <blit>ms`
readout. So it will read ~25 fps / ~38 ms by design; the real dashboard pages are
much faster because they only push changed bands. The blit-ms number is the one
to watch when evaluating future render changes (e.g. a LovyanGFX/DMA port should
drop it to ~5 ms).

## Roadmap (ordered by impact / effort)

1. **LovyanGFX migration (DONE on the `lovyangfx` branch, pending hardware
   confirmation).** TFT_eSPI can't DMA the parallel bus, so full-frame stays
   ~38 ms. LovyanGFX drives the S3 LCD_CAM peripheral with DMA — a full frame
   becomes ~5 ms, enabling smooth high-refresh even for full-screen pushes. The
   port keeps all dashboard logic; only the render layer and fonts changed
   (built-in LovyanGFX fonts for now; custom BebasNeue port deferred).
2. **Per-widget sprite for the hot path.** Give the big speed number its own
   small sprite so its update is tiny regardless of the rest of the frame
   (complements dirty-rect; mostly matters if we stay on TFT_eSPI).
3. **Decouple render rate from poll rate.** Telemetry polls every 100 ms;
   interpolate speed between polls so motion looks smooth without faster UART.
4. **Adaptive dirty granularity.** Current bands are per-update; per-value bands
   (e.g. only the changed temp row) would shrink pushes further.

## Intuitiveness roadmap (UX)

- On-screen control legend / first-boot help overlay (buttons aren't obvious).
- Page indicator dots so riders know where they are in the cycle.
- Configurable units/brand/pack without recompiling (settings persisted in NVS).
- Optional large "glance mode" layout (speed + battery only) for at-speed reading.

## How to measure

Open the **SYSTEM** page (cycle Left to page 5). It shows live FPS, free/min
heap, internal temperature, and whether the canvas is in `SRAM` or `PSRAM`.
Compare FPS before/after changes there.
