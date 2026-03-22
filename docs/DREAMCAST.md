# Dreamcast Maple Bus Driver — Beta Implementation Guide

## Overview

GP2040-CE NOBD now includes a native **Dreamcast controller output** mode. The RP2040 board communicates directly with the Dreamcast console over the Maple Bus protocol — no adapter, no USB, no level shifter required.

**Status:** Beta — fully functional on real hardware (buttons, dpad, triggers, analog stick).

**Supported boards:** RP2040AdvancedBreakoutBoard (tested), Pico, PicoW, Pico2

---

## Wiring Guide

### What You Need
- GP2040-CE board (RP2040-based)
- A Dreamcast controller cable (cut from a broken controller or extension cable)
- Soldering iron + wire

### Dreamcast Controller Cable Pinout

The Dreamcast controller connector has 5 pins:

| Pin | Color (standard cable) | Signal |
|-----|----------------------|--------|
| 1 | Red | VCC (+5V power from console) |
| 2 | Green | SDCKA (Data/Clock Line A) |
| 3 | Blue | SDCKB (Data/Clock Line B) |
| 4 | White | Sense (active low — active connection indicator) |
| 5 | Black | GND |

> **Note:** Wire colors may vary between cables. Always verify with a multimeter.

### Wiring to RP2040AdvancedBreakoutBoard

Connect to the **screw terminal** on the board:

| DC Cable | Screw Terminal | Notes |
|----------|---------------|-------|
| Red (VCC) | VCC | Powers the board from Dreamcast 5V |
| Green (SDCKA) | GPIO pin configured as DC Data 1 | Default: GPIO 28 |
| Blue (SDCKB) | GPIO pin configured as DC Data 2 | Default: GPIO 29 |
| White (Sense) | GND | Directly tie to ground (tells DC a device is connected) |
| Black (GND) | GND | Common ground |

> **Important:** The Sense pin must be tied to GND for the Dreamcast to detect the controller. This tells the console a peripheral is plugged in.

### GPIO Pin Configuration

The Dreamcast data pins are configured in `proto/enums.proto` and default to:
- **SDCKA (Data 1):** GPIO 28
- **SDCKB (Data 2):** GPIO 29

These can be changed in the board configuration or web UI.

### Power Notes

- The Dreamcast provides **5V** on the VCC line, which powers the RP2040 board directly
- The Maple Bus signals operate at **3.3V TTL** — the RP2040's native GPIO voltage. No level shifter is needed
- If your joystick/buttons need 5V power (e.g., Suzo Happ induction joysticks with Hall effect sensors), they should draw from the same 5V rail coming from the Dreamcast VCC line
- When in Dreamcast mode, the USB port is not initialized — all power comes from the Dreamcast

---

## How It Works

### Architecture

The Dreamcast Maple Bus is a two-wire serial protocol. The RP2040's PIO (Programmable I/O) hardware handles the low-level signal timing:

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  Dreamcast   │────▶│   PIO RX     │────▶│  MapleBus    │
│  Console     │     │  (3 SMs on   │     │  Decoder     │
│              │◀────│   PIO1)      │     │  (maple_bus  │
│              │     │              │     │   .cpp)       │
│              │     │   PIO TX     │     │              │
│              │     │  (1 SM on    │◀────│              │
│              │     │   PIO0)      │     │              │
└──────────────┘     └──────────────┘     └──────────────┘
                                                │
                                                ▼
                                          ┌──────────────┐
                                          │ Dreamcast    │
                                          │ Driver       │
                                          │ (protocol    │
                                          │  + button    │
                                          │  mapping)    │
                                          └──────────────┘
                                                │
                                                ▼
                                          ┌──────────────┐
                                          │  GP2040-CE   │
                                          │  Gamepad     │
                                          │  (reads GPIO │
                                          │   buttons)   │
                                          └──────────────┘
