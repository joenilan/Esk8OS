"""
Host-side PIXEL-EXACT preview for the Longboard-Display (ESK8OS) UI.

Renders the dashboard at the exact physical resolution of the Lilygo
T-Display S3 (170x320 portrait) using the SAME glyph bitmaps the firmware
draws with (src/ui/BebasNeue*.h + LovyanGFX's 6x8 GLCD Font0) and a faithful
re-implementation of LovyanGFX's draw_string() placement math (datum handling,
per-glyph xOffset/yOffset, baseline metrics from GFXfont::getDefaultMetric).
Layout code below is a 1:1 port of src/ui/ui.cpp + src/services/bridge.cpp
(v0.9.4) — same coordinates, same fonts, same color rules.

The only things not bit-identical to a device photo: mock telemetry values,
and LCD panel effects (gamma, subpixels). Glyphs land on the same pixels.

Pages match the firmware PageId enum:
    0 HUD   1 DASH   2 POWER   3 TRIP   4 SETTINGS   5 SYSTEM   6 GRAPHS   7 LOGS

Usage:  python preview.py                  -> writes preview.png (scaled x3)
        python preview.py --page 6         -> graphs page
        python preview.py --hud            -> Big HUD (same as --page 0)
        python preview.py --hud-face safety
        python preview.py --theme ice      -> alternate color theme
        python preview.py --bridge / --splash
        python preview.py --no-demo        -> rider name instead of DEMO MODE
        python preview.py --all            -> dump every page to preview_<name>.png
"""
import argparse
import glob as globmod
import os
import re

from PIL import Image, ImageDraw

# --- physical panel ---------------------------------------------------------
W, H = 170, 320
X0 = 0
SCALE = 3  # upscale factor for the saved PNG (nearest-neighbour, crisp pixels)
HERE = os.path.dirname(os.path.abspath(__file__))

# --- pages (mirror the firmware PageId enum) --------------------------------
HUD, DASH, POWER, TRIP, SETTINGS, SYSTEM, GRAPHS, LOGS = range(8)
PAGE_COUNT = 8
PAGE_NAMES = ["hud", "dash", "power", "trip", "settings", "system", "graphs", "logs"]

# ---------------------------------------------------------------------------
# THEMES — mirrors src/ui/Theme.cpp exactly (same order, same RGB values).
# ---------------------------------------------------------------------------
THEMES = {
    "cam": {  # shipping look — NZXT CAM violet
        "bg": (26, 26, 26), "border": (68, 68, 68), "dim": (136, 136, 136),
        "label": (170, 170, 170), "white": (255, 255, 255), "green": (0, 200, 100),
        "red": (255, 51, 51), "accent": (185, 80, 215), "yellow": (255, 205, 0),
        "orange": (255, 128, 0),
    },
    "ember": {
        "bg": (20, 16, 14), "border": (72, 58, 48), "dim": (150, 122, 100),
        "label": (192, 162, 138), "white": (255, 246, 236), "green": (120, 200, 90),
        "red": (255, 60, 48), "accent": (255, 140, 40), "yellow": (255, 205, 0),
        "orange": (255, 120, 0),
    },
    "ice": {
        "bg": (18, 22, 26), "border": (54, 66, 74), "dim": (120, 140, 150),
        "label": (162, 182, 192), "white": (240, 248, 255), "green": (0, 210, 150),
        "red": (255, 70, 70), "accent": (0, 200, 230), "yellow": (255, 210, 90),
        "orange": (255, 140, 40),
    },
    "light": {
        "bg": (236, 236, 240), "border": (176, 176, 182), "dim": (120, 120, 126),
        "label": (74, 74, 80), "white": (24, 24, 28), "green": (0, 150, 72),
        "red": (210, 32, 32), "accent": (150, 44, 190), "yellow": (190, 140, 0),
        "orange": (214, 96, 0),
    },
    "cyber": {
        "bg": (10, 8, 18), "border": (62, 30, 84), "dim": (124, 92, 162),
        "label": (186, 142, 224), "white": (228, 230, 255), "green": (0, 255, 180),
        "red": (255, 42, 120), "accent": (255, 44, 204), "yellow": (255, 238, 60),
        "orange": (255, 120, 200),
    },
    "synthwave": {
        "bg": (22, 12, 34), "border": (70, 40, 90), "dim": (150, 110, 170),
        "label": (210, 150, 200), "white": (250, 240, 255), "green": (60, 240, 200),
        "red": (255, 80, 110), "accent": (255, 90, 170), "yellow": (255, 210, 100),
        "orange": (255, 150, 90),
    },
    "mono": {
        "bg": (16, 16, 16), "border": (64, 64, 64), "dim": (130, 130, 130),
        "label": (180, 180, 180), "white": (245, 245, 245), "green": (200, 200, 200),
        "red": (235, 235, 235), "accent": (255, 255, 255), "yellow": (210, 210, 210),
        "orange": (225, 225, 225),
    },
    "forest": {
        "bg": (14, 22, 16), "border": (48, 70, 52), "dim": (110, 140, 116),
        "label": (160, 190, 164), "white": (236, 246, 238), "green": (90, 220, 120),
        "red": (240, 90, 70), "accent": (120, 210, 110), "yellow": (220, 200, 90),
        "orange": (235, 150, 70),
    },
}

BG = BORDER = DIM = LABEL = WHITE = GREEN = RED = ACCENT = YELLOW = ORANGE = (0, 0, 0)
CURRENT_THEME = "cam"


def apply_theme(name):
    global BG, BORDER, DIM, LABEL, WHITE, GREEN, RED, ACCENT, YELLOW, ORANGE, CURRENT_THEME
    CURRENT_THEME = name if name in THEMES else "cam"
    t = THEMES.get(name, THEMES["cam"])
    BG, BORDER, DIM, LABEL = t["bg"], t["border"], t["dim"], t["label"]
    WHITE, GREEN, RED = t["white"], t["green"], t["red"]
    ACCENT, YELLOW, ORANGE = t["accent"], t["yellow"], t["orange"]


apply_theme("cam")

