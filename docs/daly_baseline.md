# Daly BMS ZOMBIESK8-BMS — verified settings baseline

Captured from SMART BMS Pro (2026-08-29, pack fully charged, 7 cycles).
This is the ground truth EVEE's read-only Modbus surface (0x145..0x149) and
the 0x90-0x98 telemetry are validated against. EVEE never writes any of
these — diagnostics only.

## Voltage / current (app: Voltage-Current)

| setting | value | EVEE cross-check |
|---|---|---|
| Cell Overvolt Protect | 4.25 V | cells rest 4.19 V at full — inside limit |
| Cell Undervolt Protect | 2.7 V | EVEE limp floor 3.17 V/cell fires first |
| Total Overvolt Protect | 42.5 V | pack rests 41.9 V full — inside |
| Total Undervolt Protect | 27.0 V | EVEE home floor 41.3 V pack-equivalent fires first |
| Volt Diff Protect | 0.3 V | EVEE alert threshold 100 mV — fires first |
| Charge Overcurrent Protect | 20.0 A | not yet read via Modbus (register 0x144 range) |
| Discharge Overcurrent Protect | 45.0 A | **matches Modbus protect1 45.0 A / 1000 ms** |
| Short Circuit Current | 600 A | hardware-level, EVEE does not read |
| HW Short Circuit Protect | Open | disabled |
| Board 1 Cell Qty | 10 | matches cellCount=10 on the wire |
| Board 2/3 Cell Qty | 0 | single board |
| Current Collection Switch | VADC | — |

## Discharge O/C chain (Modbus-verified + app)

- warning 36.0 A → `0x98` byte2 bit2 (fires 2-11× per commute — 18 A/side
  × 2 = 36 A sits exactly at this threshold by design)
- protect1 45.0 A / 1000 ms → byte2 bit3 — opened the discharge MOS on the
  2026-07-30 validation ride (sustained 44.5-44.9 A for ~3 s)
- protect2 60.0 A / 500 ms → byte2 bit4 (unreached)

## Temperature (app: Temperature Settings)

| setting | value |
|---|---|
| Charge High / Low | 45 °C / 2 °C |
| Discharge High / Low | 55 °C / -20 °C |
| Temp Diff Protect | 15 °C |
| Fan Start | -- |
| Heating Start / Stop | 0 °C / 0 °C |
| Temp Ctrl Qty | 2 (matches tempCount=2) |
| NTC Offline Protect | Close |

EVEE pack temps ride 18-33 °C — far inside all of these.

## Balance (app: Balance Settings)

| setting | value | note |
|---|---|---|
| Mode | Passive | |
| Balance Start Voltage | 4.1 V | **this is why the pack rests at ~4.19 V/cell and the 0x98 frame shows the byte-0 high-voltage L1 warning (0x11) at full charge** — balancing holds cells near 4.1+ V, and the L1 cell-high threshold sits just above |
| Balance Stop Voltage | -- | |
| Balance Start Volt Diff | 0.02 V | 20 mV — tighter than EVEE's 50 mV "weak cell" display threshold |
| Balance Start Current | 1.0 A | |
| Active Balance Switch | Open | |

## Cell parameters (app: Cell Parameters)

| setting | value | EVEE cross-check |
|---|---|---|
| Battery Type | Li-ion | |
| Rated Capacity | 32.0 Ah | matches rider override packAh=32.0 |
| Sleep Wait Time | 3600 s | BMS may sleep 1 h after current stops — relevant to parked-link stability |
| SOC Setting | 100.0 % | coulomb counter primed at full |
| SOC Cal Zero / 100 | -- | never force-recalibrated |
| Comm Method | RS485 | UART-K bridge independent |
| Inverter Type | NONE | |
| Capacity Learn Switch | Close | BMS does not self-learn capacity |
| Key control MOS | Disable | MOS purely automatic |
| BMS SW | 12_240601_K00T | |

## EVEE's alarm thresholds vs the BMS's own

| condition | BMS acts at | EVEE alerts at |
|---|---|---|
| discharge current | warn 36 A / P1 45 A / P2 60 A | O/C warning = ALERT banner (11), protect = BMS CUT banner |
| cell delta | Volt Diff Protect 0.3 V | delta ≥ 100 mV = red, ≥ 50 mV = min-cell highlight |
| cell voltage high | 4.25 V protect | L1 warning byte (0x11) shown as verified WARNING |
| cell voltage low | 2.7 V protect | EVEE home/limp floors (3.47/3.17 V) fire long before |
| temp | 55 °C dis / 45 °C chg | pack rides 18-33 °C |

Layering is correct everywhere: EVEE's warnings fire before the BMS acts,
and the BMS acts before cell damage territory.
