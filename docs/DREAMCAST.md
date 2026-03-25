# Dreamcast Maple Bus Driver

## Overview

GP2040-CE NOBD includes a native **Dreamcast controller output** mode. The RP2040 board communicates directly with the Dreamcast console over the Maple Bus protocol — no adapter, no USB, no level shifter required.

**Status:** Buttons, d-pad, triggers, and **VMU save/load** all confirmed working on real Dreamcast hardware. Analog stick untested.

**Supported boards:** RP2040AdvancedBreakoutBoard (tested), Pico, PicoW, Pico2

---

## Wiring Guide

### What You Need
- GP2040-CE board (RP2040-based)
- A Dreamcast controller cable (cut from a broken controller or extension cable)
- Soldering iron + wire (or screw terminals on breakout boards)

### Dreamcast Controller Cable Pinout

| Pin | Color (standard cable) | Signal |
|-----|----------------------|--------|
| 1 | Red | VCC (+5V power from console) |
| 2 | Green | SDCKA (Data/Clock Line A) |
| 3 | Blue | SDCKB (Data/Clock Line B) |
| 4 | White | Sense (tie to GND) |
| 5 | Black | GND |

> **Note:** Wire colors may vary between cables. Always verify with a multimeter.

### Wiring to RP2040AdvancedBreakoutBoard

| DC Cable | Screw Terminal | Notes |
|----------|---------------|-------|
| Red (VCC) | VCC | Powers the board from Dreamcast 5V |
| Green (SDCKA) | GPIO 2 | Maple Bus Data A (default) |
| Blue (SDCKB) | GPIO 3 | Maple Bus Data B (default) |
| White (Sense) | GND | Tells DC a device is connected |
| Black (GND) | GND | Common ground |

> **Important:** Flip the physical switch on the board from **USB to Options** before connecting to the Dreamcast. Without this, the board won't receive power from the DC's 5V line.

### GPIO Pin Configuration

Default pins are GPIO 2/3. Configurable in the web UI under **Settings → Input Mode → Dreamcast**.

**Avoid GPIO 23, 24, and 26–29** — these have on-board circuitry (SMPS, VBUS sense, ADC) that causes signal noise at Maple Bus speeds. Pins must be consecutive (Data B = Data A + 1).

---

## Setup

### Activating Dreamcast Mode

1. Connect board to PC via USB
2. Open web UI at `http://192.168.7.1`
3. Go to **Settings → Input Mode** and select **Dreamcast**
4. Set GPIO pins if using non-default wiring
5. Save and reboot into controller mode
6. Plug into Dreamcast

Alternatively, hold **L1** while plugging in the board to boot directly into Dreamcast mode (configurable hotkey).

### Input Timing

Debounce and NOBD sync windows are **unnecessary in Dreamcast mode**. The Dreamcast polls controllers every 16ms (60Hz). This 16ms interval acts as a natural sync window — simultaneous presses within that window always land on the same frame.

**Default for fresh installs:** Stock Debounce 0 (raw GPIO passthrough, zero latency).

---

## VMU (Virtual Memory Unit)

The driver emulates a standard VMU at sub-peripheral address 0x01. Games detect it as a memory card and can save/load normally.

- **256 blocks × 512 bytes = 128KB** stored in RP2040 flash
- Standard VMU filesystem: system block (255), FAT (254), file info (241-253), save area (0-199)
- Auto-formats on first boot if flash area is blank
- **VMU Manager** in the web UI: export `.bin` backups, import `.dci` saves, format VMU

### VMU Manager

Accessible from **Settings → Input Mode → Dreamcast** in the web UI.

| Feature | Description |
|---------|-------------|
| **Export** | Download full 128KB VMU as `.bin` file |
| **Import** | Upload `.dci` saves — auto byte-order conversion, replaces existing saves |
| **Format** | Wipe and re-format VMU (requires typing FORMAT to confirm) |

---

## OLED Display

### Normal Mode (default)
Shows `DC VMU:5/200` in the standard status bar format (yellow text, black background). Drawn once on boot, then **zero display overhead during gameplay**. OLED retains the static image. Redraws only when a VMU save/load occurs.