# ---------------------------------------------------------------------------
# FONT ENGINE — the firmware's actual glyphs.
#
# GfxFont parses the Adafruit-GFX headers the firmware compiles in
# (src/ui/BebasNeueNN.h) and reproduces LovyanGFX's GFXfont metrics:
#   baseline = max(-yOffset) over all glyphs; height = baseline + max(h+yOffset).
# GlcdFont parses LovyanGFX's Fonts/glcdfont.h (Font0): 5 column-bytes per
# char (LSB = top row), 6px advance, 8px line, monospace.
# ---------------------------------------------------------------------------
class GfxFont:
    def __init__(self, path):
        src = open(path, encoding="utf-8", errors="replace").read()
        bm = re.search(r"Bitmaps\[\]\s*(?:PROGMEM)?\s*=\s*\{(.*?)\};", src, re.S)
        self.bitmap = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", bm.group(1)))
        gl = re.search(r"Glyphs\[\]\s*(?:PROGMEM)?\s*=\s*\{(.*?)\};", src, re.S)
        self.glyphs = []  # (bitmapOffset, w, h, xAdvance, xOffset, yOffset)
        for m in re.finditer(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", gl.group(1)):
            self.glyphs.append(tuple(int(v) for v in m.groups()))
        ft = re.search(r"const GFXfont [^=]+=\s*\{.*?(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\};", src, re.S)
        self.first, self.last, self.y_advance = (int(v) for v in ft.groups())
        # LovyanGFX GFXfont::getDefaultMetric — scan every glyph.
        ab = bb = 0
        for (_, h, yo) in ((g[0], g[2], g[5]) for g in self.glyphs):
            pass
        for g in self.glyphs:
            a = -g[5]
            if a > ab: ab = a
            b = g[2] - a
            if b > bb: bb = b
        self.baseline = ab
        self.height = ab + bb

    def glyph(self, ch):
        c = ord(ch)
        if c < self.first or c > self.last:
            c = 0x20
            if c < self.first or c > self.last:
                return None
        return self.glyphs[c - self.first]

    def text_width(self, text):
        # LGFXBase::text_width, size 1.
        left = right = 0
        for ch in text:
            g = self.glyph(ch)
            if g is None:
                continue
            _, w, _, xadv, xoff, _ = g
            if left == 0 and right == 0 and xoff < 0:
                left = right = -xoff
            right = left + max(xadv, w + xoff)
            left += xadv
        return right

    def draw(self, put, text, x, y_baseline, color):
        # Per-glyph blit exactly as GFXfont::drawChar does at size 1.
        sum_x = 0
        first = True
        for ch in text:
            g = self.glyph(ch)
            if g is None:
                continue
            off, w, h, xadv, xoff, yoff = g
            if first and xoff < 0:
                sum_x = -xoff
            first = False
            gx = x + sum_x + xoff
            gy = y_baseline + yoff
            bit = off * 8
            for row in range(h):
                for col in range(w):
                    if self.bitmap[bit >> 3] & (0x80 >> (bit & 7)):
                        put(gx + col, gy + row, color)
                    bit += 1
            sum_x += xadv
        return sum_x


class GlcdFont:
    """LovyanGFX fonts::Font0 — 6x8 monospace GLCD, baseline 7."""
    def __init__(self, path=None):
        self.data = None
        if path and os.path.exists(path):
            src = open(path, encoding="utf-8", errors="replace").read()
            hexs = re.findall(r"0x([0-9A-Fa-f]{2})", src)
            if len(hexs) >= 255 * 5:
                self.data = bytes(int(h, 16) for h in hexs[: 256 * 5])
        self.height = 8
        self.baseline = 7
        self.advance = 6

    def text_width(self, text):
        return self.advance * len(text)

    def draw(self, put, text, x, y_top, color):
        for ch in text:
            c = ord(ch)
            if self.data and 0 <= c <= 255:
                if c >= 176:
                    c += 1  # LGFX 'classic' charset shift (non-cp437)
                base = c * 5
                for i in range(5):
                    line = self.data[base + i] if base + i < len(self.data) else 0
                    for j in range(8):
                        if line & (1 << j):
                            put(x + i, y_top + j, color)
            x += self.advance
        return x


def _find_glcdfont():
    for pat in (
        os.path.join(HERE, ".pio", "libdeps", "*", "LovyanGFX", "src", "lgfx", "Fonts", "glcdfont.h"),
    ):
        hits = globmod.glob(pat)
        if hits:
            return hits[0]
    return None


FONT0 = GlcdFont(_find_glcdfont())
if FONT0.data is None:
    print("WARNING: LovyanGFX glcdfont.h not found (run a pio build once) — small text will be blank")

_GFX_CACHE = {}


def bebas(pt):
    if pt not in _GFX_CACHE:
        _GFX_CACHE[pt] = GfxFont(os.path.join(HERE, "src", "ui", "BebasNeue%d.h" % pt))
    return _GFX_CACHE[pt]


# datum constants (match LovyanGFX textdatum_t semantics we use)
TL, TC, TR, ML, MC, MR, BL, BC, BR = "TL", "TC", "TR", "ML", "MC", "MR", "BL", "BC", "BR"


class Panel:
    """A 170x320 canvas with LovyanGFX-faithful drawString()."""

    def __init__(self):
        self.img = Image.new("RGB", (W, H), BG)
        self.px = self.img.load()
        self.d = ImageDraw.Draw(self.img)
        self.datum = TL
        self.color = WHITE
        self.font = FONT0   # firmware defaults to fonts::Font0

    # -- state ----------------------------------------------------------------
    def set_datum(self, d): self.datum = d
    def set_color(self, c): self.color = c
    def set_font(self, f): self.font = f

    # -- primitives (match GFX->draw*/fill*) -----------------------------------
    def _put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[x, y] = c

    def fill_screen(self, c):
        self.d.rectangle([0, 0, W - 1, H - 1], fill=c)

    def draw_rect(self, x, y, w, h, c):
        if w > 0 and h > 0:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], outline=c)

    def fill_rect(self, x, y, w, h, c):
        if w > 0 and h > 0:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def hline(self, x, y, w, c):
        if w > 0:
            self.d.line([x, y, x + w - 1, y], fill=c)

    def vline(self, x, y, h, c):
        if h > 0:
            self.d.line([x, y, x, y + h - 1], fill=c)

    def line(self, x0, y0, x1, y1, c):
        self.d.line([x0, y0, x1, y1], fill=c)

    def fill_circle(self, x, y, r, c):
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=c)

    def draw_circle(self, x, y, r, c):
        self.d.ellipse([x - r, y - r, x + r, y + r], outline=c)

    # -- text: LGFXBase::draw_string at size 1 ----------------------------------
    def text_w(self, text, font=None):
        return (font or self.font).text_width(text)

    def draw_string(self, text, x, y, font=None):
        f = font or self.font
        cw = f.text_width(text)
        ch = f.height
        v, hz = self.datum[0], self.datum[1]
        if v == "M":
            y -= ch >> 1
        elif v == "B":
            y -= ch
        if hz == "C":
            x -= cw >> 1
        elif hz == "R":
            x -= cw
        if isinstance(f, GfxFont):
            f.draw(self._put, text, x, y + f.baseline, self.color)
        else:
            f.draw(self._put, text, x, y, self.color)
        return cw

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
    version = "v0.9.4"
    version_full = "v0.9.4 dfcfd2d"
    rider = "JOE"
    pair_code = "0AE1"
    clock = "16:34"
    motor_t, motor_p = 38, 90
    bat_t, bat_p = 32, 85
    esc_t, esc_p = 35, 75
    rem_km = 21.3            # remainingRangeKm (HOME)
    limp_km = 4.2            # remainingLimpRangeKm (LIMP)
    avg_whkm = 18.2
    batt_pct = 78
    voltage = 39.8
    odo_km = 615.2
    trip_km = 12.8
    watts = 540              # currentWatts
    peak_watts = 540         # peakWatts (peak-hold "now")
    page = HUD
    demo = True              # firmware default: demo until a VESC is configured
    # power / trip stats
    motor_amps = 28.4
    batt_amps = 14.2
    duty = 72
    wh_used = 210
    wh_regen = 18
    max_kmh = 50.0
    avg_kmh = 29.0
    trip_time = "16:34"      # mm:ss moving time
    max_watts = 1320         # maxWattsSession (MAX RIDE)
    min_voltage = 36.4       # minVoltageSession
    # SAFETY HUD face
    cell_v = 3.86
    range_alert = 0          # 0 ok, 1 turn home, 2 volt sag, 3 limp home
    sag_events = 0
    low_volt_secs = 0
    telemetry_live = True
    # settings
    wheel_name = "8IN PNEU"
    wheel_diam_mm = 203
    motor_pulley = 16
    wheel_pulley = 72
    poles = 7
    brightness = 100
    hud_face = "speed"
    battery_focus = "pct"
    settings_cursor = 5
    settings_editing = False
    cells = 10
    pack_ah = 16.5
    stop_cell = 3.30
    wh_mi = 22.0
    batt_min_v = 33.0
    batt_max_v = 42.0
    # system
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
    # LOGS page
    log_state = "on"      # "on" | "off" | "full"
    log_free_kb = 3402
    rides = [(12.8, 50.0, 232, 1320), (8.4, 47.0, 150, 1180),
             (15.2, 52.0, 318, 1402), (3.1, 41.0, 64, 980)]
    # bridge screen
    bridge_status = "CONNECTED"
    bridge_ssid = "ESK8-BRIDGE"
    bridge_pass = "esk8-0AE1"     # per-device: esk8-<pairCode> (v0.9.4)
    bridge_ipport = "192.168.4.1:65102"
    bridge_wifi_rx_k = 12
    bridge_wifi_tx_k = 8
    bridge_sta = 1
    bridge_ble_on = True
    bridge_ble_rx = 4821
    bridge_ble_tx = 9640