```

### File Map

| File | Purpose |
|------|---------|
| `src/drivers/dreamcast/maple.pio` | PIO assembly — TX (1 SM on PIO0) and RX (3 SMs on PIO1) |
| `src/drivers/dreamcast/maple_bus.cpp` | Transport layer — packet send/receive, CRC, byte encoding |
| `headers/drivers/dreamcast/maple_bus.h` | Protocol structures (packet headers, device info, controller condition) |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Application driver — button mapping, sync modes, DC protocol handling |
| `headers/drivers/dreamcast/DreamcastDriver.h` | Driver header with sync mode enum and debug fields |
| `src/gp2040.cpp` | Main loop integration — Dreamcast boot path, gamepad reading |
| `src/drivermanager.cpp` | Input mode enum routing — `INPUT_MODE_DREAMCAST` case |
| `proto/enums.proto` | `INPUT_MODE_DREAMCAST = 7`, GPIO pin definitions |
| `proto/config.proto` | `dcSyncMode`, `dcSyncWindow` config fields |
| `src/config_utils.cpp` | Default config values for DC sync mode/window |
| `src/webconfig.cpp` | Web UI API for DC configuration |

### Protocol Flow

1. **Console sends Device Request** (command 1) — "Who are you?"
2. **We respond with Device Info** (command 5) — Report as HKT-7300 arcade stick with 11 buttons
3. **Console sends Get Condition** (command 9) — "What are your inputs?" (every ~16.7ms = 60Hz)
4. **We respond with Controller Condition** (command 8) — Current button/trigger/stick state
5. **Any unknown command** → We respond with ACK (command 7)

### Button Mapping

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
| DPad Up/Down/Left/Right | DPad Up/Down/Left/Right |

Analog triggers (LT/RT) also pass through for boards with analog trigger inputs.

### Byte Order Pipeline

This was the trickiest part of the implementation. The Maple Bus has a specific byte order:

```
C struct (little-endian ARM)
    → bswap32 in sendPacket()
    → PIO sends MSB-first
    → Wire receives LSByte-first
