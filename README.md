# GP2040-CE NOBD

[![Sponsor on GitHub](https://img.shields.io/badge/Sponsor-GitHub-ea4aaa?logo=github)](https://github.com/sponsors/t3chnicallyinclined)
[![Support on Ko-fi](https://img.shields.io/badge/Support-Ko--fi-FF5E5B?logo=ko-fi)](https://ko-fi.com/trisdog)
[![Donate via Strike](https://img.shields.io/badge/Donate-Strike-7B68EE?logo=bitcoin)](https://strike.me/nobd)

A fork of [GP2040-CE](https://gp2040-ce.info/) v0.7.12 that adds **NOBD (No OBD)** — a sync window that groups near-simultaneous button presses so they arrive on the same USB frame. Built for MvC2, where dropped dashes from split LP+HP presses are a constant problem.

> **Minimal latency, maximum reliability.** Stock debounce accepts each press instantly but can't group them — your "simultaneous" buttons arrive on different USB frames. NOBD holds presses for up to 5ms so they arrive together. That's less than a third of one game frame (16.67ms), and the same timing budget stock debounce already uses for bounce filtering.

## Demo (MvC2)

**Sync disabled** — dropped dashes, stray jabs

https://github.com/user-attachments/assets/df4f4f12-4077-4e27-92e2-1057e5668e74

**Sync enabled** — dashes come out clean every time

https://github.com/user-attachments/assets/a56967f7-1b35-4f8f-9fda-de62dac0b089

## About

I'm a cloud engineer, not a firmware dev. I came back to MVC2 after a 15-year hiatus, started playing on Steam, and immediately noticed I was dropping dashes constantly. That sent me down a rabbit hole. **MVC2 is the only fighting game I play and the only game I've tested this with.** The sync window may help other games that require simultaneous button presses, but many modern fighting games have their own input leniency systems that handle this at the software level. I can't make claims about games I haven't tested.

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

MvC2 has no built-in leniency for simultaneous presses — its input model is [arcade-era strict](https://www.hitboxarcade.com/blogs/hit-box/magnetro-presents-mvc2-magneto-tech). If the buttons arrive on different frames, you get the wrong move. Players are [already reporting this](https://steamcommunity.com/app/2634890/discussions/0/4755326933235585026/) in the MVC Fighting Collection on PC — inconsistent dashes, fast inputs failing — problems that modern fighting games with built-in leniency don't have. The sync window fixes this by holding the first press until the window expires, so both buttons are committed together.

## How the Sync Window Works

When a new button press is detected, the firmware buffers it (not yet visible in USB reports). A timer starts (5ms default). Any additional presses during the window are added to the buffer. When the window expires, all buffered presses are committed at once.

- **All inputs** (buttons + directions) go through the sync window, so direction+button combos like QCB+KK arrive together
- **Releases** are instant — no delay on letting go of buttons (charge moves work fine)
- **Slider = 0** disables the sync window entirely for raw passthrough
- Replaces stock debounce — the sync window also handles switch bounce by continuously validating buffered presses against GPIO state (`sync_new &= raw`)

## "Why Not Just Lower the Polling Rate?"

If the Dreamcast polled at 60Hz and it worked, why not just set USB to 60Hz? Because frequency isn't the problem — **synchronization** is. On Dreamcast, controller reads, game logic, and rendering were all driven by the same VBlank interrupt. On PC, USB polling and the game loop run on completely independent schedules with no shared timing signal. Lowering USB to 60Hz changes the *rate* of frame boundary mismatches but doesn't eliminate them — and you lose 1000Hz responsiveness for everything else. See [the full technical explanation](docs/WHY-NOBD.md#why-not-just-lower-the-usb-polling-rate) for details.

## OBD vs NOBD — What's the Difference?

**OBD (One Button Dash)** maps a dash macro to a single button. You press one button, the firmware sends two. That's a macro — one user action producing multiple outputs that the player didn't physically perform.

**NOBD (No OBD)** is not a macro. You press two buttons, the firmware sends two buttons. Nothing is added, removed, or invented. The sync window just prevents a 1ms USB polling boundary from splitting your two real presses across two frames.

| | OBD | NOBD |
|---|---|---|
| **Buttons you press** | 1 | 2 |
| **Buttons the game sees** | 2 | 2 |
| **Inputs invented by firmware** | Yes (1 extra) | No |
| **Is it a macro?** | Yes | No |
| **Skill requirement** | None — one button = dash | Full — you still need to hit both buttons within the sync window |
| **What it fixes** | Execution barrier | Hardware timing artifact |

A **macro** is any automated sequence where a single user action produces outputs that differ from what was physically performed — the device *invents* inputs the player didn't make. By that definition:

- **OBD is a macro.** One press → two buttons. The firmware added an input.
- **NOBD is not a macro.** Two presses → two buttons. The firmware just made sure they arrived together.

Think of it like a keyboard that occasionally drops the second letter when you type "th" fast. A fix that makes the keyboard report both keys isn't "auto-typing" — it's just a keyboard that works. OBD would be pressing "t" and having the keyboard type "th" for you.

## FAQ — Addressing the Pushback

### "This is cheating."

No. Cheating gives you an ability you wouldn't otherwise have. NOBD gives you the ability you *already had* on the original hardware.

On Dreamcast and arcade CPS2 hardware, the controller and game shared a synchronized clock. The game read inputs once per frame (~16.67ms), and any two presses within that window were naturally grouped. That's how the game was designed — simultaneous meant "same frame," not "same microsecond."

USB on PC broke that. 1000Hz polling and asynchronous clocks expose sub-frame timing the game was never designed to see. Your "simultaneous" presses get split across USB polls by a timing boundary that didn't exist on the original hardware. NOBD restores the behavior the game was built around — and does it with a **stricter** window (5ms vs the original 16.67ms).

If anything, NOBD is *harder* than original hardware. Your presses need to be closer together than the arcade ever required.

### "This is a macro."

A macro is one input in, multiple inputs out. NOBD is two inputs in, two inputs out. Nothing is added. Nothing is invented. The firmware delivers exactly what your fingers did — it just makes sure both presses land on the same USB frame instead of being randomly split by a polling boundary.

By the actual definition of a macro — an automated sequence where a single user action produces outputs the player didn't physically perform — NOBD doesn't qualify. You pressed two buttons. The game saw two buttons. That's it.

### "You're just making dashing easier."

If NOBD made dashing easier, you could mash one button and get dashes. You can't. You still have to press two buttons, with the right timing, with the right spacing. If your fingers are more than 5ms apart, the sync window won't help you — your inputs won't group.

NOBD doesn't lower the execution barrier. It removes a **hardware lottery** where identical inputs sometimes work and sometimes don't based on where they land relative to a 1ms USB polling boundary. The skill requirement is the same. The randomness is gone.

### "Just get good. Practice more."

This isn't an execution problem — it's a hardware timing problem. You can have perfect execution and still drop dashes because your two presses straddled a USB frame boundary. The same input, the same timing, the same muscle memory — sometimes it works, sometimes it doesn't, depending on *when* in the 1ms polling cycle your fingers happen to land.

No amount of practice fixes a random timing boundary you can't see or control. Players who hit this on the [MVC Fighting Collection on Steam](https://steamcommunity.com/app/2634890/discussions/0/4755326933235585026/) aren't bad — they're experiencing a hardware mismatch that didn't exist on the original platform.

### "No one else has this problem."

They do. Players are [independently reporting it](https://steamcommunity.com/app/2634890/discussions/0/4755326933235585026/) in the MVC Fighting Collection on Steam — inconsistent dashes, fast inputs failing, inputs that work on one attempt and not the next. It's been a [long-standing community observation](https://archive.supercombo.gg/t/you-think-mvc2-is-hard-to-play-on-pad/133861) that MvC2 simultaneous presses are unreliable.

The reason most people don't notice in *other* fighting games is that **modern games already solved this problem at the software level.** Street Fighter 6 [reads inputs 3x per frame](https://www.eventhubs.com/news/2023/jun/17/sf6-input-trouble-breakdown/) and has multi-frame leniency windows. Guilty Gear Strive, Tekken 8, and most modern fighters have similar built-in leniency for simultaneous presses. MvC2 doesn't — it's an arcade-era game with [SF2-style input strictness](https://www.hitboxarcade.com/blogs/hit-box/magnetro-presents-mvc2-magneto-tech) running on hardware it was never designed for. We [tested this empirically](docs/WHY-NOBD.md#testing-it-how-often-does-mvc2-read-input-per-frame) — a 1ms button pulse was caught only 5% of the time, consistent with a single input read per frame and no buffering.

### "This is only a problem because you're bad at the game."

The same player, with the same stick, pressing the same buttons with the same timing, will get different results depending on:
- Whether the USB host polled between their two presses (1ms boundary — invisible, uncontrollable)
- Whether the game's frame boundary fell between the two USB reports (asynchronous clocks — invisible, uncontrollable)

This is **not** a skill variable. It's a coin flip determined by two clocks the player can't see or influence. On the original Dreamcast hardware, this coin flip didn't exist because the clocks were synchronized. NOBD eliminates the coin flip. Your execution determines the outcome — not timing luck.

### "This only affects PC/Steam, so who cares?"

Exactly — **this is a PC-specific problem**, which is precisely why it needs a PC-specific fix. MvC2 was designed for arcade hardware (CPS2) and ported to Dreamcast, both of which had synchronized input polling. The PC port via Marvel vs. Capcom Fighting Collection inherits the original game's zero-leniency input model but runs it on asynchronous USB hardware that exposes timing gaps the game was never designed to handle.

Console players on the Fighting Collection may also experience this depending on the platform's USB implementation, but the problem is most pronounced on PC where 1000Hz polling is standard and there's no platform-level input synchronization.

### "If it was a real problem, Capcom would have fixed it."

MvC2's code in the Fighting Collection is largely an emulated/ported version of a 25-year-old game. Adding input leniency would mean modifying the original game logic — something Capcom has historically avoided in re-releases to preserve arcade accuracy. Modern Capcom games (SF6) *do* solve this with multi-frame input reads, which proves they're aware of the problem. They just aren't retrofitting it into legacy titles.

### "Every other player deals with it, why can't you?"

Every other player *is* dealing with it — they just may not know why their dashes drop. Some players compensate by switching to OBD (a literal macro). Some use pad where the shorter finger travel reduces the gap. Some play on platforms with lower polling rates where the problem is less frequent. And some just accept the inconsistency as "MvC2 being MvC2" without realizing it's a hardware issue that didn't exist on the original platform.

NOBD is the only solution that fixes the actual problem without adding macros, removing inputs, or changing platforms.

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

- [Why 1000Hz USB Polling Breaks Your Dashes](docs/WHY-NOBD.md) — Deep dive into USB polling, debounce vs sync, switch types, the Dreamcast comparison, and why lowering polling rate doesn't work
- [MVC Fighting Collection — Steam Community Reports](https://steamcommunity.com/app/2634890/discussions/0/4755326933235585026/) — Players independently reporting the exact problem NOBD addresses
- [Magnetro MvC2 Magneto Tech](https://www.hitboxarcade.com/blogs/hit-box/magnetro-presents-mvc2-magneto-tech) — MvC2's SF2-style input leniency model
- [Dreamcast Maple Bus](https://dreamcast.wiki/Maple_bus) — 60Hz VBlank-synced polling (no intermediate reports)
- [KallistiOS maple.h](http://gamedev.allusion.net/docs/kos-current/maple_8h_source.html) — VBlank-driven Maple DMA source code
- [XInputGetState](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetstate) — Snapshot-only API
- [SF6 Input Polling Analysis](https://www.eventhubs.com/news/2023/jun/17/sf6-input-trouble-breakdown/) — SF6 reads inputs 3x per frame
- [GP2040-CE FAQ](https://gp2040-ce.info/faq/faq-general/) — 1000Hz USB polling, sub-1ms latency
- [Controller Input Lag](https://inputlag.science/controller/results) — Comprehensive latency data
- [MVC2 Arcade vs Dreamcast](https://archive.supercombo.gg/t/mvc2-differences-between-arcade-version-dreamcast-version/142388) — Version comparison