# ---------------------------------------------------------------------------
# COLOR ZONE HELPERS (ui.cpp)
# ---------------------------------------------------------------------------
BATT_WARN_PCT, BATT_LOW_PCT, BATT_CRIT_PCT = 50, 30, 15
MOTOR_TEMP_LIMIT, ESC_TEMP_LIMIT = 80, 80


def batt_color(pct):
    if pct >= BATT_WARN_PCT: return GREEN
    if pct >= BATT_LOW_PCT:  return YELLOW
    if pct >= BATT_CRIT_PCT: return ORANGE
    return RED


def watt_color(w):
    if w >= 3000: return RED
    if w >= 2000: return ORANGE
    if w >= 1000: return YELLOW
    return WHITE


def duty_color(d):
    if d >= 95: return RED
    if d >= 85: return ORANGE
    if d >= 70: return YELLOW
    return WHITE


def speed_unit(s): return "MPH" if s.use_mph else "KM/H"
def dist_unit(s):  return "mi" if s.use_mph else "km"
def dist_cv(s):    return 0.621371 if s.use_mph else 1.0


# ---------------------------------------------------------------------------
# SHARED CHROME — ports of drawStaticFrame / updateClock / updateBottomBar
# ---------------------------------------------------------------------------
def draw_battery_cells_row(p, s, y, cell_h, draw_fill):
    gap = 1 if s.cells > 12 else 2
    cw = max(5, min(13, (W - ((s.cells - 1) * gap)) // s.cells))
    total = s.cells * cw + (s.cells - 1) * gap
    sx = X0 + (W - total) // 2
    level = s.batt_pct * s.cells / 100.0
    full = int(level)
    frac = level - full
    cc = batt_color(s.batt_pct)
    for i in range(s.cells):
        cx = sx + i * (cw + gap)
        p.draw_rect(cx, y, cw, cell_h, BORDER)
        if not draw_fill:
            continue
        p.fill_rect(cx + 1, y + 1, cw - 2, cell_h - 2, BG)
        if i < full:
            p.fill_rect(cx + 1, y + 1, cw - 2, cell_h - 2, cc)
        elif i == full and frac > 0:
            fw = round((cw - 2) * frac)
            if fw > 0:
                p.fill_rect(cx + 1, y + 1, fw, cell_h - 2, cc)
            else:
                p.hline(cx + 2, y + cell_h - 2, max(0, cw - 4), BORDER)
        else:
            p.hline(cx + 2, y + cell_h - 2, max(0, cw - 4), BORDER)


def draw_card(p, x, y, w, h, title):
    p.draw_rect(X0 + x, y, w, h, BORDER)
    p.set_font(FONT0)
    p.set_datum(TC); p.set_color(LABEL)
    p.draw_string(title, X0 + x + w // 2, y + 3)
    p.hline(X0 + x + 4, y + 14, w - 8, BORDER)


def draw_row_label(p, label, y):
    p.set_font(FONT0)
    p.set_datum(TL); p.set_color(DIM)
    p.draw_string(label, X0 + 12, y)


def draw_val(p, y, value, color):
    p.fill_rect(X0 + 78, y - 1, 84, 12, BG)
    p.set_font(FONT0)
    p.set_datum(TR); p.set_color(color)
    p.draw_string(value, X0 + 158, y)


def draw_top_bar(p, s):
    p.set_font(FONT0)
    p.set_datum(TL); p.set_color(DIM)
    p.draw_string(s.product, X0 + 4, 4)
    p.set_datum(TC)
    if s.demo:
        p.set_color(YELLOW)
        p.draw_string("DEMO MODE", X0 + 85, 4)
    elif s.rider:
        p.set_color(DIM)
        p.draw_string("RIDER: " + s.rider, X0 + 85, 4)
    p.hline(X0, 16, W, BORDER)
    # updateClock: TR at 166,4 in white
    p.set_datum(TR); p.set_color(WHITE)
    p.draw_string(s.clock, X0 + 166, 4)


def draw_dots(p, s):
    dot_gap = 8
    dots_w = (PAGE_COUNT - 1) * dot_gap
    dot_x0 = X0 + W // 2 - dots_w // 2
    for i in range(PAGE_COUNT):
        dx = dot_x0 + i * dot_gap
        if i == s.page:
            p.fill_circle(dx, 293, 2, ACCENT)
        else:
            p.draw_circle(dx, 293, 2, BORDER)


def draw_bottom_bar(p, s):
    p.hline(X0, 298, W, BORDER)
    p.set_font(FONT0)
    du = dist_unit(s)
    cv = dist_cv(s)
    pct_str = "%d%%" % s.batt_pct
    rest = "  T:%.1f%s  O:%.0f%s" % (s.trip_km * cv, du, s.odo_km * cv, du)
    wp = p.text_w(pct_str)
    wr = p.text_w(rest)
    sx = X0 + (W - (wp + wr)) // 2
    p.set_datum(ML); p.set_color(batt_color(s.batt_pct))
    p.draw_string(pct_str, sx, 308)
    p.set_datum(ML); p.set_color(DIM)
    p.draw_string(rest, sx + wp, 308)


# ---------------------------------------------------------------------------
# PAGE 1: DASH  (drawStaticDash + updateSpeed/StatPanel/Temps/Range)
# ---------------------------------------------------------------------------
def draw_speed_readout(p, s):
    spd = int(s.speed)
    p.set_color(WHITE)
    p.set_datum(TC)
    p.draw_string(str(spd), X0 + 96, 17, font=bebas(80))
    p.set_datum(TL); p.set_color(LABEL)
    p.draw_string(speed_unit(s), X0 + 10, 27, font=bebas(24))


def draw_stat(p, cx, value, unit, vcol):
    cy = 103
    g = 3
    wn = p.text_w(value, font=bebas(34))
    wu = p.text_w(unit, font=bebas(18))
    gx = cx - (wn + g + wu) // 2
    p.set_datum(ML)
    p.set_color(vcol)
    p.draw_string(value, gx, cy, font=bebas(34))
    p.set_color(DIM)
    p.draw_string(unit, gx + wn + g, cy, font=bebas(18))


def draw_temp_row(p, y, temp, pct, hot):
    p.set_font(FONT0)
    pstr = "(%d%%)" % pct
    tstr = "%dC" % int(temp)
    pw = p.text_w(pstr)
    p.set_datum(TR); p.set_color(GREEN)
    p.draw_string(pstr, X0 + 158, y)
    p.set_color(RED if hot else WHITE)
    p.draw_string(tstr, X0 + 158 - pw - 4, y)
    bar_x, bar_y, bar_w = X0 + 8, y + 11, 154
    fill_w = max(0, min(bar_w, (bar_w * pct) // 100))
    c = RED if hot else (YELLOW if pct < 70 else GREEN)
    p.hline(bar_x, bar_y, bar_w, BORDER)
    if fill_w > 0:
        p.hline(bar_x, bar_y, fill_w, c)


def draw_page_dash(p, s):
    draw_speed_readout(p, s)

    # VOLTS | WATTS panel
    sp_w, sp_h, sp_y = 162, 32, 86
    sp_x = X0 + (W - sp_w) // 2
    midx = X0 + W // 2
    p.draw_rect(sp_x, sp_y, sp_w, sp_h, BORDER)
    p.vline(midx, sp_y + 5, sp_h - 10, BORDER)
    w = int(s.peak_watts)
    draw_stat(p, (sp_x + midx) // 2, "%.1f" % s.voltage, "V", batt_color(s.batt_pct))
    draw_stat(p, (midx + sp_x + sp_w) // 2, str(w), "W", watt_color(w))

    draw_card(p, 4, 122, 162, 70, "TEMPS")
    draw_row_label(p, "MOTOR", 140)
    draw_row_label(p, "BATTERY", 156)
    draw_row_label(p, "ESC", 172)
    draw_temp_row(p, 140, s.motor_t, s.motor_p, s.motor_t > MOTOR_TEMP_LIMIT)
    draw_temp_row(p, 156, s.bat_t, s.bat_p, False)
    draw_temp_row(p, 172, s.esc_t, s.esc_p, s.esc_t > ESC_TEMP_LIMIT)

    draw_card(p, 4, 198, 162, 54, "RANGE")
    draw_row_label(p, "HOME", 216)
    draw_row_label(p, "LIMP", 232)
    p.set_font(FONT0)
    p.set_datum(TR); p.set_color(WHITE)
    du = dist_unit(s)
    cv = dist_cv(s)
    home_str = "%.1f %s" % (s.rem_km * cv, du)
    if s.avg_kmh > 1.0:
        mins = int(s.rem_km / s.avg_kmh * 60.0)
        if 0 < mins < 1000:
            home_str += " %dm" % mins
    p.draw_string(home_str, X0 + 158, 216)
    p.draw_string("%.1f %s" % (s.limp_km * cv, du), X0 + 158, 232)


# ---------------------------------------------------------------------------
# PAGE 2: POWER  (drawStaticPower + updatePower)
# ---------------------------------------------------------------------------
def draw_page_power(p, s):
    draw_card(p, 4, 22, 162, 82, "POWER")
    draw_row_label(p, "MOTOR", 40)
    draw_row_label(p, "BATTERY", 56)
    draw_row_label(p, "DUTY", 72)
    draw_row_label(p, "PEAK NOW", 88)

    draw_card(p, 4, 108, 162, 48, "ENERGY")
    draw_row_label(p, "USED", 126)
    draw_row_label(p, "REGEN", 142)

    draw_card(p, 4, 160, 162, 48, "SESSION")
    draw_row_label(p, "MAX RIDE", 178)
    draw_row_label(p, "MIN VOLT", 194)

    duty = int(s.duty)
    peak_w = int(s.peak_watts)
    draw_val(p, 40, "%.1f A" % s.motor_amps, WHITE)
    draw_val(p, 56, "%.1f A" % s.batt_amps, WHITE)
    draw_val(p, 72, "%d %%" % duty, duty_color(duty))
    draw_val(p, 88, "%d W" % peak_w, watt_color(peak_w))
    draw_val(p, 126, "%d Wh" % int(s.wh_used), WHITE)
    draw_val(p, 142, "+%d Wh" % int(s.wh_regen), GREEN)
    max_w = int(s.max_watts)
    draw_val(p, 178, "%d W" % max_w, watt_color(max_w))
    draw_val(p, 194, "%.1f V" % s.min_voltage, WHITE)


# ---------------------------------------------------------------------------
# PAGE 3: TRIP  (drawStaticTrip + updateTrip)
# ---------------------------------------------------------------------------
def draw_page_trip(p, s):
    draw_card(p, 4, 22, 162, 102, "THIS TRIP")
    draw_row_label(p, "TIME", 40)
    draw_row_label(p, "DISTANCE", 56)
    draw_row_label(p, "AVG SPEED", 72)
    draw_row_label(p, "MAX SPEED", 88)
    draw_row_label(p, "EFFICIENCY", 104)

    draw_card(p, 4, 132, 162, 40, "ODOMETER")
    draw_row_label(p, "TOTAL", 150)

    p.set_font(FONT0)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("hold L: reset + recharge" if s.demo else "hold L to reset trip",
                  X0 + W // 2, 190)

    du = dist_unit(s)
    su = "mph" if s.use_mph else "kmh"
    cv = dist_cv(s)
    avg_wh = s.avg_whkm / 0.621371 if s.use_mph else s.avg_whkm
    draw_val(p, 40, s.trip_time, WHITE)
    draw_val(p, 56, "%.1f %s" % (s.trip_km * cv, du), WHITE)
    draw_val(p, 72, "%d %s" % (int(s.avg_kmh * cv), su), WHITE)
    draw_val(p, 88, "%d %s" % (int(s.max_kmh * cv), su), WHITE)
    draw_val(p, 104, "%.1f wh/%s" % (avg_wh, du), WHITE)
    draw_val(p, 150, "%.1f %s" % (s.odo_km * cv, du), WHITE)


# ---------------------------------------------------------------------------
# PAGE 4: SETTINGS  (drawStaticSettings + updateSettings)
# ---------------------------------------------------------------------------
def draw_setting_label(p, s, label, y, idx):
    sel = (s.settings_cursor == idx)
    p.set_font(FONT0)
    p.set_datum(TL)
    if sel and s.settings_editing:
        p.fill_rect(X0 + 2, y - 2, 84, 12, GREEN)
        p.set_color(BG)
        p.draw_string(">", X0 + 4, y)
        p.draw_string(label, X0 + 12, y)
        return
    if sel:
        p.set_color(ACCENT)
        p.draw_string(">", X0 + 4, y)
    p.set_color(ACCENT if sel else DIM)
    p.draw_string(label, X0 + 12, y)


def draw_page_settings(p, s):
    draw_card(p, 4, 22, 162, 84, "WHEEL PROFILE")
    draw_setting_label(p, s, "PROFILE", 40, 0)
    draw_row_label(p, "DIAMETER", 57)
    draw_row_label(p, "GEARING", 74)
    draw_row_label(p, "POLES", 91)

    draw_card(p, 4, 110, 162, 94, "DISPLAY")
    draw_setting_label(p, s, "UNITS", 124, 1)
    draw_setting_label(p, s, "DEMO", 137, 2)
    draw_setting_label(p, s, "BRIGHT", 150, 3)
    draw_setting_label(p, s, "THEME", 163, 4)
    draw_setting_label(p, s, "HUD", 176, 5)
    draw_setting_label(p, s, "BATT", 188, 6)

    draw_card(p, 4, 208, 162, 74, "BATTERY")
    draw_setting_label(p, s, "CELLS", 224, 7)
    draw_setting_label(p, s, "PACK AH", 239, 8)
    draw_setting_label(p, s, "STOP/C", 254, 9)
    draw_setting_label(p, s, "WH/MI", 269, 10)

    p.set_font(FONT0)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("TAP -/+   HOLD = done" if s.settings_editing else "TAP = move   HOLD = edit",
                  X0 + W // 2, 288)

    draw_val(p, 40, s.wheel_name, WHITE)
    draw_val(p, 57, "%dmm" % s.wheel_diam_mm, WHITE)
    draw_val(p, 74, "%d:%d" % (s.motor_pulley, s.wheel_pulley), WHITE)
    draw_val(p, 91, "%d" % s.poles, WHITE)
    draw_val(p, 124, "MPH" if s.use_mph else "KM/H", WHITE)
    draw_val(p, 137, "ON" if s.demo else "OFF", YELLOW if s.demo else WHITE)
    draw_val(p, 150, "%d%%" % s.brightness, WHITE)
    draw_val(p, 163, CURRENT_THEME.upper(), ACCENT)
    hud_lbl = {"battery": "BATTERY", "watts": "WATTS", "safety": "SAFETY"}.get(s.hud_face, "SPEED")
    draw_val(p, 176, hud_lbl, WHITE)
    draw_val(p, 188, "VOLTS" if s.battery_focus == "volts" else "PCT", WHITE)
    draw_val(p, 224, "%dS" % s.cells, WHITE)
    draw_val(p, 239, "%.1fAh" % s.pack_ah, WHITE)
    draw_val(p, 254, "%.2fV" % s.stop_cell, WHITE)
    draw_val(p, 269, "%.1f" % s.wh_mi, WHITE)


# ---------------------------------------------------------------------------
# PAGE 5: SYSTEM  (drawStaticSystem + updateSystem)
# ---------------------------------------------------------------------------
def draw_page_system(p, s):
    draw_card(p, 4, 22, 162, 70, "DEVICE")
    draw_row_label(p, "CHIP", 40)
    draw_row_label(p, "CORES", 56)
    draw_row_label(p, "FIRMWARE", 72)

    draw_card(p, 4, 98, 162, 70, "MEMORY")
    draw_row_label(p, "HEAP", 116)
    draw_row_label(p, "MIN", 132)
    draw_row_label(p, "PSRAM", 148)

    draw_card(p, 4, 174, 162, 70, "RUNTIME")
    draw_row_label(p, "TEMP", 192)
    draw_row_label(p, "UPTIME", 208)
    draw_row_label(p, "REFRESH", 224)

    p.set_font(FONT0)
    p.set_datum(MC)
    if s.pair_code:
        p.set_color(ACCENT)
        p.draw_string("BLE PAIR  #" + s.pair_code, X0 + W // 2, 250)
    p.set_color(DIM)
    p.draw_string("reset: " + s.reset, X0 + W // 2, 264)
    p.draw_string("canvas: " + ("PSRAM" if s.canvas_psram else "SRAM"), X0 + W // 2, 277)
    p.set_color(LABEL)
    p.draw_string(s.version_full, X0 + W // 2, 290)

    draw_val(p, 40, s.chip, WHITE)
    draw_val(p, 56, "%d @ %dM" % (s.cores, s.cpu_mhz), WHITE)
    draw_val(p, 72, "%.1f/%.1fM" % (s.fw_used, s.fw_tot), WHITE)
    draw_val(p, 116, "%d kB" % s.heap, ORANGE if s.heap < 30 else WHITE)
    draw_val(p, 132, "%d kB" % s.heap_min, WHITE)
    draw_val(p, 148, "%d kB" % s.psram, WHITE)
    draw_val(p, 192, "%.1fC" % s.mcu_temp, ORANGE if s.mcu_temp > 70 else WHITE)
    draw_val(p, 208, s.uptime, WHITE)
    draw_val(p, 224, "%d FPS | %dms" % (s.fps, s.blit_ms), GREEN if s.fps >= 30 else WHITE)


# ---------------------------------------------------------------------------
# PAGE 0: BIG HUD — four faces (ui.cpp drawHud*Face)
# ---------------------------------------------------------------------------
def draw_hud_small_metric(p, x, y, w, label, value, color):
    p.draw_rect(X0 + x, y, w, 44, BORDER)
    p.set_font(FONT0)
    p.set_datum(TC); p.set_color(DIM)
    p.draw_string(label, X0 + x + w // 2, y + 5)
    f = bebas(24)
    if f.text_width(value) > w - 6:
        f = bebas(18)
    p.set_datum(MC); p.set_color(color)
    p.draw_string(value, X0 + x + w // 2, y + 29, font=f)


def draw_hud_face_label(p, label):
    p.set_font(FONT0)
    p.set_datum(TC); p.set_color(DIM)
    p.draw_string(label, X0 + W // 2, 24)


def _hud_hero(p, value, unit, y, color, hero_pt, unit_dy, unit_color):
    vw = p.text_w(value, font=bebas(hero_pt))
    uw = p.text_w(unit, font=bebas(24)) if unit else 0
    gap = 4 if unit else 0
    x = X0 + (W - (vw + gap + uw)) // 2
    p.set_datum(TL)
    p.set_color(BG)
    p.draw_string(value, x - 2, y + 2, font=bebas(hero_pt))
    p.draw_string(value, x + 2, y + 2, font=bebas(hero_pt))
    p.set_color(color)
    p.draw_string(value, x, y, font=bebas(hero_pt))
    if unit:
        p.set_color(BG)
        p.draw_string(unit, x + vw + gap - 1, y + unit_dy, font=bebas(24))
        p.draw_string(unit, x + vw + gap + 1, y + unit_dy, font=bebas(24))
        p.set_color(unit_color)
        p.draw_string(unit, x + vw + gap, y + unit_dy, font=bebas(24))


def draw_hud_hero_metric(p, value, unit, y, color):
    _hud_hero(p, value, unit, y, color, 80, 50, LABEL)


def draw_hud_hero_metric110(p, value, unit, y, color):
    _hud_hero(p, value, unit, y, color, 110, 74, WHITE)


def draw_hud_gauge_text(p, value, y, color):
    f = bebas(34)
    p.set_datum(MC)
    p.set_color(BG)
    p.draw_string(value, X0 + W // 2 - 1, y + 1, font=f)
    p.draw_string(value, X0 + W // 2 + 1, y + 1, font=f)
    p.set_color(color)
    p.draw_string(value, X0 + W // 2, y, font=f)


def draw_full_screen_gauge(p, s, fill_frac, fill_col, segments):
    gx, gy, gw, gh = X0 + 6, 26, W - 12, 266
    fill_h = round((gh - 2) * max(0.0, min(1.0, fill_frac)))
    p.draw_rect(gx - 1, gy - 1, gw + 2, gh + 2, BORDER)
    p.fill_rect(gx + 1, gy + 1, gw - 2, gh - 2, BG)
    if fill_h > 0:
        p.fill_rect(gx + 1, gy + gh - 1 - fill_h, gw - 2, fill_h, fill_col)
    p.draw_rect(gx, gy, gw, gh, BORDER)
    for i in range(segments):
        sy = gy + (i * gh) // segments
        p.hline(gx, sy, gw, BORDER)


def draw_hud_speed_face(p, s):
    spd = int(s.speed)
    hud_speed_y = 10
    p.set_datum(TC); p.set_color(WHITE)
    p.draw_string(str(spd), X0 + W // 2, hud_speed_y, font=bebas(110))

    p.set_datum(MC); p.set_color(LABEL)
    p.draw_string(speed_unit(s), X0 + W // 2, 128, font=bebas(24))

    p.hline(X0 + 8, 146, W - 16, BORDER)

    draw_battery_cells_row(p, s, 150, 18, True)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string("%d%%" % s.batt_pct, X0 + W // 2, 184, font=bebas(34))

    watts = max(0, int(s.peak_watts))
    cv, du = dist_cv(s), dist_unit(s)
    hottest = max(s.motor_t, s.esc_t)
    draw_hud_small_metric(p, 4, 202, 78, "WATTS", str(watts), watt_color(watts))
    draw_hud_small_metric(p, 88, 202, 78, "VOLTS", "%.1f" % s.voltage, batt_color(s.batt_pct))
    draw_hud_small_metric(p, 4, 250, 78, "RANGE", "%.1f%s" % (s.rem_km * cv, du), WHITE)
    draw_hud_small_metric(p, 88, 250, 78, "TEMP", "%dC" % int(hottest),
                          RED if hottest > MOTOR_TEMP_LIMIT else GREEN)


def draw_hud_battery_face(p, s):
    draw_full_screen_gauge(p, s, s.batt_pct / 100.0, batt_color(s.batt_pct), s.cells)
    if s.battery_focus == "volts":
        draw_hud_hero_metric(p, "%.1f" % s.voltage, "V", 82, WHITE)
        draw_hud_gauge_text(p, "%d%%" % s.batt_pct, 214, WHITE)
    else:
        draw_hud_hero_metric110(p, str(s.batt_pct), "%", 70, WHITE)
        draw_hud_gauge_text(p, "%.1fV" % s.voltage, 214, WHITE)


def draw_hud_watts_face(p, s):
    watts = max(0, int(s.watts))
    peak = max(0, int(s.peak_watts))
    draw_full_screen_gauge(p, s, watts / 3000.0, watt_color(watts), 10)
    draw_hud_hero_metric110(p, str(watts), "W", 70, WHITE)
    draw_hud_gauge_text(p, "%dW PEAK" % peak, 214, WHITE)


def draw_hud_safety_face(p, s):
    have = s.telemetry_live
    draw_hud_face_label(p, "SAFETY - CELL V")

    hero_col = (DIM if not have else
                RED if s.range_alert >= 3 else
                ORANGE if s.range_alert == 2 else
                YELLOW if s.range_alert == 1 else GREEN)
    draw_hud_hero_metric(p, "%.2f" % s.cell_v if have else "-.--", "V", 44, hero_col)

    state_txt = ("NO DATA" if not have else
                 "LIMP HOME" if s.range_alert >= 3 else
                 "VOLT SAG" if s.range_alert == 2 else
                 "TURN HOME" if s.range_alert == 1 else "PACK OK")
    p.set_datum(MC); p.set_color(hero_col)
    p.draw_string(state_txt, X0 + W // 2, 128, font=bebas(24))

    p.hline(X0 + 8, 146, W - 16, BORDER)

    draw_battery_cells_row(p, s, 150, 18, True)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string("%d%%" % s.batt_pct, X0 + W // 2, 184, font=bebas(34))

    min_vl = s.min_voltage if have else 0.0
    limp = s.limp_km * dist_cv(s)
    limp_unit = dist_unit(s)
    draw_hud_small_metric(p, 4, 202, 78, "MIN V", "%.1f" % min_vl,
                          RED if 0 < min_vl <= s.batt_min_v else WHITE)
    draw_hud_small_metric(p, 88, 202, 78, "SAG", str(s.sag_events),
                          ORANGE if s.sag_events > 0 else GREEN)
    draw_hud_small_metric(p, 4, 250, 78, "LIMP", "%.1f%s" % (limp if have else 0.0, limp_unit), WHITE)
    draw_hud_small_metric(p, 88, 250, 78, "LOW", "%ds" % s.low_volt_secs,
                          ORANGE if s.low_volt_secs > 0 else GREEN)


def draw_page_hud(p, s):
    if s.hud_face == "battery":
        draw_hud_battery_face(p, s)
    elif s.hud_face == "watts":
        draw_hud_watts_face(p, s)
    elif s.hud_face == "safety":
        draw_hud_safety_face(p, s)
    else:
        draw_hud_speed_face(p, s)


# ---------------------------------------------------------------------------
# PAGE 6: GRAPHS  (drawMiniGraph / drawStaticGraphs)
# ---------------------------------------------------------------------------
def draw_mini_graph(p, x, y, w, h, label, cur_text, trend, color, values, vmin, vmax):
    p.fill_rect(X0 + x, y, w, h, BG)
    p.draw_rect(X0 + x, y, w, h, BORDER)
    p.set_font(FONT0)
    p.set_datum(TL); p.set_color(DIM)
    p.draw_string(label, X0 + x + 4, y + 3)
    p.set_datum(TR); p.set_color(color)
    p.draw_string("%s %s" % (trend, cur_text), X0 + x + w - 4, y + 3)

    gx, gy, gw, gh = X0 + x + 4, y + 17, w - 8, h - 21
    p.hline(gx, gy + gh // 2, gw, BORDER)
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
    n = 60
    spd = [max(0, 22 + 14 * math.sin(i / 6.0)) for i in range(n)]
    wts = [max(0, 900 + 800 * math.sin(i / 5.0 + 1)) for i in range(n)]
    vlt = [40.5 - 0.05 * i for i in range(n)]
    mtt = [30 + 18 * (i / n) + 4 * math.sin(i / 4.0) for i in range(n)]
    su = "mph" if s.use_mph else "kmh"
    smax = 40 if s.use_mph else 65
    draw_mini_graph(p, 4, 22, 162, 56, "SPEED", "%d %s" % (int(s.speed), su), "^", ACCENT,
                    spd, 0, smax)
    draw_mini_graph(p, 4, 82, 162, 56, "WATTS", "%d W" % int(s.watts), "v",
                    watt_color(int(round(s.watts))), wts, 0, max(3000.0, s.max_watts * 1.15))
    draw_mini_graph(p, 4, 142, 162, 56, "VOLTS", "%.1f V" % s.voltage, "-",
                    batt_color(s.batt_pct), vlt, s.batt_min_v, s.batt_max_v)
    draw_mini_graph(p, 4, 202, 162, 56, "MOTOR TEMP", "%dC" % int(s.motor_t), "^",
                    RED if s.motor_t > MOTOR_TEMP_LIMIT else GREEN, mtt, 20, 100)
    p.set_font(FONT0)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("3 MIN HISTORY", X0 + W // 2, 262)


# ---------------------------------------------------------------------------
# PAGE 7: LOGS  (drawStaticLogs + updateLogs)
# ---------------------------------------------------------------------------
def draw_log_status(p, s, y):
    p.set_font(FONT0)
    p.set_datum(MC)
    if s.log_state == "full":
        p.set_color(RED); p.draw_string("! LOG STORAGE FULL", X0 + W // 2, y)
    elif s.log_state == "off":
        p.set_color(YELLOW); p.draw_string("SESSION LOGGING OFF", X0 + W // 2, y)
    else:
        p.set_color(DIM)
        p.draw_string("session: %d KB free" % s.log_free_kb, X0 + W // 2, y)


def draw_page_logs(p, s):
    draw_card(p, 4, 22, 162, 240, "RIDE SUMMARIES")
    p.set_font(FONT0)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("CSV sessions via WiFi", X0 + W // 2, 268)

    cv, du = dist_cv(s), dist_unit(s)
    su = "mph" if s.use_mph else "kmh"
    p.set_datum(TL)
    if not s.rides:
        p.set_color(DIM)
        p.draw_string("No saved rides yet.", X0 + 14, 48)
    else:
        for r, (dist_km, max_kmh, wh_used, max_w) in enumerate(s.rides[:5]):
            dist = dist_km * cv
            max_spd = max_kmh * cv
            wh_per = (wh_used / dist) if dist > 0.01 else 0
            y = 42 + r * 40
            p.set_color(ACCENT); p.draw_string("#%d" % (r + 1), X0 + 12, y)
            p.set_color(WHITE); p.draw_string("%.1f %s" % (dist, du), X0 + 34, y)
            p.set_color(DIM)
            p.draw_string("max %d %s" % (round(max_spd), su), X0 + 12, y + 12)
            p.draw_string("%d Wh/%s  %dW" % (round(wh_per), du, round(max_w)), X0 + 12, y + 24)
    draw_log_status(p, s, 252)


# ---------------------------------------------------------------------------
# FULL FRAME  (drawStaticFrame + updates)
# ---------------------------------------------------------------------------
def draw(p, s):
    p.fill_screen(BG)
    draw_top_bar(p, s)

    {HUD: draw_page_hud, DASH: draw_page_dash, POWER: draw_page_power,
     TRIP: draw_page_trip, SETTINGS: draw_page_settings, SYSTEM: draw_page_system,
     GRAPHS: draw_page_graphs, LOGS: draw_page_logs}.get(s.page, draw_page_dash)(p, s)

    owns_full_height = s.page in (HUD, SETTINGS, SYSTEM)
    if not owns_full_height:
        draw_battery_cells_row(p, s, 276, 12, True)
        draw_dots(p, s)
    draw_bottom_bar(p, s)


# ---------------------------------------------------------------------------
# BOOT SPLASH  (drawBootSplashFrame + drawBootProgress)
# ---------------------------------------------------------------------------
def draw_splash(p, s, progress=0.7):
    p.fill_screen(BG)
    p.set_font(FONT0)

    p.set_datum(TR); p.set_color(BORDER)
    p.draw_string(s.version, X0 + W - 6, 6)

    top_y, gap = 70, 4
    w_main = p.text_w("ESK8", font=bebas(80))
    w_os = p.text_w("OS", font=bebas(24))
    start_x = X0 + (W - (w_main + gap + w_os)) // 2
    p.set_datum(TL)
    p.set_color(WHITE)
    p.draw_string("ESK8", start_x, top_y, font=bebas(80))
    p.set_color(ACCENT)
    p.draw_string("OS", start_x + w_main + gap, top_y, font=bebas(24))

    p.set_font(FONT0)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("RIDE DASHBOARD", X0 + W // 2, 188)
    p.draw_string("tap L / R: pages", X0 + W // 2, 196)
    p.draw_string("hold L: units  R: HUD", X0 + W // 2, 210)
    p.draw_string("hold L+R: bridge mode", X0 + W // 2, 224)

    if s.rider:
        p.set_color(LABEL)
        p.draw_string("RIDER: " + s.rider, X0 + W // 2, 300)

    bw, bh, by = 120, 8, 250
    bx = X0 + (W - bw) // 2
    p.draw_rect(bx, by, bw, bh, BORDER)
    pct = int(max(0.0, min(1.0, progress)) * 100)
    p.fill_rect(bx + 1, by + 1, (bw - 2) * pct // 100, bh - 2, GREEN)

    p.set_datum(MC); p.set_color(YELLOW)
    p.draw_string("CONNECTING TO VESC", X0 + W // 2, 272)


# ---------------------------------------------------------------------------
# BRIDGE MODE  (services/bridge.cpp drawBridgeScreen + status + stats)
# ---------------------------------------------------------------------------
def draw_bridge(p, s):
    p.fill_screen(BG)

    p.set_datum(TC); p.set_color(ACCENT)
    p.draw_string("BRIDGE MODE", X0 + W // 2, 18, font=bebas(24))

    p.set_font(FONT0)
    p.set_color(LABEL)
    p.draw_string("VESC TOOL CONFIG", X0 + W // 2, 56)

    p.set_datum(TL)
    p.set_color(DIM);   p.draw_string("WiFi:", X0 + 12, 80)
    p.set_color(WHITE); p.draw_string(s.bridge_ssid, X0 + 46, 80)
    p.set_color(DIM);   p.draw_string("pass:", X0 + 12, 94)
    p.set_color(WHITE); p.draw_string(s.bridge_pass, X0 + 46, 94)
    p.set_color(DIM);   p.draw_string("TCP:", X0 + 12, 108)
    p.set_color(WHITE); p.draw_string(s.bridge_ipport, X0 + 40, 108)
    p.set_color(DIM);   p.draw_string("BLE:", X0 + 12, 122)
    p.set_color(WHITE); p.draw_string("ESK8-BLE", X0 + 40, 122)

    p.set_color(DIM)
    p.draw_string("Desktop: TCP connection", X0 + 12, 146)
    p.draw_string("Mobile: scan BLE in app", X0 + 12, 158)
    p.set_color(DIM);   p.draw_string("logs:", X0 + 12, 174)
    p.set_color(WHITE); p.draw_string("http://192.168.4.1", X0 + 44, 174)

    p.set_datum(TC); p.set_color(DIM)
    p.draw_string("hold L+R to exit", X0 + W // 2, 300)

    # status box
    p.fill_rect(X0 + 8, 212, W - 16, 38, BG)
    p.draw_rect(X0 + 8, 212, W - 16, 38, BORDER)
    if s.bridge_status in ("CONNECTED", "TRAFFIC"):
        sc = GREEN
    elif s.bridge_status == "ERROR":
        sc = RED
    else:
        sc = DIM
    p.set_datum(MC); p.set_color(sc)
    p.draw_string(s.bridge_status, X0 + W // 2, 232, font=bebas(24))

    # throughput lines
    p.set_font(FONT0)
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("WiFi RX %dK TX %dK STA %d" % (s.bridge_wifi_rx_k, s.bridge_wifi_tx_k, s.bridge_sta),
                  X0 + W // 2, 258)
    p.set_color(GREEN if s.bridge_ble_on else DIM)
    p.draw_string("BLE %s  RX %d TX %d" % ("ON" if s.bridge_ble_on else "--",
                                           s.bridge_ble_rx, s.bridge_ble_tx),
                  X0 + W // 2, 270)


# ---------------------------------------------------------------------------
def _read_version():
    try:
        with open(os.path.join(HERE, "version.txt")) as f:
            return "v" + f.read().strip()
    except OSError:
        return State.version


def render(s, kind="page", progress=0.7):
    p = Panel()
    if kind == "splash":
        draw_splash(p, s, progress)
    elif kind == "bridge":
        draw_bridge(p, s)
    else:
        draw(p, s)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", type=int, default=State.speed)
    ap.add_argument("--kmh", action="store_true", help="metric (default is MPH)")
    ap.add_argument("--splash", action="store_true", help="render the boot splash")
    ap.add_argument("--hud", action="store_true", help="render the Big HUD (page 0)")
    ap.add_argument("--bridge", action="store_true", help="render the VESC bridge screen")
    ap.add_argument("--progress", type=float, default=0.7)
    ap.add_argument("--batt", type=int, default=State.batt_pct, help="battery %% for color test")
    ap.add_argument("--watts", type=int, default=State.watts)
    ap.add_argument("--no-demo", action="store_true",
                    help="render as if demo mode is off (rider name in the topbar)")
    ap.add_argument("--hud-face", default=State.hud_face,
                    choices=["speed", "battery", "watts", "safety"], help="HUD face")
    ap.add_argument("--battery-focus", default=State.battery_focus,
                    choices=["pct", "volts"], help="battery HUD hero value")
    ap.add_argument("--page", type=int, default=0,
                    help="0 hud 1 dash 2 power 3 trip 4 settings 5 system 6 graphs 7 logs")
    ap.add_argument("--theme", default="cam", choices=list(THEMES), help="color theme")
    ap.add_argument("--logstate", default=State.log_state, choices=["on", "off", "full"],
                    help="LOGS-page session-logging status")
    ap.add_argument("--all", action="store_true", help="dump every page to preview_<name>.png")
    ap.add_argument("-o", "--out", default="preview.png")
    a = ap.parse_args()

    apply_theme(a.theme)

    s = State()
    s.version = _read_version()
    s.version_full = s.version + " dfcfd2d"
    s.speed = a.speed
    s.use_mph = not a.kmh
    s.batt_pct = a.batt
    s.watts = a.watts
    s.peak_watts = max(a.watts, State.peak_watts)
    s.hud_face = a.hud_face
    s.battery_focus = a.battery_focus
    s.page = 0 if a.hud else a.page
    s.log_state = a.logstate
    if a.no_demo:
        s.demo = False
    # keep test voltage consistent with the battery level (3.2-4.2 V/cell)
    if a.batt != State.batt_pct:
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
    render(s, kind, a.progress).save(a.out)
    print("wrote", a.out, "(%dx%d logical, x%d, theme=%s, firmware glyphs)" % (W, H, SCALE, a.theme))


if __name__ == "__main__":
    main()
