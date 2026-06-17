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
    product = "ESK8 OS"
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


# ---------------------------------------------------------------------------
# THE LAYOUT  (logical 170x320 -- port these numbers straight into main.cpp)
# ---------------------------------------------------------------------------
def draw(p: Panel, s: State):
    p.fill_screen(BG)
    unit = "MPH" if s.use_mph else "KM/H"

    # -- TOP BAR (y 0..15) ---------------------------------------------------
    p.set_datum(TL); p.set_color(DIM)
    p.draw_string(s.product, 4, 4)
    p.set_datum(TC); p.set_color(DIM)
    p.draw_string("RIDER: " + s.rider, 85, 4)
    p.set_datum(TR); p.set_color(WHITE)
    p.draw_string(s.clock, 166, 4)
    p.hline(0, 16, W, BORDER)

    # -- SPEED (y 20..118) ---------------------------------------------------
    p.set_datum(MC); p.set_color(WHITE)
    p.draw_string(str(s.speed), 80, 70, px=80)
    p.set_datum(TR); p.set_color(LABEL)
    p.draw_string(unit, 164, 26)

    # -- PRO MODE pill (y 122..144) -----------------------------------------
    pmw, pmh, pmy = 84, 22, 122
    pmx = (W - pmw) // 2
    p.fill_rect(pmx, pmy, pmw, pmh, WHITE)
    p.set_datum(MC); p.set_color(BG)
    p.draw_string("PRO MODE", W // 2, pmy + pmh // 2)

    # -- TEMPS card (y 150..212) --------------------------------------------
    card(p, "TEMPS", 4, 150, 162, 62)
    rows = [("MOTOR", s.motor_t, s.motor_p),
            ("BATTERY", s.bat_t, s.bat_p),
            ("ESC", s.esc_t, s.esc_p)]
    ry = 166
    for label, t, pct in rows:
        p.set_datum(TL); p.set_color(DIM)
        p.draw_string(label, 12, ry)
        pstr = " (%d%%)" % pct
        p.set_datum(TR); p.set_color(GREEN)
        p.draw_string(pstr.strip(), 158, ry)
        pw = p.text_w(pstr.strip())
        p.set_datum(TR); p.set_color(WHITE)
        p.draw_string("%d°C" % t, int(158 - pw - 4), ry)
        ry += 14

    # -- RANGE card (y 216..278) --------------------------------------------
    card(p, "RANGE", 4, 216, 162, 62)
    du = "mi" if s.use_mph else "km"
    cv = 0.621371 if s.use_mph else 1.0          # km -> mi for distances
    avg_wh = s.avg_whkm / 0.621371 if s.use_mph else s.avg_whkm  # wh/km -> wh/mi
    rrows = [("ESTIMATED", "%.1f %s" % (s.est_km * cv, du)),
             ("REMAINING", "%.1f %s" % (s.rem_km * cv, du)),
             ("AVG. WH/%s" % du.upper(), "%.1f wh/%s" % (avg_wh, du))]
    ry = 232
    for label, val in rrows:
        p.set_datum(TL); p.set_color(DIM)
        p.draw_string(label, 12, ry)
        p.set_datum(TR); p.set_color(WHITE)
        p.draw_string(val, 158, ry)
        ry += 14

    # -- BATTERY CELLS (y 282..294) -----------------------------------------
    cells = 10
    cw, ch, gap = 13, 11, 2
    total = cells * cw + (cells - 1) * gap
    sx = (W - total) // 2
    filled = round(s.batt_pct * cells / 100)
    for i in range(cells):
        cx = sx + i * (cw + gap)
        p.draw_rect(cx, 282, cw, ch, BORDER)
        if i < filled:
            cc = RED if filled <= 2 else GREEN
            p.fill_rect(cx + 1, 283, cw - 2, ch - 2, cc)

    # -- BOTTOM BAR (y 300..318) --------------------------------------------
    p.hline(0, 298, W, BORDER)
    p.set_datum(MC); p.set_color(DIM)
    odo = s.odo_km * (0.621371 if s.use_mph else 1.0)
    p.draw_string("%d%%  |  %.1fV  |  %d%s" % (s.batt_pct, s.voltage, odo, du),
                  W // 2, 306)


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
    ap.add_argument("-o", "--out", default="preview.png")
    a = ap.parse_args()

    s = State()
    s.speed = a.speed
    s.use_mph = not a.kmh

    p = Panel()
    if a.splash:
        draw_splash(p, s, a.progress)
    else:
        draw(p, s)
    p.save(a.out)
    print("wrote", a.out, "(%dx%d logical, x%d)" % (W, H, SCALE))


if __name__ == "__main__":
    main()
