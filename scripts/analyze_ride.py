"""Summarize an ESK8OS ride/session CSV and estimate source resistance
(pack+wiring) from the volts-vs-battery-current slope.
Usage: python scripts/analyze_ride.py <csv>"""
import sys, csv, statistics

path = sys.argv[1]
rows = []
with open(path) as f:
    for line in f:
        line = line.strip()
        if line.startswith("t_s,") or line.startswith("uptime_s,"):
            header = line.split(",")
            break
    rdr = csv.DictReader(f, fieldnames=header)
    for r in rdr:
        row = {}
        for k in header:
            try:
                row[k] = float(r[k])
            except (ValueError, TypeError):
                row[k] = r.get(k, "")
        if isinstance(row.get(header[0]), (int, float)):
            rows.append(row)

if not rows:
    print("no data rows")
    sys.exit()

def col(k): return [r[k] for r in rows if k in r]
def ncol(k): return [r[k] for r in rows if isinstance(r.get(k), (int, float))]

time_key = "uptime_s" if "uptime_s" in rows[0] else "t_s"
dist_key = "trip_mi" if "trip_mi" in rows[0] else ("dist_mi" if "dist_mi" in rows[0] else "dist_km")
speed_key = "speed_mph" if "speed_mph" in rows[0] else "speed_kmh"
speed_unit = "mph" if speed_key.endswith("mph") else "kmh"
dist_unit = "mi" if dist_key.endswith("mi") else "km"

dur = rows[-1][time_key] - rows[0][time_key]
dist = max(ncol(dist_key))
spd = ncol(speed_key)
volts = ncol("volts")
batt_a = ncol("batt_a")
watts = ncol("watts")

print(f"file        : {path}")
print(f"samples     : {len(rows)}  ({dur:.0f} s)")
print(f"distance    : {dist:.2f} {dist_unit}")
print(f"speed       : max {max(spd):.1f}  avg {statistics.mean(spd):.1f} {speed_unit}")
print(f"volts       : {max(volts):.2f} -> {min(volts):.2f}  (rest~{max(volts):.2f})")
print(f"watts       : max {max(watts):.0f}")
print(f"batt_a      : max {max(batt_a):.1f} A")
if "m1a" in rows[0] and "m2a" in rows[0]:
    print(f"motor A     : m1 max {max(ncol('m1a')):.1f} A  m2 max {max(ncol('m2a')):.1f} A  combined max {max(ncol('motor_a')):.1f} A")
if "max_batt_a" in rows[0]:
    print(f"max batt A  : {max(ncol('max_batt_a')):.1f} A")
if "cell_v" in rows[0]:
    print(f"cell volts  : loaded min {min(ncol('cell_v')):.2f} V/cell")
if "home_s" in rows[0]:
    print(f"sag/limp    : {max(ncol('sag_events')):.0f} sag events, {max(ncol('home_s')):.0f}s below home, {max(ncol('limp_s')):.0f}s below limp")
print(f"temps       : motor max {max(ncol('motor_c')):.0f}C  esc max {max(ncol('esc_c')):.0f}C")

# Source resistance: under load, V ~ Vrest - I*R. Linear fit V vs I over loaded pts.
pts = [(r["batt_a"], r["volts"]) for r in rows if r["batt_a"] > 1.0]
if len(pts) >= 5:
    n = len(pts)
    sx = sum(i for i, _ in pts); sy = sum(v for _, v in pts)
    sxx = sum(i*i for i, _ in pts); sxy = sum(i*v for i, v in pts)
    denom = n*sxx - sx*sx
    if denom != 0:
        slope = (n*sxy - sx*sy) / denom          # dV/dI  (negative)
        intercept = (sy - slope*sx) / n           # ~ rest voltage
        r_milliohm = -slope * 1000.0
        print(f"V-vs-I fit  : Vrest~{intercept:.2f} V, slope {slope*1000:.1f} mV/A")
        print(f"~source R   : ~{r_milliohm:.0f} mohm  -- ROUGH ONLY (confounded by SoC drift and")
        print(f"              single-vs-dual-motor current scaling). Use transient_r.py for real ohmic R.")
else:
    print(f"source R    : not enough loaded points (got {len(pts)}, need >=5)")
