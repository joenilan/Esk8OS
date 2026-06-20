"""
Host-side design preview for the Longboard-Display (ESK8OS) UI.

Renders the dashboard at the EXACT physical resolution of the Lilygo
T-Display S3 (170x320 portrait) so layout can be iterated without flashing
the device or fighting Wokwi's 240x320 ILI9341 stand-in.

Note: this renders the logical 170x320 UI. Wokwi screenshots may appear mirrored
or rotated relative to this preview because the simulator uses an ILI9341
stand-in with different orientation behavior than the real ST7789 panel.

It mimics just enough of the LovyanGFX drawing API (datum-based drawString,
rects, fast h-lines, GFX fonts) that the layout math here can be ported 1:1
to the firmware. The Bebas GFX font is rasterised from the same BebasNeue.ttf
at the same pixel sizes the GFX headers were generated at, so the hero number
matches the device closely.

Pages match the firmware PageId enum:
    0 HUD   1 DASH   2 POWER   3 TRIP   4 SETTINGS   5 SYSTEM   6 GRAPHS   7 LOGS

Usage:  python preview.py                  -> writes preview.png (scaled x3)
        python preview.py --page 6         -> graphs page
        python preview.py --page 7         -> ride logs page
        python preview.py --hud            -> Big HUD (same as --page 0)
        python preview.py --theme ice      -> alternate color theme
        python preview.py --bridge         -> VESC Tool bridge screen
        python preview.py --splash
        python preview.py --all             -> dump every page to preview_<name>.png
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

# --- physical panel ---------------------------------------------------------
W, H = 170, 320
SCALE = 3  # upscale factor for the saved PNG (nearest-neighbour, crisp pixels)
TTF = "BebasNeue.ttf"

# --- pages (mirror the firmware PageId enum) --------------------------------
HUD, DASH, POWER, TRIP, SETTINGS, SYSTEM, GRAPHS, LOGS = range(8)
PAGE_COUNT = 8
PAGE_NAMES = ["hud", "dash", "power", "trip", "settings", "system", "graphs", "logs"]

# ---------------------------------------------------------------------------
# THEMES — a palette table. Adding a theme here is all it takes host-side; the
# firmware mirrors this idea (ten COL_* globals set in one place), so a device
# theme is this same table + a Settings row + NVS persist. NOTE for the port:
# the ST7789 reads blue-leaning purples as blue, so accents must be picked
# on-device (the "cam" accent is magenta-shifted on purpose).
# ---------------------------------------------------------------------------
THEMES = {
    "cam": {  # shipping look — NZXT CAM violet
        "bg": (26, 26, 26), "border": (68, 68, 68), "dim": (136, 136, 136),
        "label": (170, 170, 170), "white": (255, 255, 255), "green": (0, 200, 100),
        "red": (255, 51, 51), "accent": (185, 80, 215), "yellow": (255, 205, 0),
        "orange": (255, 128, 0),
    },
    "ember": {  # warm — amber/orange accent
        "bg": (20, 16, 14), "border": (72, 58, 48), "dim": (150, 122, 100),
        "label": (192, 162, 138), "white": (255, 246, 236), "green": (120, 200, 90),
        "red": (255, 60, 48), "accent": (255, 140, 40), "yellow": (255, 205, 0),
        "orange": (255, 120, 0),
    },
    "ice": {  # cool — true cyan accent (renders fine on the panel)
        "bg": (18, 22, 26), "border": (54, 66, 74), "dim": (120, 140, 150),
        "label": (162, 182, 192), "white": (240, 248, 255), "green": (0, 210, 150),
        "red": (255, 70, 70), "accent": (0, 200, 230), "yellow": (255, 210, 90),
        "orange": (255, 140, 40),
    },
    "light": {  # light mode — "white" is the PRIMARY TEXT color, so it goes dark.
        "bg": (236, 236, 240), "border": (176, 176, 182), "dim": (120, 120, 126),
        "label": (74, 74, 80), "white": (24, 24, 28), "green": (0, 150, 72),
        "red": (210, 32, 32), "accent": (150, 44, 190), "yellow": (190, 140, 0),
        "orange": (214, 96, 0),
    },
    "cyber": {  # cyberpunk — neon magenta/cyan on near-black
        "bg": (10, 8, 18), "border": (62, 30, 84), "dim": (124, 92, 162),
        "label": (186, 142, 224), "white": (228, 230, 255), "green": (0, 255, 180),
        "red": (255, 42, 120), "accent": (255, 44, 204), "yellow": (255, 238, 60),
        "orange": (255, 120, 200),
    },
}

# Palette globals — populated by apply_theme() before any drawing happens.
BG = BORDER = DIM = LABEL = WHITE = GREEN = RED = ACCENT = YELLOW = ORANGE = (0, 0, 0)


def apply_theme(name):
    global BG, BORDER, DIM, LABEL, WHITE, GREEN, RED, ACCENT, YELLOW, ORANGE
    t = THEMES.get(name, THEMES["cam"])
    BG, BORDER, DIM, LABEL = t["bg"], t["border"], t["dim"], t["label"]
    WHITE, GREEN, RED = t["white"], t["green"], t["red"]
    ACCENT, YELLOW, ORANGE = t["accent"], t["yellow"], t["orange"]


apply_theme("cam")  # default; main() may override from --theme

# Battery warning thresholds (percent). Tune to match your VESC cutoff.
BATT_WARN = 50   # below this -> yellow
BATT_LOW = 30    # below this -> orange
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

    def vline(self, x, y, h, c):
        self.d.line([x, y, x, y + h - 1], fill=c)

    def line(self, x0, y0, x1, y1, c):
        self.d.line([x0, y0, x1, y1], fill=c)

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
# Telemetry sample (mock values for the preview)
# ---------------------------------------------------------------------------
class State:
    speed = 27
    use_mph = True
    product = "ESK8OS"
    version = "v0.5.1"
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
    page = HUD
    fault = ""        # "" = none, else fault name -> banner
    demo = True       # ships in demo mode (Settings DEMO row + trip hint)
    # power / trip stats
    motor_amps = 28.4
    batt_amps = 14.2
    duty = 72
    wh_used = 210
    wh_regen = 18
    max_kmh = 50.0
    avg_kmh = 29.0
    ride_time = "16:34"
    # SESSION card
    max_watts = 1320
    min_voltage = 36.4
    # settings — wheel profile + display + battery (cursor 0..7 -> SET_* enum)
    wheel_name = "8IN PNEU"
    wheel_diam_mm = 203
    motor_pulley = 16
    wheel_pulley = 72
    poles = 7
    brightness = 100
    settings_cursor = 4   # 0 prof,1 units,2 demo,3 bright,4 cells,5 packAh,6 stopCell,7 whmi
    cells = 10
    pack_ah = 16.5
    stop_cell = 3.30
    wh_mi = 22
    batt_min_v = 33.0
    batt_max_v = 42.0
    # system (host-side mocks of the ESP32 stats)
    chip = "ESP32-S3"
    cores = 2
    cpu_mhz = 240
    fw_used = 1.0
    fw_tot = 6.3
    heap = 168
    heap_min = 150
    psram = 8100
    mcu_temp = 44.5
    uptime = "00:12:34"
    fps = 31
    blit_ms = 5
    reset = "POWER-ON"
    canvas_psram = False
    # detail-log (LittleFS) status line on the LOGS page
    log_state = "on"      # "on" | "off" | "full"
    log_free_kb = 3402
    # saved ride summaries (newest first): (dist_km, max_kmh, wh_used, max_watts)
    rides = [(12.8, 50.0, 232, 1320), (8.4, 47.0, 150, 1180),
             (15.2, 52.0, 318, 1402), (3.1, 41.0, 64, 980)]
    # bridge screen
    bridge_status = "CONNECTED"
    bridge_ip = "192.168.4.1"
    bridge_rx_k = 12
    bridge_tx_k = 8
    bridge_sta = 1


# ---------------------------------------------------------------------------
# THE LAYOUT  (logical 170x320 -- these numbers match the firmware)
# ---------------------------------------------------------------------------
def speed_unit(s): return "MPH" if s.use_mph else "KM/H"
def dist_unit(s):  return "mi" if s.use_mph else "km"
def dist_cv(s):    return 0.621371 if s.use_mph else 1.0


def list_rows(p, rows, y0, step=16, lx=12, vx=158, vcol=None):
    y = y0
    for label, val in rows:
        p.set_datum(TL); p.set_color(DIM); p.draw_string(label, lx, y)
        p.set_datum(TR); p.set_color(vcol or WHITE); p.draw_string(val, vx, y)
        y += step


def row(p, label, val, y, vcol=None):
    """A single labeled value row with its own value color."""
    p.set_datum(TL); p.set_color(DIM); p.draw_string(label, 12, y)
    p.set_datum(TR); p.set_color(vcol or WHITE); p.draw_string(val, 158, y)


def card(p, title, x, y, w, h):
    """Bordered card with a centered header and an underline."""
    p.draw_rect(x, y, w, h, BORDER)
    p.set_datum(TC); p.set_color(LABEL)
    p.draw_string(title, x + w // 2, y + 3)
    p.hline(x + 4, y + 14, w - 8, BORDER)


def draw(p, s):
    # HUD is self-contained (its own top bar + bottom bar, no cells/dots).
    if s.page == HUD:
        draw_hud(p, s)
        return

    p.fill_screen(BG)
    draw_topbar(p, s)
    {DASH: draw_page_dash, POWER: draw_page_power, TRIP: draw_page_trip,
     SETTINGS: draw_page_settings, SYSTEM: draw_page_system,
     GRAPHS: draw_page_graphs, LOGS: draw_page_logs}.get(s.page, draw_page_dash)(p, s)
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
    n = PAGE_COUNT
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
    # editable rows highlighted in the accent with a ">" cursor when selected
    def slabel(text, y, idx):
        sel = (s.settings_cursor == idx)
        if sel:
            p.set_datum(TL); p.set_color(ACCENT); p.draw_string(">", 4, y)
        p.set_datum(TL); p.set_color(ACCENT if sel else DIM); p.draw_string(text, 12, y)

    def value(val, y, vcol=None):
        p.set_datum(TR); p.set_color(vcol or WHITE); p.draw_string(val, 158, y)

    card(p, "WHEEL PROFILE", 4, 22, 162, 82)
    slabel("PROFILE", 40, 0); value(s.wheel_name, 40)
    list_rows(p, [("DIAMETER", "%dmm" % s.wheel_diam_mm),
                  ("GEARING", "%d:%d" % (s.motor_pulley, s.wheel_pulley)),
                  ("POLES", "%d" % s.poles)], 56)

    card(p, "DISPLAY", 4, 110, 162, 58)
    slabel("UNITS", 128, 1);      value("MPH" if s.use_mph else "KM/H", 128)
    slabel("DEMO", 144, 2);       value("ON" if s.demo else "OFF", 144, YELLOW if s.demo else WHITE)
    slabel("BRIGHTNESS", 160, 3); value("%d%%" % s.brightness, 160)

    card(p, "BATTERY", 4, 174, 162, 88)
    slabel("CELLS", 192, 4);   value("%dS" % s.cells, 192)
    slabel("PACK AH", 208, 5); value("%.1fAh" % s.pack_ah, 208)
    slabel("STOP/C", 224, 6);  value("%.2fV" % s.stop_cell, 224)
    slabel("WH/MI", 240, 7);   value("%d" % s.wh_mi, 240)
    row(p, "WINDOW", "%.1f-%.1f" % (s.batt_min_v, s.batt_max_v), 256, DIM)

    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("L: select  R: change", W // 2, 268)


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

    # Footer pulled up so the version line clears the battery cells (y=276).
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("reset: " + s.reset, W // 2, 246)
    p.draw_string("canvas: " + ("PSRAM" if s.canvas_psram else "SRAM"), W // 2, 257)
    p.set_color(LABEL)
    p.draw_string(s.version + " a1b2c3d", W // 2, 268)


def _mini_graph(p, x, y, w, h, label, cur_text, trend, color, values, vmin, vmax):
    """One mini line graph (mirrors firmware drawMiniGraph)."""
    p.draw_rect(x, y, w, h, BORDER)
    p.set_datum(TL); p.set_color(DIM)
    p.draw_string(label, x + 4, y + 3)
    p.set_datum(TR); p.set_color(color)
    p.draw_string("%s %s" % (trend, cur_text), x + w - 4, y + 3)

    gx, gy, gw, gh = x + 4, y + 17, w - 8, h - 21
    p.hline(gx, gy + gh // 2, gw, BORDER)   # midline guide
    if len(values) < 2 or vmax <= vmin:
        return
    last = None
    n = len(values)
    for i, v in enumerate(values):
        v = max(vmin, min(vmax, v))
        px = gx + (i * (gw - 1)) // max(1, n - 1)
        py = gy + gh - 1 - int((v - vmin) * (gh - 1) / (vmax - vmin))
        if last:
            p.line(last[0], last[1], px, py, color)
        last = (px, py)


def draw_page_graphs(p, s):
    import math
    # Synthesize ~60 samples (3-min window) just for the preview shape.
    n = 60
    spd = [max(0, 22 + 14 * math.sin(i / 6.0)) for i in range(n)]
    wts = [max(0, 900 + 800 * math.sin(i / 5.0 + 1)) for i in range(n)]
    vlt = [40.5 - 0.05 * i for i in range(n)]
    mtt = [30 + 18 * (i / n) + 4 * math.sin(i / 4.0) for i in range(n)]
    su = "mph" if s.use_mph else "kmh"
    smax = 40 if s.use_mph else 65
    _mini_graph(p, 4, 22, 162, 56, "SPEED", "%d %s" % (s.speed, su), "^", ACCENT,
                spd, 0, smax)
    _mini_graph(p, 4, 82, 162, 56, "WATTS", "%d W" % s.watts, "v", watt_color(s.watts),
                wts, 0, max(3000, s.max_watts * 1.15))
    _mini_graph(p, 4, 142, 162, 56, "VOLTS", "%.1f V" % s.voltage, "-", batt_color(s.batt_pct),
                vlt, s.batt_min_v, s.batt_max_v)
    _mini_graph(p, 4, 202, 162, 56, "MOTOR TEMP", "%dC" % s.motor_t, "^", GREEN,
                mtt, 20, 100)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("3 MIN HISTORY", W // 2, 262)


def _log_status_line(p, s, y):
    """Detail-log (LittleFS) status line at the bottom of the LOGS list."""
    p.set_datum(MC)
    if s.log_state == "full":
        p.set_color(RED); p.draw_string("! LOG STORAGE FULL", W // 2, y)
    elif s.log_state == "off":
        p.set_color(YELLOW); p.draw_string("DETAIL LOGGING OFF", W // 2, y)
    else:
        p.set_color(DIM); p.draw_string("log: %d KB free" % s.log_free_kb, W // 2, y)


def draw_page_logs(p, s):
    card(p, "RIDE LOGS", 4, 22, 162, 240)
    cv, du = dist_cv(s), dist_unit(s)
    su = "mph" if s.use_mph else "kmh"
    if not s.rides:
        p.set_datum(TL); p.set_color(DIM); p.draw_string("No saved rides yet.", 14, 48)
    else:
        for r, (dist_km, max_kmh, wh_used, max_w) in enumerate(s.rides[:5]):
            dist = dist_km * cv
            wh_per = (wh_used / dist) if dist > 0.01 else 0
            y = 42 + r * 40
            p.set_datum(TL); p.set_color(ACCENT); p.draw_string("#%d" % (r + 1), 12, y)
            p.set_color(WHITE); p.draw_string("%.1f %s" % (dist, du), 34, y)
            p.set_color(DIM)
            p.draw_string("max %d %s" % (round(max_kmh * cv), su), 12, y + 12)
            p.draw_string("%d Wh/%s  %dW" % (round(wh_per), du, round(max_w)), 12, y + 24)
    _log_status_line(p, s, 236)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("saved on trip reset", W // 2, 268)


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


def draw_bridge(p, s):
    """VESC Tool bridge screen (WiFi-TCP + BLE + ride-log web download)."""
    p.fill_screen(BG)
    p.set_datum(TC); p.set_color(ACCENT)
    p.draw_string("BRIDGE MODE", W // 2, 18, px=24)
    p.set_datum(TC); p.set_color(LABEL)
    p.draw_string("VESC TOOL CONFIG", W // 2, 56)

    p.set_datum(TL)
    p.set_color(DIM);  p.draw_string("WiFi:", 12, 80)
    p.set_color(WHITE); p.draw_string("ESK8-BRIDGE", 46, 80)
    p.set_color(DIM);  p.draw_string("pass:", 12, 94)
    p.set_color(WHITE); p.draw_string("esk8bridge", 46, 94)
    p.set_color(DIM);  p.draw_string("TCP:", 12, 108)
    p.set_color(WHITE); p.draw_string("%s:65102" % s.bridge_ip, 40, 108)
    p.set_color(DIM);  p.draw_string("BLE:", 12, 122)
    p.set_color(WHITE); p.draw_string("ESK8-BLE", 40, 122)

    p.set_color(DIM)
    p.draw_string("Desktop: TCP connection", 12, 146)
    p.draw_string("Mobile: scan BLE in app", 12, 158)
    p.set_color(DIM);  p.draw_string("logs:", 12, 174)
    p.set_color(WHITE); p.draw_string("http://192.168.4.1", 44, 174)

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


def draw_splash(p, s, progress=0.7):
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


def draw_hud(p, s):
    """PAGE 0: Big HUD. Glanceable ride screen — huge speed, big battery, four
    key tiles. No page dots / mid-screen cell strip; the bottom status bar stays.
    Speed uses the native BebasNeue110 (~79px digits) drawn at scale 1.0 — crisp,
    no fractional scaling — so positioning is predictable."""
    p.fill_screen(BG)
    draw_topbar(p, s)

    # SPEED — big, top-anchored just below the status bar so it can't bleed up.
    # NOTE: the firmware's hudSpeedY is tuned separately (LovyanGFX places this
    # tall font lower than PIL does), so this Y won't match the device exactly.
    p.set_datum(TC); p.set_color(WHITE)
    p.draw_string(str(s.speed), W // 2, 22, px=110)

    # unit, centered under the number
    p.set_datum(MC); p.set_color(LABEL)
    p.draw_string(speed_unit(s), W // 2, 128, font=p.sans(15, bold=True))

    p.hline(8, 146, W - 16, BORDER)   # separator

    # battery cells — larger than the shared strip, centered; the secondary
    # cluster is pushed toward the bottom to fill the space under the hero speed.
    cells, cw, ch, gap = 10, 13, 18, 2
    total = cells * cw + (cells - 1) * gap
    sx = (W - total) // 2
    filled = round(s.batt_pct * cells / 100)
    cc = batt_color(s.batt_pct)
    cy = 158
    for i in range(cells):
        cx = sx + i * (cw + gap)
        p.draw_rect(cx, cy, cw, ch, BORDER)
        if i < filled:
            p.fill_rect(cx + 1, cy + 1, cw - 2, ch - 2, cc)
        else:
            p.hline(cx + 2, cy + ch - 2, cw - 4, BORDER)

    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string("%d%%" % s.batt_pct, W // 2, 192, font=p.sans(16, bold=True))

    # four key tiles (2x2), sitting just above the bottom status bar
    def tile(x, y, w, label, val, vcol):
        p.draw_rect(x, y, w, 44, BORDER)
        p.set_datum(TC); p.set_color(DIM); p.draw_string(label, x + w // 2, y + 5)
        p.set_datum(MC); p.set_color(vcol)
        p.draw_string(val, x + w // 2, y + 29, font=p.sans(14, bold=True))

    cv, du = dist_cv(s), dist_unit(s)
    tile(4, 202, 78, "WATTS", str(s.watts), watt_color(s.watts))
    tile(88, 202, 78, "VOLTS", "%.1f" % s.voltage, batt_color(s.batt_pct))
    tile(4, 250, 78, "RANGE", "%.1f%s" % (s.rem_km * cv, du), WHITE)
    tile(88, 250, 78, "TEMP", "%dC" % int(round(max(s.motor_t, s.esc_t))), GREEN)

    draw_bottom(p, s)


def _read_version():
    try:
        with open("version.txt") as f:
            return "v" + f.read().strip()
    except OSError:
        return State.version


def render(s, kind="page", progress=0.7, toast=""):
    p = Panel()
    if kind == "splash":
        draw_splash(p, s, progress)
    elif kind == "bridge":
        draw_bridge(p, s)
    else:
        draw(p, s)
        if toast:
            draw_toast(p, toast)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", type=int, default=State.speed)
    ap.add_argument("--kmh", action="store_true", help="metric (default is MPH)")
    ap.add_argument("--splash", action="store_true", help="render the boot splash")
    ap.add_argument("--hud", action="store_true", help="render the Big HUD (page 0)")
    ap.add_argument("--bridge", action="store_true", help="render the VESC bridge screen")
    ap.add_argument("--toast", default="", help="overlay a confirmation banner, e.g. RECHARGED")
    ap.add_argument("--progress", type=float, default=0.7)
    ap.add_argument("--batt", type=int, default=State.batt_pct, help="battery %% for color test")
    ap.add_argument("--watts", type=int, default=State.watts)
    ap.add_argument("--page", type=int, default=0,
                    help="0 hud 1 dash 2 power 3 trip 4 settings 5 system 6 graphs 7 logs")
    ap.add_argument("--theme", default="cam", choices=list(THEMES), help="color theme")
    ap.add_argument("--logstate", default=State.log_state, choices=["on", "off", "full"],
                    help="LOGS-page detail-logging status")
    ap.add_argument("--fault", default="", help="fault name -> show banner")
    ap.add_argument("--all", action="store_true", help="dump every page to preview_<name>.png")
    ap.add_argument("-o", "--out", default="preview.png")
    a = ap.parse_args()

    apply_theme(a.theme)

    s = State()
    s.version = _read_version()
    s.speed = a.speed
    s.use_mph = not a.kmh
    s.batt_pct = a.batt
    s.watts = a.watts
    s.page = 0 if a.hud else a.page
    s.fault = a.fault
    s.log_state = a.logstate
    # keep test voltage consistent with the battery level (3.2-4.2 V/cell)
    s.voltage = round(32.0 + (42.0 - 32.0) * a.batt / 100.0, 1)

    if a.all:
        for i, name in enumerate(PAGE_NAMES):
            s.page = i
            render(s).save("preview_%s.png" % name)
        render(s, "bridge").save("preview_bridge.png")
        render(s, "splash", a.progress).save("preview_splash.png")
        print("wrote preview_<page>.png for all %d pages + bridge + splash" % PAGE_COUNT)
        return

    kind = "splash" if a.splash else ("bridge" if a.bridge else "page")
    render(s, kind, a.progress, a.toast).save(a.out)
    print("wrote", a.out, "(%dx%d logical, x%d, theme=%s)" % (W, H, SCALE, a.theme))
    print("note: Wokwi screenshots may be mirrored/rotated relative to this logical preview")


if __name__ == "__main__":
    main()
