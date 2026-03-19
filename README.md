# GP2040-CE NOBD

[![Sponsor on GitHub](https://img.shields.io/badge/Sponsor-GitHub-ea4aaa?logo=github)](https://github.com/sponsors/t3chnicallyinclined)
[![Support on Ko-fi](https://img.shields.io/badge/Support-Ko--fi-FF5E5B?logo=ko-fi)](https://ko-fi.com/trisdog)
[![Donate via Strike](https://img.shields.io/badge/Donate-Strike-7B68EE?logo=bitcoin)](https://strike.me/nobd)

A fork of [GP2040-CE](https://gp2040-ce.info/) v0.7.12 that adds **NOBD (No OBD)** — a sync window that groups near-simultaneous button presses so they arrive on the same USB frame. Built for MvC2, where dropped dashes from split LP+HP presses are a constant problem.

> **Minimal latency, maximum reliability.** Stock debounce accepts each press instantly but can't group them — your "simultaneous" buttons arrive on different USB frames. NOBD holds presses for up to 5ms to guarantee grouping. That's less than a third of one game frame (16.67ms), and the same timing budget stock debounce already uses for bounce filtering.

## Demo (MvC2)

**Sync disabled** — dropped dashes, stray jabs

https://github.com/user-attachments/assets/df4f4f12-4077-4e27-92e2-1057e5668e74

**Sync enabled** — dashes come out clean every time

https://github.com/user-attachments/assets/a56967f7-1b35-4f8f-9fda-de62dac0b089

## About

I'm a cloud engineer, not a firmware dev. I came back to MVC2 after a 15-year hiatus, started playing on Steam, and immediately noticed I was dropping dashes constantly. That sent me down a rabbit hole. **MVC2 is the only fighting game I play and the only game I've tested this with.** The sync window may help other fighting games that require simultaneous button presses, but many modern games (SF6, Guilty Gear, Skullgirls, etc.) have their own input leniency systems that may already handle this. I can't make claims about games I haven't tested.

Everything here was pieced together from datasheets, API docs, community threads, trial and error, and a lot of back-and-forth with Claude AI. If you spot something wrong, feel free to correct me.

## The Problem: Frame Boundaries

When you press two buttons "at the same time," your fingers are actually 2-8ms apart. Games read input once per frame (~16.67ms at 60fps). If your two presses straddle a frame boundary, the game sees them on separate frames — LP on one frame, HP on the next. In MvC2, that means a stray jab instead of a dash.

```
 Case 1: Both presses land within the same frame → DASH ✓

   Frame N poll         Frame N+1 poll
        ↓                     ↓
   ─────┼─────────────────────┼─────────
        :    LP    HP         :
        :    ↑     ↑          :
        :    T=2   T=5        :
        Game reads LP=1, HP=1 → DASH


 Case 2: Presses straddle a frame boundary → DROPPED ✗

              Frame N poll         Frame N+1 poll
                   ↓                     ↓
   ────────────────┼─────────────────────┼──────
              LP   :         HP          :
              ↑    :         ↑           :
              T=15 :         T=18        :
        Game reads LP=1, HP=0       Game reads LP=1, HP=1
        → STRAY JAB                 → HP arrives too late
```

MvC2 has no built-in leniency for simultaneous presses — if the buttons arrive on different frames, you get the wrong move. The sync window fixes this by holding the first press until the window expires, so both buttons are always committed together.

## How the Sync Window Works

When a new button press is detected, the firmware buffers it (not yet visible in USB reports). A timer starts (5ms default). Any additional presses during the window are added to the buffer. When the window expires, all buffered presses are committed at once.

- **All inputs** (buttons + directions) go through the sync window, so direction+button combos like QCB+KK arrive together
- **Releases** are instant — no delay on letting go of buttons (charge moves work fine)
- **Slider = 0** disables the sync window entirely for raw passthrough
- Replaces stock debounce — the sync window also handles switch bounce by continuously validating buffered presses against GPIO state (`sync_new &= raw`)

## OBD Context

OBD (One Button Dash) maps a dash macro to a single button. NOBD is an alternative: you still press two buttons, but the firmware ensures they arrive together. No macros, no shortcuts — just reliable delivery of what your fingers are already doing.

## Configuration

In the GP2040-CE web UI (hold S2 on boot → `http://192.168.7.1` → Settings):

| Slider | Behavior |
|--------|----------|
| **0 ms** | Raw passthrough, no sync or debounce |
| **3-5 ms** | Recommended range. 5ms = same timing budget as stock debounce |
| **6-8 ms** | If you still get occasional drops |

Works on all platforms GP2040-CE supports (PC, Dreamcast via adapter, PS3/PS4/Switch, MiSTer, etc.) since the sync window operates at the GPIO level before any protocol-specific output.

## Install

1. Download the `.uf2` from the [Releases page](https://github.com/t3chnicallyinclined/GP2040-CE-NOBD/releases)
2. Unplug your board, hold BOOTSEL, plug in via USB
3. Copy the `.uf2` to the `RPI-RP2` drive that appears
4. Board reboots with new firmware

Built for **RP2040 Advanced Breakout Board**. To build for a different board, change the board config in `build_fw.bat` and run `.\build_fw.bat`.

## Building from Source

If you want to build the firmware yourself (e.g. for a different board), you'll need:

- Windows with MSVC Build Tools 2022
- CMake + Ninja
- ARM GCC toolchain (arm-none-eabi-gcc)
- Node.js + npm (for web UI)

### Build Steps

1. Clone the repo and open a terminal in the project root
2. Build the web UI (only needed on first build or after changing `www/`):
   ```
   cd www && npm install && npm run build && cd ..
   ```
3. Run the build script:
   ```powershell
   .\build_nobd.bat
   ```
4. UF2 files will be in the `release/` directory

### Note on mbedtls / Pico SDK Compatibility

The PS4 authentication driver uses mbedtls v3 API calls, compatible with Pico SDK 2.x. No special steps are needed — just clone, build, and go.

## Finger Gap Tester

Measure your natural finger gap with the **[Finger Gap Tester](https://github.com/t3chnicallyinclined/finger-gap-tester)** — available as a Python CLI or Rust GUI app. Detects strays, bounces, pre-fire, and recommends a NOBD sync window value.

## References

- [Why 1000Hz USB Polling Breaks Your Dashes](docs/WHY-NOBD.md) — Deep dive into USB polling, debounce vs sync, switch types, and the Dreamcast comparison
- [Dreamcast Maple Bus](https://dreamcast.wiki/Maple_bus) — 60Hz VBlank-synced polling (no intermediate reports)
- [XInputGetState](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetstate) — Snapshot-only API
- [DirectInput Buffered Mode](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416236(v=vs.85)) — Sees every state change
- [SF6 Input Polling Analysis](https://www.eventhubs.com/news/2023/jun/17/sf6-input-trouble-breakdown/) — SF6 reads inputs 3x per frame
- [GP2040-CE FAQ](https://gp2040-ce.info/faq/faq-general/) — 1000Hz USB polling, sub-1ms latency
- [Controller Input Lag](https://inputlag.science/controller/results) — Comprehensive latency data
- [MVC2 Arcade vs Dreamcast](https://archive.supercombo.gg/t/mvc2-differences-between-arcade-version-dreamcast-version/142388) — Version comparison
