"""
Host-side design preview for the Longboard-Display UI.

Renders the dashboard at the EXACT physical resolution of the Lilygo
T-Display S3 (170x320 portrait) so layout can be iterated without flashing
the device or fighting Wokwi's 240x320 ILI9341 stand-in.

Note: this renders the logical 170x320 UI. Wokwi screenshots may appear mirrored
or rotated relative to this preview because the simulator uses an ILI9341
stand-in with different orientation behavior than the real ST7789 panel.

It mimics just enough of the LovyanGFX drawing API (datum-based drawString,
rects, fast h-lines, GFX fonts) that the layout math here can be ported 1:1
to src/main.cpp. The Bebas GFX font is rasterised from the same
BebasNeue.ttf at the same pixel sizes the GFX headers were generated at,
so the hero number matches the device closely.

Usage:  python preview.py                  -> writes preview.png (scaled x3)
        python preview.py --speed 27 --kmh
        python preview.py --page 4          -> system info page
        python preview.py --bridge          -> VESC Tool bridge screen
        python preview.py --splash
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

# --- physical panel ---------------------------------------------------------
W, H = 170, 320
SCALE = 3  # upscale factor for the saved PNG (nearest-neighbour, crisp pixels)
TTF = "BebasNeue.ttf"

# --- NZXT CAM palette (matches main.cpp) ------------------------------------
BG     = (26, 26, 26)
BORDER = (68, 68, 68)
DIM    = (136, 136, 136)
LABEL  = (170, 170, 170)
WHITE  = (255, 255, 255)
GREEN  = (0, 200, 100)
RED    = (255, 51, 51)
ACCENT = (185, 80, 215)   # #b950d7 brand violet/purple (magenta-shifted so it reads purple, not blue, on the panel)
YELLOW = (255, 205, 0)
ORANGE = (255, 128, 0)

# Battery warning thresholds (percent). Tune to match your VESC cutoff.
BATT_WARN = 50   # below this -> yellow
BATT_LOW  = 30   # below this -> orange
BATT_CRIT = 15   # below this -> red (stop / charge now)


def batt_color(pct):
    if pct >= BATT_WARN:
        return GREEN
    if pct >= BATT_LOW:
        return YELLOW
    if pct >= BATT_CRIT:
        return ORANGE
    return RED


def watt_color(w):
    if w >= 3000:
        return RED
    if w >= 2000:
        return ORANGE
    if w >= 1000:
        return YELLOW
    return WHITE


def duty_color(d):
    if d >= 95:
        return RED
    if d >= 85:
        return ORANGE
    if d >= 70:
        return YELLOW
    return WHITE

# datum constants (subset of TFT_eSPI)
TL, TC, TR, ML, MC, MR = "TL", "TC", "TR", "ML", "MC", "MR"


class Panel:
    """Tiny TFT_eSPI-like canvas operating in logical 170x320 pixels."""

    def __init__(self):
        self.img = Image.new("RGB", (W, H), BG)
        self.d = ImageDraw.Draw(self.img)
        self.datum = TL
        self.color = WHITE
        # GLCD "font 1" on the device is a 6x8 monospace bitmap. Pillow's
        # default bitmap font is the closest no-dependency stand-in for the
        # small UI text; the hero number uses the real Bebas TTF.
        self._small = ImageFont.load_default()
        self._bebas = {}
        self._sans = {}
        self._sans_bold = {}

    def bebas(self, px):
        if px not in self._bebas:
            self._bebas[px] = ImageFont.truetype(TTF, px)
        return self._bebas[px]

    def sans(self, px, bold=False):
        cache = self._sans_bold if bold else self._sans
        if px not in cache:
            name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
            cache[px] = ImageFont.truetype(name, px)
        return cache[px]

    # -- state setters -------------------------------------------------------
    def set_datum(self, d): self.datum = d
    def set_color(self, c): self.color = c

    # -- primitives ----------------------------------------------------------
    def fill_screen(self, c):
        self.d.rectangle([0, 0, W, H], fill=c)

    def draw_rect(self, x, y, w, h, c):
        self.d.rectangle([x, y, x + w - 1, y + h - 1], outline=c)

    def fill_rect(self, x, y, w, h, c):
        self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def hline(self, x, y, w, c):
        self.d.line([x, y, x + w - 1, y], fill=c)

    # -- text ----------------------------------------------------------------
    def _anchor(self, font):
        # Pillow text anchors: horizontal l/m/r, vertical a(top)/m/s(baseline)
        h = {"L": "l", "C": "m", "R": "r"}[self.datum[1]]
        v = {"T": "a", "M": "m"}[self.datum[0]]
        return h + v

    def draw_string(self, text, x, y, font=None, px=None):
        f = self.bebas(px) if px else (font or self._small)
        self.d.text((x, y), text, font=f, fill=self.color, anchor=self._anchor(f))

    def text_w(self, text, font=None, px=None):
        f = self.bebas(px) if px else (font or self._small)
        return self.d.textlength(text, font=f)

    def save(self, path):
        big = self.img.resize((W * SCALE, H * SCALE), Image.NEAREST)
        big.save(path)


# ---------------------------------------------------------------------------
# Telemetry sample (matches the mock values in image.png)
# ---------------------------------------------------------------------------
class State:
    speed = 42
    use_mph = True
    product = "ESK8OS"
    version = "v3.2"
    rider = "JOE"
    clock = "16:34"
    motor_t, motor_p = 38, 90
    bat_t, bat_p = 32, 85
    esc_t, esc_p = 35, 75
    est_km = 18.5
    rem_km = 21.3
    avg_whkm = 18.2
    batt_pct = 78
    voltage = 39.2
    odo_km = 615
    trip_km = 12.8
    watts = 540
    # page system: 0 dash, 1 power, 2 trip, 3 settings, 4 system
    page = 0
    fault = ""        # "" = none, else fault name -> banner
    demo = True       # ships in demo mode (Settings DEMO row + trip hint)
    # page-1 / page-2 stats
    motor_amps = 28.4
    batt_amps = 14.2
    duty = 72
    wh_used = 210
    wh_regen = 18
    max_kmh = 50.0
    avg_kmh = 29.0
    ride_time = "16:34"
    # page-1 SESSION card
    max_watts = 1320
    min_voltage = 36.4
    # page-3 wheel profile
    wheel_name = "8IN PNEU"
    wheel_diam_mm = 203
    motor_pulley = 16
    wheel_pulley = 72
    poles = 7
    # page-4 system (host-side mocks of the ESP32 stats)
    chip = "ESP32-S3"
    cores = 2
    cpu_mhz = 240
    fw_used = 0.8
    fw_tot = 6.3
    heap = 168     # ~170 kB free after the 108 kB SRAM canvas
    heap_min = 150
    psram = 8100
    mcu_temp = 44.5
    uptime = "00:12:34"
    fps = 26
    blit_ms = 38
    reset = "POWER-ON"
    canvas_psram = False
    # bridge screen
    bridge_status = "CONNECTED"
    bridge_ip = "192.168.4.1"
    bridge_rx_k = 12
    bridge_tx_k = 8
    bridge_sta = 1


# ---------------------------------------------------------------------------
# THE LAYOUT  (logical 170x320 -- port these numbers straight into main.cpp)
# ---------------------------------------------------------------------------
def speed_unit(s): return "MPH" if s.use_mph else "KM/H"
def dist_unit(s):  return "mi" if s.use_mph else "km"
def dist_cv(s):    return 0.621371 if s.use_mph else 1.0


def list_rows(p, rows, y0, step=16, lx=12, vx=158, vcol=WHITE):
    y = y0
    for label, val in rows:
        p.set_datum(TL); p.set_color(DIM); p.draw_string(label, lx, y)
        p.set_datum(TR); p.set_color(vcol); p.draw_string(val, vx, y)
        y += step


def row(p, label, val, y, vcol=WHITE):
    """A single labeled value row with its own value color."""
    p.set_datum(TL); p.set_color(DIM); p.draw_string(label, 12, y)
    p.set_datum(TR); p.set_color(vcol); p.draw_string(val, 158, y)


def draw(p: Panel, s: State):
    p.fill_screen(BG)
    draw_topbar(p, s)
    if s.page == 1:
        draw_page_power(p, s)
    elif s.page == 2:
        draw_page_trip(p, s)
    elif s.page == 3:
        draw_page_settings(p, s)
    elif s.page == 4:
        draw_page_system(p, s)
    else:
        draw_page_dash(p, s)
    draw_cells(p, s)
    draw_dots(p, s)
    draw_bottom(p, s)
    # safety overlays (drawn on top of everything)
    if s.fault:
        draw_fault_banner(p, s)
    elif s.batt_pct < BATT_CRIT:
        draw_crit_overlay(p, s)


def draw_topbar(p, s):
    p.set_datum(TL); p.set_color(DIM)
    p.draw_string(s.product, 4, 4)
    p.set_datum(TC); p.set_color(DIM)
    p.draw_string("RIDER: " + s.rider, 85, 4)
    p.set_datum(TR); p.set_color(WHITE)
    p.draw_string(s.clock, 166, 4)
    p.hline(0, 16, W, BORDER)


def draw_cells(p, s):
    cells, cw, ch, gap = 10, 13, 12, 2
    total = cells * cw + (cells - 1) * gap
    sx = (W - total) // 2
    filled = round(s.batt_pct * cells / 100)
    cc = batt_color(s.batt_pct)
    for i in range(cells):
        cx = sx + i * (cw + gap)
        p.draw_rect(cx, 276, cw, ch, BORDER)
        if i < filled:
            p.fill_rect(cx + 1, 277, cw - 2, ch - 2, cc)
        else:
            p.hline(cx + 2, 286, cw - 4, BORDER)


def draw_dots(p, s):
    # Page-indicator dots in the gap between cells and the bottom bar.
    n = 5
    gap = 8
    x0 = W // 2 - (n - 1) * gap // 2
    for i in range(n):
        dx = x0 + i * gap
        box = [dx - 2, 291, dx + 2, 295]
        if i == s.page:
            p.d.ellipse(box, fill=ACCENT)
        else:
            p.d.ellipse(box, outline=BORDER)


def draw_bottom(p, s):
    p.hline(0, 298, W, BORDER)
    cv, du = dist_cv(s), dist_unit(s)
    pct_str = "%d%%" % s.batt_pct
    rest = "  T:%.1f%s  O:%d%s" % (s.trip_km * cv, du, s.odo_km * cv, du)
    wp, wr = p.text_w(pct_str), p.text_w(rest)
    bsx = (W - (wp + wr)) // 2
    p.set_datum(ML); p.set_color(batt_color(s.batt_pct))
    p.draw_string(pct_str, int(bsx), 307)
    p.set_datum(ML); p.set_color(DIM)
    p.draw_string(rest, int(bsx + wp), 307)


def draw_page_dash(p, s):
    # -- SPEED (y 16..78): number top-aligned, unit to its upper-left -------
    p.set_datum(TC); p.set_color(WHITE)
    p.draw_string(str(s.speed), 96, 17, px=80)
    p.set_datum(TL); p.set_color(LABEL)
    p.draw_string(speed_unit(s), 10, 27, font=p.sans(17, bold=True))

    # -- VOLTS | WATTS panel (y 86..118) ------------------------------------
    bw, bh, by = 162, 32, 86
    bx = (W - bw) // 2
    midx = W // 2
    p.draw_rect(bx, by, bw, bh, BORDER)
    p.d.line([midx, by + 5, midx, by + bh - 5], fill=BORDER)
    cy = by + bh // 2 - 1

    def stat(cx, value, unit, vcol, npx=18, upx=17, g=3):
        value_font = p.sans(npx, bold=True)
        unit_font = p.sans(upx)
        wn = p.text_w(value, font=value_font)
        wu = p.text_w(unit, font=unit_font)
        gx = cx - (wn + g + wu) / 2
        p.set_datum(ML); p.set_color(vcol)
        p.draw_string(value, int(gx), cy, font=value_font)
        p.set_datum(ML); p.set_color(DIM)
        p.draw_string(unit, int(gx + wn + g), cy, font=unit_font)

    stat((bx + midx) // 2, "%.1f" % s.voltage, "V", batt_color(s.batt_pct))
    stat((midx + bx + bw) // 2, str(s.watts), "W", watt_color(s.watts))

    # -- TEMPS card (y 122..192) --------------------------------------------
    card(p, "TEMPS", 4, 122, 162, 70)
    rows = [("MOTOR", s.motor_t, s.motor_p),
            ("BATTERY", s.bat_t, s.bat_p),
            ("ESC", s.esc_t, s.esc_p)]
    ry = 140
    for label, t, pct in rows:
        p.set_datum(TL); p.set_color(DIM)
        p.draw_string(label, 12, ry)
        pstr = "(%d%%)" % pct
        p.set_datum(TR); p.set_color(GREEN)
        p.draw_string(pstr, 158, ry)
        pw = p.text_w(pstr)
        p.set_datum(TR); p.set_color(WHITE)
        p.draw_string("%d°C" % t, int(158 - pw - 4), ry)
        bar_x, bar_y, bar_w = 8, ry + 11, 154
        fill_w = max(0, min(bar_w, bar_w * pct // 100))
        bar_c = YELLOW if pct < 70 else GREEN
        p.hline(bar_x, bar_y, bar_w, BORDER)
        if fill_w:
            p.hline(bar_x, bar_y, fill_w, bar_c)
        ry += 16

    # -- RANGE card (y 198..268) --------------------------------------------
    card(p, "RANGE", 4, 198, 162, 70)
    cv, du = dist_cv(s), dist_unit(s)
    avg_wh = s.avg_whkm / 0.621371 if s.use_mph else s.avg_whkm
    rem_disp = "%.1f %s" % (s.rem_km * cv, du)
    if s.avg_kmh > 1.0:                          # add estimated time-left
        mins = int(s.rem_km / s.avg_kmh * 60)
        if 0 < mins < 1000:
            rem_disp += " %dm" % mins
    rrows = [("ESTIMATED", "%.1f %s" % (s.est_km * cv, du)),
             ("REMAINING", rem_disp),
             ("AVG. WH/%s" % du.upper(), "%.1f wh/%s" % (avg_wh, du))]
    list_rows(p, rrows, 216)


def draw_page_power(p, s):
    cv = dist_cv(s)
    su = "mph" if s.use_mph else "kmh"
    card(p, "POWER", 4, 22, 162, 82)
    row(p, "MOTOR",   "%.1f A" % s.motor_amps, 40)
    row(p, "BATTERY", "%.1f A" % s.batt_amps, 56)
    row(p, "DUTY",    "%d %%" % s.duty, 72, duty_color(s.duty))
    row(p, "PEAK",    "%d W" % s.watts, 88, watt_color(s.watts))

    card(p, "ENERGY", 4, 108, 162, 48)
    row(p, "USED",  "%d Wh" % s.wh_used, 126)
    row(p, "REGEN", "+%d Wh" % s.wh_regen, 142, GREEN)

    card(p, "SPEED", 4, 160, 162, 48)
    row(p, "MAX", "%.0f %s" % (s.max_kmh * cv, su), 178)
    row(p, "AVG", "%.0f %s" % (s.avg_kmh * cv, su), 194)

    card(p, "SESSION", 4, 212, 162, 48)
    row(p, "MAX PWR",  "%d W" % s.max_watts, 230, watt_color(s.max_watts))
    row(p, "MIN VOLT", "%.1f V" % s.min_voltage, 246)


def draw_page_trip(p, s):
    cv, du = dist_cv(s), dist_unit(s)
    su = "mph" if s.use_mph else "kmh"
    avg_wh = s.avg_whkm / 0.621371 if s.use_mph else s.avg_whkm
    card(p, "THIS TRIP", 4, 22, 162, 102)
    list_rows(p, [("TIME", s.ride_time),
                  ("DISTANCE", "%.1f %s" % (s.trip_km * cv, du)),
                  ("AVG SPEED", "%.0f %s" % (s.avg_kmh * cv, su)),
                  ("MAX SPEED", "%.0f %s" % (s.max_kmh * cv, su)),
                  ("EFFICIENCY", "%.0f wh/%s" % (avg_wh, du))], 40)
    card(p, "ODOMETER", 4, 132, 162, 40)
    list_rows(p, [("TOTAL", "%d %s" % (s.odo_km * cv, du))], 150)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("hold L: reset + recharge" if s.demo else "hold L to reset trip",
                  W // 2, 190)


def draw_page_settings(p, s):
    card(p, "WHEEL PROFILE", 4, 22, 162, 82)
    list_rows(p, [("PROFILE", s.wheel_name),
                  ("DIAMETER", "%dmm" % s.wheel_diam_mm),
                  ("GEARING", "%d:%d" % (s.motor_pulley, s.wheel_pulley)),
                  ("POLES", "%d" % s.poles)], 40)

    card(p, "DISPLAY", 4, 110, 162, 54)
    row(p, "UNITS", "MPH" if s.use_mph else "KM/H", 128)
    row(p, "DEMO", "ON" if s.demo else "OFF", 144, YELLOW if s.demo else WHITE)

    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("R: change wheel", W // 2, 176)
    p.draw_string("hold L+R: bridge", W // 2, 190)


def draw_page_system(p, s):
    card(p, "DEVICE", 4, 22, 162, 70)
    list_rows(p, [("CHIP", s.chip),
                  ("CORES", "%d @ %dM" % (s.cores, s.cpu_mhz)),
                  ("FIRMWARE", "%.1f/%.1fM" % (s.fw_used, s.fw_tot))], 40)

    card(p, "MEMORY", 4, 98, 162, 70)
    list_rows(p, [("HEAP", "%d kB" % s.heap),
                  ("MIN", "%d kB" % s.heap_min),
                  ("PSRAM", "%d kB" % s.psram)], 116)

    card(p, "RUNTIME", 4, 174, 162, 70)
    list_rows(p, [("TEMP", "%.1f°C" % s.mcu_temp),
                  ("UPTIME", s.uptime)], 192)
    row(p, "REFRESH", "%df %dms" % (s.fps, s.blit_ms), 224, GREEN if s.fps >= 30 else WHITE)

    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("reset: " + s.reset, W // 2, 256)
    p.draw_string("canvas: " + ("PSRAM" if s.canvas_psram else "SRAM"), W // 2, 268)


def draw_fault_banner(p, s):
    p.fill_rect(8, 118, W - 16, 64, RED)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string("! FAULT", W // 2, 134, px=24)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string(s.fault, W // 2, 166)


def draw_crit_overlay(p, s):
    p.fill_rect(8, 108, W - 16, 92, RED)
    p.set_datum(MC); p.set_color(WHITE)
    alert_font = p.sans(15, bold=True)
    p.draw_string("LOW BATTERY", W // 2, 125, font=alert_font)
    p.draw_string("STOP & CHARGE", W // 2, 157, font=alert_font)
    p.draw_string("%d%%   %.1f V" % (s.batt_pct, s.voltage), W // 2, 181, font=alert_font)


def draw_toast(p, msg):
    """Transient confirmation banner over the dashboard (matches showToast)."""
    p.set_datum(MC)
    tw = p.text_w(msg, px=18) + 28
    tx = (W - tw) // 2
    p.fill_rect(int(tx), 150, int(tw), 30, GREEN)
    p.set_color(BG)
    p.draw_string(msg, W // 2, 165, px=18)


def draw_bridge(p: Panel, s: State):
    """VESC Tool WiFi-TCP bridge screen."""
    p.fill_screen(BG)
    p.set_datum(TC); p.set_color(ACCENT)
    p.draw_string("BRIDGE MODE", W // 2, 18, px=24)
    p.set_datum(TC); p.set_color(LABEL)
    p.draw_string("VESC TOOL CONFIG", W // 2, 56)

    p.set_datum(TL)
    p.set_color(DIM);  p.draw_string("WiFi:", 12, 84)
    p.set_color(WHITE); p.draw_string("ESK8-BRIDGE", 46, 84)
    p.set_color(DIM);  p.draw_string("pass:", 12, 100)
    p.set_color(WHITE); p.draw_string("esk8bridge", 46, 100)
    p.set_color(DIM);  p.draw_string("TCP:", 12, 124)
    p.set_color(WHITE); p.draw_string("%s:65102" % s.bridge_ip, 40, 124)
    p.set_color(DIM)
    p.draw_string("VESC Tool > Connection", 12, 150)
    p.draw_string("> TCP > connect", 12, 162)

    # status box
    p.fill_rect(8, 212, W - 16, 38, BG)
    p.draw_rect(8, 212, W - 16, 38, BORDER)
    if s.bridge_status in ("CONNECTED", "TRAFFIC"):
        sc = GREEN
    elif s.bridge_status == "ERROR":
        sc = RED
    else:
        sc = DIM
    p.set_datum(MC); p.set_color(sc)
    p.draw_string(s.bridge_status, W // 2, 232, px=24)

    # live throughput / station line
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("RX %dK  TX %dK  STA %d" % (s.bridge_rx_k, s.bridge_tx_k, s.bridge_sta),
                  W // 2, 260)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("hold L+R to exit", W // 2, 300)


def draw_splash(p: Panel, s: State, progress=0.7):
    """Boot splash. `progress` 0..1 drives the loading bar."""
    p.fill_screen(BG)

    # wordmark: big "ESK8" with a small superscript "OS" -> "ESK8 OS"
    main_px, os_px, gap = 80, 34, 3
    top = 86
    wmain = p.text_w("ESK8", px=main_px)
    wos = p.text_w("OS", px=os_px)
    x = (W - (wmain + gap + wos)) / 2
    p.set_datum(TL); p.set_color(WHITE)
    p.draw_string("ESK8", int(x), top, px=main_px)
    p.set_datum(TL); p.set_color(ACCENT)          # superscript, top-aligned
    p.draw_string("OS", int(x + wmain + gap), top, px=os_px)

    # tagline
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("RIDE DASHBOARD", W // 2, 168)

    # controls legend
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("L: page     R: units", W // 2, 196)
    p.draw_string("hold L: reset trip", W // 2, 210)
    p.draw_string("hold L+R: bridge mode", W // 2, 224)

    # version (top-right)
    p.set_datum(TR); p.set_color(BORDER)
    p.draw_string(s.version, W - 6, 6)

    # progress bar
    bw, bh, by = 120, 8, 250
    bx = (W - bw) // 2
    p.draw_rect(bx, by, bw, bh, BORDER)
    fillw = int((bw - 2) * max(0.0, min(1.0, progress)))
    if fillw > 0:
        p.fill_rect(bx + 1, by + 1, fillw, bh - 2, GREEN)

    # status line
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("CONNECTING TO VESC...", W // 2, 272)

    # rider
    p.set_datum(MC); p.set_color(LABEL)
    p.draw_string("RIDER: " + s.rider, W // 2, 300)


def card(p: Panel, title, x, y, w, h):
    """Bordered card with a centered header and an underline."""
    p.draw_rect(x, y, w, h, BORDER)
    p.set_datum(TC); p.set_color(LABEL)
    p.draw_string(title, x + w // 2, y + 3)
    p.hline(x + 4, y + 14, w - 8, BORDER)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", type=int, default=State.speed)
    ap.add_argument("--kmh", action="store_true", help="metric (default is MPH)")
    ap.add_argument("--splash", action="store_true", help="render the boot splash")
    ap.add_argument("--bridge", action="store_true", help="render the VESC bridge screen")
    ap.add_argument("--toast", default="", help="overlay a confirmation banner, e.g. RECHARGED")
    ap.add_argument("--progress", type=float, default=0.7)
    ap.add_argument("--batt", type=int, default=State.batt_pct, help="battery %% for color test")
    ap.add_argument("--watts", type=int, default=State.watts)
    ap.add_argument("--page", type=int, default=0,
                    help="0 dash, 1 power, 2 trip, 3 settings, 4 system")
    ap.add_argument("--fault", default="", help="fault name -> show banner")
    ap.add_argument("-o", "--out", default="preview.png")
    a = ap.parse_args()

    s = State()
    s.speed = a.speed
    s.use_mph = not a.kmh
    s.batt_pct = a.batt
    s.watts = a.watts
    s.page = a.page
    s.fault = a.fault
    # keep test voltage consistent with the battery level (3.2-4.2 V/cell)
    s.voltage = round(32.0 + (42.0 - 32.0) * a.batt / 100.0, 1)

    p = Panel()
    if a.splash:
        draw_splash(p, s, a.progress)
    elif a.bridge:
        draw_bridge(p, s)
    else:
        draw(p, s)
        if a.toast:
            draw_toast(p, a.toast)
    p.save(a.out)
    print("wrote", a.out, "(%dx%d logical, x%d)" % (W, H, SCALE))
    print("note: Wokwi screenshots may be mirrored/rotated relative to this logical preview")


if __name__ == "__main__":
    main()
