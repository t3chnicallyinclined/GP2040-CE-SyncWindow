# W6100-EVB-Pico2 — Dreamcast Online Fighting Stick

RP2350 + W6100 hardwired Ethernet. Sub-millisecond network latency for native Dreamcast online play. No WiFi bridge needed — Ethernet built in.

**NOTE:** GPIO 23 is NOT broken out on this board. Pin layout accounts for this.

## Pin Layout

```
LEFT SIDE (pins 1-20)           RIGHT SIDE (pins 21-40)
═══════════════════             ═══════════════════════
GP0  (1)  = P1 Maple A          (40) VBUS
GP1  (2)  = P1 Maple B          (39) VSYS
     (3)  = GND                  (38) GND
GP2  (4)  = P2 Maple A           (37) 3V3_EN
GP3  (5)  = P2 Maple B           (36) 3V3
GP4  (6)  = UP                   (35) ADC_VREF
GP5  (7)  = DOWN                 (34) GP28 = S2 (Start)
     (8)  = GND                  (33) GND (analog)
GP6  (9)  = LEFT                 (32) GP27 = I2C SCL
GP7  (10) = RIGHT                (31) GP26 = I2C SDA
GP8  (11) = B1 (DC: A)           (30) RUN
GP9  (12) = B2 (DC: B)           (29) GP22 = S1 (Select)
     (13) = GND                  (28) GND
GP10 (14) = B3 (DC: X)           (27) GP21 = W6100 INTn [reserved]
GP11 (15) = B4 (DC: Y)           (26) GP20 = W6100 RSTn [reserved]
GP12 (16) = R1 (DC: Z)           (25) GP19 = W6100 MOSI [reserved]
GP13 (17) = L1 (DC: C)           (24) GP18 = W6100 SCLK [reserved]
     (18) = GND                  (23) GND
GP14 (19) = R2 (DC: RT)          (22) GP17 = W6100 CSn  [reserved]
GP15 (20) = L2 (DC: LT)          (21) GP16 = W6100 MISO [reserved]
```

## Pin Summary

| Function | GPIOs | Count |
|----------|-------|-------|
| P1 Maple Bus | 0, 1 | 2 |
| P2 Maple Bus | 2, 3 | 2 |
| Directions | 4, 5, 6, 7 | 4 |
| Face/Shoulder | 8-15 | 8 |
| S1/S2 | 22, 28 | 2 |
| W6100 Ethernet | 16-21 (on-board) | 6 |
| I2C Display | 26, 27 | 2 |
| System | 24 (VBUS), 25 (LED), 29 (VSYS) | 3 |
| Not broken out | 23 | 1 |

All pin assignments are **recommended defaults** — configurable via the web UI Settings page.

## Wiring

### Dreamcast Cables
```
P1 cable (DC Port 1):  Data A -> GP0,  Data B -> GP1,  5V + GND -> board power
P2 cable (DC Port 2):  Data A -> GP2,  Data B -> GP3,  GND only (skip 5V)
```

Only connect 5V from ONE cable. Both cables need GND for signal reference.

### Buttons (directly from stick, active-low with internal pull-ups)
```
Directions:  GP4=UP, GP5=DOWN, GP6=LEFT, GP7=RIGHT
Face:        GP8=A, GP9=B, GP10=X, GP11=Y
Shoulder:    GP12=Z(R1), GP13=C(L1)
Triggers:    GP14=RT(R2), GP15=LT(L2)
System:      GP22=Select(S1), GP28=Start(S2)
```

### Ethernet
Built-in RJ45 connector on the board. No wiring needed — just plug in an Ethernet cable.

### Display (optional)
```
GP26=SDA, GP27=SCL (I2C, 128x64 OLED)
```

## Building

```bash
GP2040_BOARDCONFIG=W6100EVBPico2 cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
cmake --build build
```

Or use the CI/CD release workflow which builds this board automatically.

## Advantages Over WiFi Bridge Setup

| | WiFi (current) | W6100 Ethernet |
|---|---|---|
| Latency | 1-5ms (unpredictable) | <1ms (deterministic) |
| Extra hardware | Pico 2 W bridge + scripts | None (built-in) |
| Setup complexity | WiFi config + UART wiring | Plug in Ethernet cable |
| RAM | 264KB (tight) | 520KB (comfortable) |
| PIO blocks | 2 (exactly full) | 3 (room to spare) |

## Status

Board config created. RP2350 PIO is instruction-compatible with RP2040 — maple_tx/maple_rx programs run unmodified. W6100 Ethernet driver integration is next step (replaces UART `pollUartRx` with UDP `recvfrom` via SPI).