No splash screen, no profile banner, no button layout widgets — all skipped in DC mode for zero CPU overhead.

### Diagnostic Mode
**Hold S1 (Select) for 3 seconds** to toggle on/off. When on:
- Display updates continuously with live counters
- All debug counters enabled across Maple Bus, VMU, and transport layers

```
Rx:1234 Tx:1234 XF:0
VMU ok:46 er:0 fw:21
Vr:500 Vt:500
Hold S1 3s to exit
```

When toggled off: forces one redraw of the status bar, then back to zero overhead. All debug counters disabled.

---

## Button Mapping

| GP2040-CE Button | Dreamcast Button |
|-----------------|-----------------|
| B1 | A |
| B2 | B |
| B3 | X |
| B4 | Y |
| L1 | C |
| R1 | Z |
| S2 (Start) | Start |
| L2 | Left Trigger (digital → full press) |
| R2 | Right Trigger (digital → full press) |
| DPad | DPad |

Analog triggers pass through for boards with analog trigger inputs. DPad is forced to digital mode regardless of web UI setting.

---

## Architecture

### Wire-Order Design (NO DMA bswap)

Based on [MaplePad by mackieks](https://github.com/mackieks/MaplePad). All data stays in wire (network) byte order. Field extraction uses explicit `__builtin_bswap32()`. This eliminates byte-order ambiguity.

### PIO Configuration

```
PIO0 (TX): SM0 — drives both pins, DMA-fed
PIO1 (RX): SM0 — samples pins on IRQ 7 (autopush at 32 bits)
           SM1 — watches pinA edges, fires IRQ 7
           SM2 — watches pinB edges, fires IRQ 7
```

RX uses a 256-word DMA ring buffer to prevent FIFO overflow during main loop latency.

### Performance Optimization

With diagnostics off (default), the Maple Bus hot path is pure:
- Poll PIO for incoming packet
- Parse header
- Build response
- Send via DMA
- Flush RX echo

**Zero** display draws, debug counter writes, or unnecessary computation during gameplay.

### File Map

| File | Purpose |
|------|---------|
| `src/drivers/dreamcast/maple.pio` | PIO assembly — TX + RX programs |
| `src/drivers/dreamcast/maple_bus.cpp` | Transport — packet send/receive, CRC, DMA |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Protocol — button mapping, command dispatch |
| `src/drivers/dreamcast/DreamcastVMU.cpp` | VMU — filesystem, block read/write, flash |
| `src/addons/display.cpp` | Display — DC-specific zero-overhead mode |
| `src/webconfig.cpp` | VMU Manager API endpoints |

---

## Known Issues & Limitations

1. **Analog stick untested** — mapped but not verified on hardware
2. **No vibration/rumble** — ACKs rumble commands but doesn't actuate
3. **Single controller only** — one port, one controller
4. **Web UI requires USB** — when in DC mode, USB is not initialized. Configure via USB before connecting to Dreamcast
5. **VMU flash writes** — brief ~45ms pause during saves (Core 1 handles flash while Core 0 spins in RAM)

---

## References

- [MaplePad by mackieks](https://github.com/mackieks/MaplePad) — Architecture reference for wire-order design
- [DreamPicoPort by OrangeFox86](https://github.com/OrangeFox86/DreamPicoPort) — Alternative implementation, byte order reference
- [marcus comstedt's Maple Bus page](http://mc.pp.se/dc/maplebus.html) — Protocol reference
- [Dreamcast Wiki - Maple Bus](https://dreamcast.wiki/Maple_bus) — Protocol documentation
- [VMU Expert Reference](vmu_expert_reference.md) — Comprehensive VMU filesystem/protocol docs

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| nobd-18 | 2025-03-25 | Zero display overhead, diagnostic toggle (S1 hold 3s), all debug counters gated |
| nobd-17 | 2025-03-25 | Hot path optimization — removed dead code, gated diagnostics |
| nobd-16 | 2025-03-25 | VMU Manager web UI, DCI import, Dreamcast UI in Input Mode tab |
| nobd-15 | 2025-03-24 | VMU byte order fix, save/load confirmed working on real hardware |
| Beta 1 | 2025-03-22 | Initial working Dreamcast output — buttons, dpad, triggers |