```

The controller condition struct must be in **host byte order** matching DreamPicoPort's layout:
```
[leftTrigger, rightTrigger, buttonsHi, buttonsLo, joyY2, joyX2, joyY, joyX]
```

The `sendPacket()` function applies `__builtin_bswap32()` to each 32-bit word before pushing to the PIO FIFO. This converts the little-endian ARM struct layout to the correct bit order for PIO's MSB-first output, which results in LSByte-first on the wire — exactly what the Dreamcast expects.

### CRC Calculation

Maple Bus uses an 8-bit XOR-based CRC:
1. XOR all 32-bit payload words together
2. Fold the 32-bit result to 8 bits: `(xor >> 24) ^ (xor >> 16) ^ (xor >> 8) ^ xor`
3. Store in the low byte of the CRC word
4. `sendPacket()`'s bswap32 moves it to bits 31-24 for PIO MSB-first output

### DC Sync Modes

The driver supports three sync modes for managing input timing relative to the Dreamcast's 60Hz polling:

| Mode | Description | Use Case |
|------|-------------|----------|
| **OFF (0)** | Raw gamepad state sent immediately on each poll | Lowest latency, no input grouping |
| **Accumulate (1)** | Collects all button presses between polls, sends accumulated state | Default — prevents dropped inputs between 60Hz polls |
| **Window (2)** | Delays response after detecting a press to group simultaneous inputs | Fighting games — ensures dashes/throws register together |

---

## NOBD Integration

When in Dreamcast mode, the NOBD sync window still works at the GPIO level. The flow is:

1. **NOBD sync window** (`syncGpioGetAll()`) groups near-simultaneous button presses at the GPIO reading stage
2. **DC Accumulate mode** then collects those grouped presses between Dreamcast 60Hz polls
3. The combined effect: presses within the NOBD window are guaranteed to arrive on the same Dreamcast frame

This is the same NOBD benefit as USB mode — reliable dashes, throw techs, and multi-button inputs.

### DPad Mode

The Dreamcast driver forces `DPAD_MODE_DIGITAL` regardless of the web UI setting. This is necessary because:
- The Dreamcast expects digital dpad data in the button bytes
- If DPad Mode is set to "Left Analog", GP2040-CE routes dpad inputs to analog axes and zeros `state.dpad`
- The DC driver reads `state.dpad` for directional buttons, so it would see nothing

---

## Debug Display

When a Dreamcast driver is active, the OLED debug screen shows:

```
Line 1: Rx:XXXXX (packets received from DC)
Line 2: Sy:XXXXX Tx:XXXXX (sync sequences / packets sent)
Line 3: XF:XXXXX Fi:XXXXX (XOR failures / FIFO count)
Line 4: P:XX Cmd:X (pin state / last command)
Line 5: DC:XXXX GP:XX D:X (DC buttons / gamepad buttons / dpad)
```

- **Rx = Tx** means healthy bidirectional communication
- **XF:0** means all CRC checks pass (some XF is normal — counts self-echo detection)
- **DC:FFFF** = all buttons released, **DC:FFFB** = A pressed (inverted logic: 0=pressed)
- **D:0** = no dpad pressed, **D:1** = Up, **D:2** = Down, **D:4** = Left, **D:8** = Right

---

## Known Issues & Limitations (Beta)

1. **XOR failures (XF counter)**: The XF counter slowly increases during normal operation. This is because the RX PIO picks up echo from our own TX transmissions. The `flushRx()` call after each TX clears most of these, but some slip through. This does not affect functionality — the driver correctly ignores invalid packets.

2. **No VMU/memory card emulation**: The driver reports as a standard controller only. Memory card save/load requires a real VMU in a separate port.

3. **No vibration/rumble**: The driver doesn't respond to vibration pack commands. Could be added in future.

4. **Single controller only**: Currently supports one controller on one port. Multi-controller support would require additional PIO state machines.

5. **Web UI mode switching**: Changing away from Dreamcast mode in the web UI requires a USB connection. When in DC mode, the USB stack is not initialized, so the web UI is inaccessible. Change modes via USB before connecting to Dreamcast.

6. **Pin assignment**: SDCKA/SDCKB pins are hardcoded in the board config. Changing them requires rebuilding firmware.

---

## Implementation Decisions — Pros & Cons

### Current Approach: PIO-based bit-banging (from charcole/Dreamcast-PopnMusic)

**Pros:**
- Proven working — charcole's code runs on bare Pico with no level shifter
- PIO handles precise timing requirements (~1MHz Maple Bus clock)
- No external hardware needed beyond wire
- Runs entirely on Core 0 — Core 1 available for other tasks
- Small code footprint (~800 lines total)

**Cons:**
- Uses 1 SM on PIO0 (TX) + 3 SMs on PIO1 (RX) = 4 of 8 total SMs
- RX uses polling (not DMA) — `pollReceive()` must be called frequently
- Debug/development is difficult — PIO programs are hard to trace
- The triple-SM RX approach (edge detect + two data samplers) is complex

### Alternative Approaches for Future

1. **DMA-based RX** (from OrangeFox86/DreamPicoPort): Single RX SM with DMA to capture raw samples, then CPU decodes. Frees up 2 PIO SMs but needs more CPU for decoding.

2. **Core 1 dedicated**: Run the entire Maple Bus driver on Core 1 with tight polling. Guarantees response timing but blocks Core 1 from other use.

3. **Interrupt-driven**: Use PIO IRQs for packet detection instead of polling. Lower CPU overhead between packets but more complex interrupt handling.

4. **Hardware UART approach**: Some have used UART peripherals for Maple Bus by encoding/decoding the two-wire protocol as UART frames. Simpler PIO code but requires creative signal routing.

---

## References

- [charcole/Dreamcast-PopnMusic](https://github.com/charcole/Dreamcast-PopnMusic) — Original PIO implementation our driver is based on
- [OrangeFox86/DreamPicoPort](https://github.com/OrangeFox86/DreamPicoPort) — Alternative implementation with level shifters (reference for byte order)
- [mackieks/MaplePad](https://github.com/mackieks/MaplePad) — Another Pico-based Maple Bus implementation
- [Dreamcast Wiki - Maple Bus](https://dreamcast.wiki/Maple_bus) — Protocol documentation, confirms 3.3V TTL
- [marcus comstedt's Maple Bus page](http://mc.pp.se/dc/maplebus.html) — Detailed protocol reference

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| Beta 1 | 2026-03-22 | Initial working Dreamcast output — buttons, dpad, triggers, analog stick |
