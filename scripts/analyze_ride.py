"""Summarize an ESK8OS ride CSV and estimate source resistance (pack+wiring)
from the volts-vs-battery-current slope. Usage: python scripts/analyze_ride.py <csv>"""
import sys, csv, statistics

path = sys.argv[1]
rows = []
with open(path) as f:
    for line in f:
        line = line.strip()
        if line.startswith("t_s,"):
            header = line.split(",")
            break
    rdr = csv.DictReader(f, fieldnames=header)
    for r in rdr:
        try:
            rows.append({k: float(r[k]) for k in header})
        except (ValueError, TypeError):
            pass

if not rows:
    print("no data rows")
    sys.exit()

def col(k): return [r[k] for r in rows if k in r]

dur = rows[-1]["t_s"] - rows[0]["t_s"]
dist = max(col("dist_mi"))
spd = col("speed_mph")
volts = col("volts")
batt_a = col("batt_a")
watts = col("watts")

print(f"file        : {path}")
print(f"samples     : {len(rows)}  ({dur:.0f} s)")
print(f"distance    : {dist:.2f} mi")
print(f"speed       : max {max(spd):.1f}  avg {statistics.mean(spd):.1f} mph")
print(f"volts       : {max(volts):.2f} -> {min(volts):.2f}  (rest~{max(volts):.2f})")
print(f"watts       : max {max(watts):.0f}")
print(f"batt_a      : max {max(batt_a):.1f} A")
print(f"temps       : motor max {max(col('motor_c')):.0f}C  esc max {max(col('esc_c')):.0f}C")

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
