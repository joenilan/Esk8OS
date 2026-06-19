"""
Host-side design preview for the Longboard-Display UI.

Renders the dashboard at the EXACT physical resolution of the Lilygo
T-Display S3 (170x320 portrait) so layout can be iterated without flashing
the device or fighting Wokwi's 240x320 ILI9341 stand-in.

It mimics just enough of the TFT_eSPI API (datum-based drawString, rects,
fast h-lines, GFX free-fonts) that the layout math here can be ported 1:1
to src/main.cpp. The Bebas free-fonts are rasterised from the same
BebasNeue.ttf at the same pixel sizes the GFX headers were generated at,
so the hero number matches the device closely.

Usage:  python preview.py            -> writes preview.png (scaled x3)
        python preview.py --speed 27 --mph
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
BLUE   = (68, 136, 255)
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

    def bebas(self, px):
        if px not in self._bebas:
            self._bebas[px] = ImageFont.truetype(TTF, px)
        return self._bebas[px]

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
    # page system
    page = 0          # 0 dash, 1 power, 2 trip
    fault = ""        # "" = none, else fault name -> banner
    # page-1 / page-2 stats
    motor_amps = 28.4
    batt_amps = 14.2
    duty = 72
    wh_used = 210
    wh_regen = 18
    max_kmh = 50.0
    avg_kmh = 29.0
    ride_time = "16:34"


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


def draw(p: Panel, s: State):
    p.fill_screen(BG)
    draw_topbar(p, s)
    if s.page == 1:
        draw_page_power(p, s)
    elif s.page == 2:
        draw_page_trip(p, s)
    else:
        draw_page_dash(p, s)
    draw_cells(p, s)
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
    p.draw_string(str(s.speed), 96, 14, px=80)
    p.set_datum(TL); p.set_color(LABEL)
    p.draw_string(speed_unit(s), 10, 28, px=24)

    # -- VOLTS | WATTS panel (y 86..118) ------------------------------------
    bw, bh, by = 162, 32, 86
    bx = (W - bw) // 2
    midx = W // 2
    p.draw_rect(bx, by, bw, bh, BORDER)
    p.d.line([midx, by + 5, midx, by + bh - 5], fill=BORDER)
    cy = by + bh // 2 - 1

    def stat(cx, value, unit, vcol, npx=24, upx=18, g=3):
        wn = p.text_w(value, px=npx)
        wu = p.text_w(unit, px=upx)
        gx = cx - (wn + g + wu) / 2
        p.set_datum(ML); p.set_color(vcol)
        p.draw_string(value, int(gx), cy, px=npx)
        p.set_datum(ML); p.set_color(DIM)
        p.draw_string(unit, int(gx + wn + g), cy, px=upx)

    stat((bx + midx) // 2, "%.1f" % s.voltage, "V", batt_color(s.batt_pct))
    stat((midx + bx + bw) // 2, str(s.watts), "W", WHITE)

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
        ry += 16

    # -- RANGE card (y 198..268) --------------------------------------------
    card(p, "RANGE", 4, 198, 162, 70)
    cv, du = dist_cv(s), dist_unit(s)
    avg_wh = s.avg_whkm / 0.621371 if s.use_mph else s.avg_whkm
    rrows = [("ESTIMATED", "%.1f %s" % (s.est_km * cv, du)),
             ("REMAINING", "%.1f %s" % (s.rem_km * cv, du)),
             ("AVG. WH/%s" % du.upper(), "%.1f wh/%s" % (avg_wh, du))]
    list_rows(p, rrows, 216)


def draw_page_power(p, s):
    cv = dist_cv(s)
    su = "mph" if s.use_mph else "kmh"
    card(p, "POWER", 4, 22, 162, 82)
    list_rows(p, [("MOTOR", "%.1f A" % s.motor_amps),
                  ("BATTERY", "%.1f A" % s.batt_amps),
                  ("DUTY", "%d %%" % s.duty),
                  ("PEAK", "%d W" % s.watts)], 40)
    card(p, "ENERGY", 4, 110, 162, 54)
    list_rows(p, [("USED", "%d Wh" % s.wh_used),
                  ("REGEN", "+%d Wh" % s.wh_regen)], 128)
    card(p, "SPEED", 4, 170, 162, 54)
    list_rows(p, [("MAX", "%.0f %s" % (s.max_kmh * cv, su)),
                  ("AVG", "%.0f %s" % (s.avg_kmh * cv, su))], 188)


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
    p.draw_string("hold L to reset trip", W // 2, 190)


def draw_fault_banner(p, s):
    p.fill_rect(8, 118, W - 16, 64, RED)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string("! FAULT", W // 2, 134, px=24)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string(s.fault, W // 2, 166)


def draw_crit_overlay(p, s):
    p.fill_rect(8, 108, W - 16, 92, RED)
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string("LOW BATTERY", W // 2, 126, px=24)
    p.draw_string("STOP & CHARGE", W // 2, 158)
    p.draw_string("%d%%   %.1f V" % (s.batt_pct, s.voltage), W // 2, 182)


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
    p.set_datum(TL); p.set_color(BLUE)          # superscript, top-aligned
    p.draw_string("OS", int(x + wmain + gap), top, px=os_px)

    # tagline
    p.set_datum(MC); p.set_color(DIM)
    p.draw_string("RIDE DASHBOARD", W // 2, 168)

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
    ap.add_argument("--progress", type=float, default=0.7)
    ap.add_argument("--batt", type=int, default=State.batt_pct, help="battery %% for color test")
    ap.add_argument("--watts", type=int, default=State.watts)
    ap.add_argument("--page", type=int, default=0, help="0 dash, 1 power, 2 trip")
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
    else:
        draw(p, s)
    p.save(a.out)
    print("wrote", a.out, "(%dx%d logical, x%d)" % (W, H, SCALE))


if __name__ == "__main__":
    main()
