#pragma once
// ============================================================================
// LovyanGFX display configuration.
//
// Hardware: LilyGo T-Display-S3 (ESP32-S3, ST7789 170x320, 8-bit parallel via
// the LCD_CAM peripheral — LovyanGFX drives this with DMA, which TFT_eSPI
// cannot do in parallel mode).
//
// Wokwi: logic-only sim with an SPI ILI9341 240x320 stand-in (Wokwi has no
// 8-bit parallel support). Gated by WOKWI_SIMULATION.
//
// Pins/offsets match the verified community config for this board
// (LovyanGFX discussion #406): WR8 RD9 DC7 CS6 RST5, D0-7 = 39,40,41,42,45,46,
// 47,48, BL38, panel offset_x=35, invert on.
//
// ---- HARDWARE BRING-UP CHEAT SHEET (if the panel looks wrong, change ONE) ----
//   * Reds and blues swapped ............ flip  _panel cfg  c.rgb_order
//   * Whole screen looks photo-negative . flip  _panel cfg  c.invert
//   * Image shifted sideways / cut off .. adjust c.offset_x (try 0 then 35)
//   * Mirrored or rotated wrong ......... change tft.setRotation(0) in setup()
//                                          (0..3 normal, 4..7 mirrored)
//   * Totally black / nothing ........... check GPIO15 power rail + c.pin_rst,
//                                          and try lowering bus freq_write to 16M
// (Wokwi's ILI9341 stand-in may render mirrored vs hardware — that's sim-only.)
// ============================================================================
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#ifdef WOKWI_SIMULATION

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
public:
    LGFX() {
        {   auto c = _bus.config();
            c.spi_host   = VSPI_HOST;
            c.spi_mode   = 0;
            c.freq_write = 40000000;
            c.freq_read  = 16000000;
            c.pin_sclk   = 18;
            c.pin_mosi   = 23;
            c.pin_miso   = 19;
            c.pin_dc     = 2;
            _bus.config(c);
            _panel.setBus(&_bus);
        }
        {   auto c = _panel.config();
            c.pin_cs      = 15;
            c.pin_rst     = 4;
            c.pin_busy    = -1;
            c.panel_width  = 240;
            c.panel_height = 320;
            c.offset_x = 0;
            c.offset_y = 0;
            _panel.config(c);
        }
        setPanel(&_panel);
    }
};

#else  // ---- LilyGo T-Display-S3 hardware ----

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_Parallel8 _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        {   auto c = _bus.config();
            c.freq_write = 20000000;     // 20 MHz; can try higher once verified
            c.pin_wr = 8;
            c.pin_rd = 9;
            c.pin_rs = 7;                // D/C
            c.pin_d0 = 39; c.pin_d1 = 40; c.pin_d2 = 41; c.pin_d3 = 42;
            c.pin_d4 = 45; c.pin_d5 = 46; c.pin_d6 = 47; c.pin_d7 = 48;
            _bus.config(c);
            _panel.setBus(&_bus);
        }
        {   auto c = _panel.config();
            c.pin_cs   = 6;
            c.pin_rst  = 5;
            c.pin_busy = -1;
            c.panel_width   = 170;
            c.panel_height  = 320;
            c.offset_x      = 35;        // 170-wide panel centered in 240 controller
            c.offset_y      = 0;
            c.offset_rotation = 0;
            c.dummy_read_pixel = 8;
            c.dummy_read_bits  = 1;
            c.readable   = false;
            c.invert     = true;
            c.rgb_order  = false;
            c.dlen_16bit = false;
            c.bus_shared = false;
            _panel.config(c);
        }
        {   auto c = _light.config();
            c.pin_bl      = 38;
            c.invert      = false;
            c.freq        = 44100;
            c.pwm_channel = 7;
            _light.config(c);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

#endif

// Rotation passed to tft.setRotation() in setup(). 0..3 = 0/90/180/270 degrees,
// 4..7 add a horizontal mirror. Keep it to the PORTRAIT values (0/2/4/6) so the
// 170-wide UI band stays centered. The Wokwi ILI9341 stand-in renders mirrored
// vs the real panel, so it uses a mirrored portrait; hardware uses plain.
#ifdef WOKWI_SIMULATION
  #define DISPLAY_ROTATION 4
#else
  #define DISPLAY_ROTATION 0
#endif
