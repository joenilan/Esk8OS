"""Ohmic source resistance from instantaneous current steps. Between consecutive
1 s samples, R = -dVolts/dAmps over large current changes — this isolates the
ohmic drop and is immune to slow SoC depletion and polarization (the things that
corrupt a whole-ride V-vs-I fit). Usage: python transient_r.py <csv> [<csv> ...]"""
import sys, statistics

def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("t_s,"):
                hdr = line.strip().split(","); break
        for line in f:
            p = line.strip().split(",")
            if len(p) >= 11:
                try: rows.append({hdr[i]: float(p[i]) for i in range(11)})
                except ValueError: pass
    return rows

def transient_R(rows, min_di=8.0, min_v=30):
    rs = []
    for a, b in zip(rows, rows[1:]):
        if b["t_s"] - a["t_s"] > 1.5: continue          # consecutive only
        if a["volts"] <= min_v or b["volts"] <= min_v: continue  # skip brownout
        di = b["batt_a"] - a["batt_a"]
        dv = b["volts"] - a["volts"]
        if abs(di) >= min_di:
            r = -dv / di
            if 0.0 < r < 1.0:                            # sane ohmic range
                rs.append(r)
    return rs

for path in sys.argv[1:]:
    rs = transient_R(load(path))
    print(f"\n=== {path} ===")
    if len(rs) < 8:
        print(f"  only {len(rs)} usable current-step events")
        continue
    rs.sort()
    med = statistics.median(rs)
    print(f"  current-step events: {len(rs)}")
    print(f"  ohmic R  median {med*1000:.0f} mohm  ({med:.3f} ohm)")
    print(f"           p25 {rs[len(rs)//4]*1000:.0f} / p75 {rs[3*len(rs)//4]*1000:.0f} mohm")
