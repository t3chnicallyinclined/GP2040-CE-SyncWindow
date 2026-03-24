# GP2040-CE NOBD

[![Sponsor on GitHub](https://img.shields.io/badge/Sponsor-GitHub-ea4aaa?logo=github)](https://github.com/sponsors/t3chnicallyinclined)
[![Support on Ko-fi](https://img.shields.io/badge/Support-Ko--fi-FF5E5B?logo=ko-fi)](https://ko-fi.com/trisdog)
[![Donate via Strike](https://img.shields.io/badge/Donate-Strike-7B68EE?logo=bitcoin)](https://strike.me/nobd)

A fork of [GP2040-CE](https://gp2040-ce.info/) v0.7.12 that adds **NOBD (No OBD)** — a sync window that groups near-simultaneous button presses so they arrive on the same USB frame. Built for MvC2, where split LP+HP presses cause dropped dashes.

> **Also includes a native Dreamcast Maple Bus driver** — play on a real DC without an adapter. [Jump to Dreamcast wiring →](#dreamcast-native-controller)

---

## Demo (MvC2)

**Sync disabled** — dropped dashes, stray jabs

https://github.com/user-attachments/assets/df4f4f12-4077-4e27-92e2-1057e5668e74

**Sync enabled** — dashes come out clean every time

https://github.com/user-attachments/assets/a56967f7-1b35-4f8f-9fda-de62dac0b089

---

## Flash

1. Download the `.uf2` for your board from the [Releases page](https://github.com/t3chnicallyinclined/GP2040-CE-NOBD/releases)
2. Unplug your board, hold **BOOTSEL**, plug in via USB
3. Drag and drop the `.uf2` onto the `RPI-RP2` drive that appears
4. Board reboots with new firmware — done

**Supported boards:** RP2040AdvancedBreakoutBoard, Pico, PicoW, Pico2

---

## Configure

Open the web UI: hold **S2** on boot → navigate to `http://192.168.7.1` → **Settings**

| Mode | What it does |
|------|-------------|
| **NOBD Sync Window** | Groups near-simultaneous presses. Default: 5ms |
| **Stock Debounce** | GP2040-CE stock behavior. Set to 0 for raw passthrough |

**Recommended:** Start at 5ms. If you still drop dashes occasionally, try 6–8ms.

A **Release Debounce** checkbox appears in NOBD mode — enables bounce filtering on button release. Off by default. Useful for rhythm games where release bounce causes phantom inputs, not needed for fighting games.

---

## Dreamcast — Native Controller

GP2040-CE NOBD includes a native Maple Bus driver. Your fight stick talks directly to the Dreamcast — no adapter, no USB dongle, no level shifter.

**Status:** Beta — buttons, d-pad, and triggers confirmed working. Analog stick untested. VMU save/load confirmed working (saves persist through power cycle).

### What You Need

- RP2040-based GP2040-CE board
- A Dreamcast controller cable (cut from a broken controller or extension)
- Soldering iron (not necessary if you have breakout screw terminals like in gp2040 advanced breakout board)

### Cable Pinout

| Pin | Color (typical) | Signal |
|-----|----------------|--------|
| 1 | Red | VCC (+5V from console) |
| 2 | Green | SDCKA (Data/Clock A) |
| 3 | Blue | SDCKB (Data/Clock B) |
| 4 | White | Sense |
| 5 | Black | GND |

> Wire colors vary between cables — verify with a multimeter before soldering.

### Wiring to RP2040AdvancedBreakoutBoard

| DC Cable | Board Connection | Notes |
|----------|----------------|-------|
| Red (VCC) | VCC | Board is powered directly by the DC's 5V |
| Green (SDCKA) | GPIO 2 | Maple Bus Data A (default) |
| Blue (SDCKB) | GPIO 3 | Maple Bus Data B (default) |
| White (Sense) | GND | Tie directly to ground — tells DC a device is present |
| Black (GND) | GND | Common ground |

> **Important — RP2040AdvancedBreakoutBoard:** The board has a physical switch that selects between USB and the Options/Aux header. **Flip it to the Options position** before connecting to the Dreamcast, otherwise the board won't power on from the DC's 5V line.

> The Maple Bus pins default to GPIO 2/3. If you wire to different pins, update them in the web UI under **Settings → Dreamcast** so the firmware knows where to look.

### Wiring Photos

<!-- TODO: Add photo of wiring on RP2040AdvancedBreakoutBoard -->
![Wiring diagram placeholder](docs/images/wiring-placeholder.png)

<!-- TODO: Add photo of controller cable with pins labeled -->
![Controller cable pinout placeholder](docs/images/cable-placeholder.png)

<!-- TODO: Add photo of completed setup plugged into Dreamcast -->
![Completed setup placeholder](docs/images/setup-placeholder.png)

### Activating Dreamcast Mode

Hold **L1** while plugging in the board. The OLED will show the DC mode indicator.

For full Dreamcast wiring details and troubleshooting, see [docs/DREAMCAST.md](docs/DREAMCAST.md).

---

## Why NOBD?

The short version: when you press two buttons "simultaneously," your fingers are actually 2–8ms apart. At 1000Hz USB polling, that gap splits your inputs across USB frames. In MvC2 — a 25-year-old arcade game with zero input leniency — that means a stray jab instead of a dash.

NOBD holds the first press for up to 5ms so both buttons commit on the same frame. Nothing added, nothing invented — just your two presses arriving together instead of split by a timing boundary you can't see or control.

[Full technical explanation →](docs/WHY-NOBD.md)

---

## Finger Gap Tester

Measure your natural finger gap with the **[Finger Gap Tester](https://github.com/t3chnicallyinclined/finger-gap-tester)** — Python CLI or Rust GUI. Detects strays, bounces, pre-fire, and recommends a sync window value.

---

## Contributing / Building from Source

See [docs/README.md](docs/README.md) — covers how this project was built using AI-assisted firmware development (Claude Code), how to set up your build environment, and how to contribute without being a firmware expert.

---

## Links

- [Why NOBD — Technical Deep Dive](docs/WHY-NOBD.md)
- [Dreamcast Wiring Guide](docs/DREAMCAST.md)
- [Contributing & AI-Assisted Dev Guide](docs/README.md)
- [Finger Gap Tester](https://github.com/t3chnicallyinclined/finger-gap-tester)
- [GP2040-CE upstream](https://gp2040-ce.info/)
- [Releases](https://github.com/t3chnicallyinclined/GP2040-CE-NOBD/releases)
