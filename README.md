# GP2040-CE NOBD

## Why This Fork Exists

You know that feeling when you press two buttons at the same time and sometimes it just... doesn't work? You go for a dash in MVC2 (LP+HP) and instead of the dash, you get a stray jab. You pressed both buttons. You *know* you did. But the game only saw one of them.

**That's not a skill issue. A lot of those drops were never your fault.**

There's a timing problem baked into how button presses travel from your fingers to the game. Your controller, the USB protocol, and the game's input reading all work exactly as designed, but none of them account for the fact that your fingers aren't perfectly synchronized. The controller reports each button the instant it's pressed, USB carries those reports faithfully, and the game reads at frame boundaries that can land right in the gap between your two presses. No single piece is broken, but nobody at any layer bothered to group near-simultaneous presses together. The result is that a significant percentage of your "simultaneous" inputs are silently getting split into two separate presses. It affects every fighting game that requires pressing two or more buttons at once, on PC and console alike.

This fork of GP2040-CE adds **NOBD** (No OBD) — a tap sync window, a small buffer at the firmware level that groups near-simultaneous button presses so they always arrive together. It's a simple idea: if two buttons are pressed within a few milliseconds of each other, hold both until they can be reported at the same time. That's it.

> **Zero added latency.** Stock GP2040-CE already debounces every input at 5ms. NOBD replaces that debounce with a 5ms sync window — same latency, but now your simultaneous presses are guaranteed to arrive together instead of getting split across USB frames.

## Demo

See the difference for yourself:

**Sync disabled** — dropped dashes, stray jabs

https://github.com/user-attachments/assets/df4f4f12-4077-4e27-92e2-1057e5668e74

**Sync enabled** — dashes come out clean every time

https://github.com/user-attachments/assets/a56967f7-1b35-4f8f-9fda-de62dac0b089

**A quick note about me:** I'm a cloud engineer, not a firmware developer or electrical engineer. I came back to MVC2 after a 15-year hiatus, started playing on Steam, and immediately noticed I was dropping dashes left and right. I kept thinking "this was way more consistent on Dreamcast." I have a general understanding of how this stuff works and I knew the Dreamcast ran at 60fps with button polling synced to the frame, so I assumed the issue was that modern USB sticks poll at 1000Hz with a 1ms window instead of the Dreamcast's 16ms window. Turns out I wasn't exactly right about that, but I was close enough that it sent me down a deep research rabbit hole. For what it's worth, MVC2 is the only fighting game I play (and if anyone wants to throw hands with my Colossus, let me know), so the examples from other games below are all from research, not personal experience. But once I started digging, it became clear this isn't just a Marvel problem. Everything below is what I pieced together from datasheets, API docs, community threads, a lot of trial and error, and a healthy amount of back-and-forth with Claude AI. I may have misunderstood some things along the way, so if you spot anything wrong, feel free to correct me.

## The OBD Debate

First, let's settle something: Marvel vs Capcom 2 is the greatest fighting game of all time. Now that that's out of the way.

You already know the debate. OBD vs no OBD. One-button dash vs raw inputs. People arguing in Discord, on Twitter, in tournament lobbies. "OBD is training wheels." "No OBD is playing with a handicap." It never ends.

Here's the thing though: **the reason OBD exists is because nothing in the chain from your buttons to the game accounts for human finger timing.** Your controller reports each button the instant it's pressed, USB sends those reports faithfully, and the game reads inputs at frame boundaries. When you press PP for a dash and your fingers are 3ms apart (which is normal, that's fast), your stick sends LP to the game before HP even registers. The game sees a jab instead of a dash. That's not you being bad. That's a gap in the system that nobody ever fixed.

OBD was always a band-aid for a gap in the input chain that nobody bothered to fix. Until now.

**This firmware fix makes your simultaneous presses actually simultaneous.** No macros. No shortcuts. No single button doing the work of two. You still press two buttons to dash, you still press two buttons for supers, you still press two buttons for everything that requires two buttons. The difference is that when you press them within a few milliseconds of each other, your controller groups them together and sends them as one clean input instead of leaking a stray jab to the game.

---

## This Affects More Games Than You Think

Simultaneous button presses are everywhere in fighting games, and every single one of them is vulnerable to this:

| Game | Simultaneous Press Mechanic | What Happens When It Splits |
|------|----------------------------|---------------------------|
| **Street Fighter 6** | Throw (LP+LK), Drive Impact (HP+HK), Drive Parry (MK+MP) | You get a normal instead of a throw/DI/parry |
| **Guilty Gear Strive** | Roman Cancel (3 buttons), Burst (D+any) | You get a normal attack instead of RC/Burst |
| **Tekken 8** | Throw break (1+2), Rage Art (d/f+1+2) | You fail the throw break or get the wrong move |
| **Killer Instinct** | Combo breaker (LP+LK, MP+MK, HP+HK) | You fail the breaker and get locked out |
| **Dragon Ball FighterZ** | Super Dash (H+S), Dragon Rush (L+M) | You get a heavy or special instead of the dash/rush |
| **MVC2** | Dash (PP/KK), supers | You get a stray punch/kick instead of a dash |

Players have been running into this for years, even if they don't always know what's causing it. A [Tekken 8 Steam thread](https://steamcommunity.com/app/1778820/discussions/0/4202490036017551704/) titled "Simultaneous buttons pressing" sums it up: *"whenever I press one of the buttons just a TINY bit sooner, I mess up the combo."* A [SF6 thread](https://steamcommunity.com/app/1364780/discussions/0/5911590080724088340/) notes that *"the game has funny logic regarding simultaneous input that may cause your DI/DP slower by 1f or 2."*

Here's what I think is the most telling sign: **every modern fighting game ships with macro/shortcut buttons** for common simultaneous inputs. SF6 has shoulder button shortcuts for throws and Drive Impact. DBFZ has R1/R2. Guilty Gear has dedicated macro buttons. Why would developers build these workarounds if pressing two buttons at once worked reliably? They know it's a problem.

A [fighting game development guide](https://andrea-jens.medium.com/i-wanna-make-a-fighting-game-a-practical-guide-for-beginners-part-6-311c51ab21c4) puts it plainly: requiring exact-frame simultaneous input at 60fps gives a *"16.67ms window which is unreasonable and makes getting a valid simultaneous input impractical."* The author recommends a ~50ms buffer on the game side, but not every game does this, and no controller firmware provides one either.

There's also **[plinking](https://streetfighter.fandom.com/wiki/Plinking)** from the SF4 days, a technique the community invented specifically to work around unreliable simultaneous presses. Players figured out they could press buttons 1 frame apart and exploit the priority system to widen the input window. That's how far back this problem goes.

## Why It Happens: The Frame Boundary Problem

Here's the thing most people don't realize: when you press two buttons "at the same time," your fingers are actually a few milliseconds apart. Even with great execution, there's typically a 2-8ms gap between them. Your brain says "simultaneous" but your fingers say "almost."

That gap usually doesn't matter. But sometimes it does, because of how games read input.

Fighting games check your controller once per frame at 60fps, which means one read every ~16.67ms. If both of your presses land between the same two frame reads, the game sees them together. Simultaneous press, dash comes out. But if that tiny gap between your fingers happens to straddle a frame boundary, the game reads one button on one frame and the other button on the next frame. Two separate presses. No dash.

```
 Your fingers are 3ms apart: LP first, then HP

 Case 1: Both land within the same frame (DASH):

   Frame N poll         Frame N+1 poll
        ↓                     ↓
   ─────┼─────────────────────┼─────────
        :    LP    HP         :
        :    ↑     ↑          :
        :    T=2   T=5        :
        :                     :
        :    Both are held    :
        :    by T=16.67 ──────→ Game reads LP=1, HP=1 ✓ DASH


 Case 2: Presses straddle a frame boundary (DROPPED):

              Frame N poll         Frame N+1 poll
                   ↓                     ↓
   ────────────────┼─────────────────────┼──────
              LP   :         HP          :
              ↑    :         ↑           :
              T=15 :         T=18        :
                   :                     :
        Game reads LP=1, HP=0       Game reads LP=1, HP=1
        LP detected as new press    HP detected as new press
        HP hasn't happened yet      LP already processed ✗ NO DASH
```

The theoretical probability of a frame-boundary split is `finger_gap / 16.67ms`:

| Gap Between Your Fingers | Theoretical Chance of a Split |
|-----------|----------|
| 1ms (very tight) | ~6% |
| 2ms | ~12% |
| 3ms | ~18% |
| 4ms | ~24% |
| 5ms | ~30% |
| 8ms (loose timing) | ~48% |

**Big caveat:** these are theoretical worst-case numbers. The real drop rate depends on how the specific game detects simultaneous presses, whether it has a built-in leniency window, and how tight your execution is. Real-world rates are probably lower.

But the problem is real. You've felt it. Those random dashes that just don't come out even though you pressed both buttons. That's a frame boundary split. It happens more under pressure (stress makes your finger timing less precise). There's no reason to leave it to chance when the controller could just group near-simultaneous presses together.

## What 1000Hz USB Adds to the Problem

The frame-boundary issue exists on any platform. But modern USB controllers make it worse by creating **intermediate states** where only one of your two buttons is visible.

A 1000Hz USB fight stick sends a button state snapshot every 1ms (~16 reports per game frame). When your fingers are 3ms apart, the controller sends 3 reports showing only the first button before the second arrives:

```
USB HID reports (1ms apart):

  T=0ms: LP=0, HP=0
  T=1ms: LP=1, HP=0  ← LP pressed, HP not yet
  T=2ms: LP=1, HP=0  ← still LP-only
  T=3ms: LP=1, HP=0  ← still LP-only
  T=4ms: LP=1, HP=1  ← HP finally arrives
```

How much this matters depends on how the game reads input:

- **XInput** (most common): The game only sees the latest report each frame. The intermediate LP-only reports usually don't matter, unless the game's frame poll lands during the gap, catching LP without HP.
- **DirectInput buffered mode**: The game receives every HID report as a separate event, so it sees LP-only followed by LP+HP as distinct state changes ([Microsoft docs](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416236(v=vs.85))).
- **Street Fighter 6**: Reads inputs **3 times per frame** (~every 5.5ms), tripling the chances of catching an intermediate state ([WydD/EventHubs analysis](https://www.eventhubs.com/news/2023/jun/17/sf6-input-trouble-breakdown/)).
- **Steam Input**: Intercepts HID reports and re-processes them before presenting to the game ([Valve docs](https://partner.steamgames.com/doc/features/steam_controller/steam_input_gamepad_emulation_bestpractices)). The internal processing pipeline can introduce additional intermediate-state visibility.
- **MiSTer/emulators**: FPGA cores and emulators may consume USB reports directly, seeing every intermediate state ([MiSTer Addons analysis](https://misteraddons.com/pages/latency)).

Worth noting: on the original arcade hardware (Naomi/JVS) and Dreamcast (Maple Bus), this wasn't an issue. Those systems read all button states once per frame with no intermediate reports. The problem is specific to how modern USB controllers report at high polling rates.

## As Far As I Can Tell, Nobody Has Fixed This

I looked into whether any existing controller or firmware handles this. Every major fight stick manufacturer (Brook, Victrix, Razer, Hori, Hitbox) implements **SOCD cleaning** for conflicting directional inputs. But I couldn't find any that offer attack button synchronization.

Some games try to handle it on their end, but the leniency varies:
- **Skullgirls**: [3-frame leniency (~50ms)](https://steamcommunity.com/app/245170/discussions/0/616198900634630066/), very generous, rarely drops
- **Tekken 8**: [*"almost frame perfect"*](https://steamcommunity.com/app/1778820/discussions/0/4202490036017551704/), extremely strict, drops often
- **SF6**: Ships macro buttons as a workaround
- **Most games**: No documented leniency at all

You can't rely on the game to fix this. It depends entirely on which game you're playing and whether the developer thought about it. A controller-level solution works the same everywhere.

## What This Fork Does: The Sync Window

The idea is simple: instead of reporting each button the instant it's pressed, wait a tiny bit. If another button arrives within that window, report them together. If not, report the single press after the window expires.

The sync window operates at the **controller firmware level**, before any USB report is sent. When a new button press is detected, the firmware **buffers it**. The press is not included in USB HID reports. A short timer starts (5ms by default). Any additional presses during that window are added to the buffer. When the window expires, **all buffered presses are committed at once**:

```
Without sync window (standard 1000Hz):

  T=0:  LP=0, HP=0  (report sent)
  T=1:  LP=1, HP=0  (report sent, LP visible alone)
  T=2:  LP=1, HP=0  (report sent, still LP alone)
  T=3:  LP=1, HP=0  (report sent, still LP alone)
  T=4:  LP=1, HP=1  (report sent, HP finally arrives)
        ↑
        3ms window where LP-only state is visible to the game


With sync window (5ms):

  T=0:  LP=0, HP=0  (report sent)
  T=1:  LP=0, HP=0  (report sent, LP buffered, NOT reported)
  T=2:  LP=0, HP=0  (report sent, still buffered)
  T=3:  LP=0, HP=0  (report sent, still buffered)
  T=4:  LP=0, HP=0  (report sent, HP added to buffer)
  T=5:  LP=1, HP=1  (report sent, window expired, both committed)
        ↑
        NO intermediate state - game NEVER sees LP without HP
```

The game either sees **nothing** (during the buffer window) or **both buttons together** (after commit). There is no point in time where a partial press is visible in the USB reports, regardless of how the game reads input (XInput, DirectInput, SF6's triple polling, MiSTer, etc.).

The trade-off is a small amount of added latency on the **initial** press (the first button waits for the window to expire). But keep in mind, stock GP2040-CE already has a 5ms debounce delay on every press. Since the sync window replaces that debounce, the **net increase is zero** at the default 5ms setting — you're getting the same latency as stock, but with guaranteed simultaneous delivery. Most people's natural finger gap is 2-5ms, so the default covers virtually all presses. The upside is that near-simultaneous presses are **guaranteed** to appear together. No more coin flip. No more dropped dashes because your fingers were 3ms apart and a frame boundary happened to land in between.

Critically, this **only affects new presses** (buttons and directions):
- **All inputs** (buttons + directions) go through the sync window so that direction+button combos (like QCB+KK for fly cancel) arrive together. Stock GP2040-CE already debounced all inputs at 5ms, so this is equivalent latency.
- **Releases** (letting go of a button): Instant, zero delay. Charge moves (Megaman buster, Sentinel drones, etc.) work perfectly
- **Holds** (keeping a button down): Completely unaffected. Once committed, a button stays held indefinitely
- **Config mode**: Bypassed entirely so the web UI always works
- **Slider = 0**: Disables the sync window for raw 1000Hz passthrough

## Works On All Platforms

The sync window operates at the GPIO level inside the firmware, before any console-specific output protocol. It works the same whether you're playing on:
- PC (Steam, emulators)
- Dreamcast (via adapter)
- PS3/PS4/Switch
- MiSTer
- Any other platform GP2040-CE supports

## Configuration

In the GP2040-CE web UI (hold S2 on boot, navigate to `http://192.168.7.1`):
- Go to **Settings**
- Set **NOBD** slider:
  - **0 ms** = raw passthrough, no sync (default 1000Hz behavior)
  - **5 ms** = default, recommended for most players (covers 2-5ms natural finger gap, same latency as stock debounce)
  - **6-8 ms** = if you still get occasional dropped simultaneous presses
  - Keep it as low as works for you. Higher values add latency to initial presses

## Why It Replaces the Debounce (and Why I Think That's Fine)

Again, I'm not a firmware engineer or an electrical engineer. This is just my understanding from digging through the code and reading about how switches work. My tech background helped me connect the dots, but I could be wrong about some of this. Take it for what it is.

This fork replaces GP2040-CE's debounce logic with the sync window. They live in the same function (`debounceGpioGetAll()`), and stacking both would mean two delay stages on every press for no real benefit. So one replaces the other.

The obvious question: doesn't removing debounce cause problems?

Quick background on what debounce does. When a mechanical switch closes, the metal contacts physically bounce against each other for a brief period (generally under 5ms for standard microswitches). That bounce creates rapid on/off signals. Without filtering, the controller could read those bounces as separate presses. Traditional debounce waits for the signal to stabilize before registering the input.

**For attack buttons**, the sync window handles this through one line in the code:

```
sync_new &= raw;
```

Every cycle, the firmware checks that every buffered press is still physically held on the GPIO pin. If a button bounces open mid-buffer, that line drops it. When the contact settles back closed, it gets re-captured. By the time the 5ms window expires, the bounce has long settled and only the stable press survives to be committed. The bounce noise gets continuously cleaned out. Release-side bounce is handled the same way: phantom re-presses enter the buffer but get cleaned out by `sync_new &= raw` before the window expires.

So the sync window handles bounce not because "5ms is longer than bounce" (that's an oversimplification). It handles it because the code continuously validates buffered presses against the actual physical state of the buttons, every single cycle.

**All inputs** (buttons and directions) go through the sync window. This ensures that direction+button combos (like QCB+KK for fly cancel) arrive together. Stock GP2040-CE already debounced all inputs at 5ms, so this is the same latency — just smarter about grouping simultaneous presses.

A few caveats:
- **Slider at 0** means raw passthrough with no sync AND no debounce. No filtering at all.
- **Optical and hall effect switches** don't bounce (no physical contacts), so debounce was already doing nothing for those.
- This is all based on my research and reading the code. I'm not an expert, so if something feels off, stock GP2040-CE firmware is always there to flash back.

## Files Changed

### `src/gp2040.cpp` - Core sync logic
Replaced the `debounceGpioGetAll()` function with the sync window implementation:
1. Passes directional inputs (up/down/left/right) straight through with zero delay
2. Detects truly new attack button presses (`buttonRaw & ~prev & ~sync_new`)
3. Passes releases through instantly (`gamepad->debouncedGpio &= ~just_released`)
4. Accumulates new attack presses into a sync buffer (`sync_new |= just_pressed`)
5. Commits all buffered presses when the window expires (`gamepad->debouncedGpio |= sync_new`)

### `www/src/Locales/en/SettingsPage.jsx` - UI label
Renamed "Debounce Delay in milliseconds" to "NOBD Sync Window in milliseconds" in the web UI settings page.

### `build_fw.ps1` - Build script
PowerShell build script for Windows. Sets up the MSVC environment, overrides the `GP2040_BOARDCONFIG` env var (which otherwise overrides CMake `-D` flags), and builds with Ninja.

## Building

```powershell
# From the GP2040-CE directory
powershell -ExecutionPolicy Bypass -File build_fw.ps1
```

Output: `build/GP2040-CE_0.7.12_RP2040AdvancedBreakoutBoard.uf2`

## Flashing

1. Unplug the board
2. Hold BOOTSEL and plug in via USB
3. Copy the `.uf2` file to the `RPI-RP2` drive that appears
4. Board auto-reboots with new firmware

A pre-built `.uf2` is available on the [Releases page](https://github.com/t3chnicallyinclined/GP2040-CE-NOBD/releases).

## Board

Built for **RP2040 Advanced Breakout Board**. To build for a different board, change `RP2040AdvancedBreakoutBoard` in `build_fw.ps1` to your board's config directory name under `configs/`.

## References

### Hardware & Protocol
- [Dreamcast Maple Bus Wiki](https://dreamcast.wiki/Maple_bus) - Maple Bus protocol details, 60Hz VBlank-synced polling
- [Maple Bus Wire Protocol](http://mc.pp.se/dc/maplewire.html) - Low-level timing and signaling
- [Sega Naomi System Overview](https://segaretro.org/Sega_NAOMI) - Naomi arcade hardware (MVC2's original platform)
- [JVS (JAMMA Video Standard) Protocol](https://segaretro.org/JVS) - Arcade I/O bus used for input reading

### Input APIs & Polling
- [XInputGetState - Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetstate) - Snapshot-only API, intermediate states lost
- [DirectInput Buffered vs Immediate - Microsoft](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416236(v=vs.85)) - Buffered mode sees every state change
- [GameInput Fundamentals - Microsoft](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-fundamentals) - Explicitly cites fighting games needing full input history
- [Steam Input Gamepad Emulation - Valve](https://partner.steamgames.com/doc/features/steam_controller/steam_input_gamepad_emulation_bestpractices) - How Steam hooks input APIs

### Fighting Game Analysis
- [SF6 Input Polling Analysis - WydD/EventHubs](https://www.eventhubs.com/news/2023/jun/17/sf6-input-trouble-breakdown/) - SF6 reads inputs 3x per frame
- [Fighting Game Input Systems - Andrea Jens](https://andrea-jens.medium.com/i-wanna-make-a-fighting-game-a-practical-guide-for-beginners-part-6-311c51ab21c4) - Simultaneous input at 60fps is "unreasonable"
- [Plinking - Street Fighter Wiki](https://streetfighter.fandom.com/wiki/Plinking) - SF4 workaround for unreliable simultaneous presses
- [Combo Breakers - Infil's KI Guide](https://ki.infil.net/cbreaker.html) - KI requires simultaneous strength-matched presses

### Controller Latency & Testing
- [GP2040-CE FAQ](https://gp2040-ce.info/faq/faq-general/) - 1000Hz USB polling, sub-1ms latency
- [Controller Input Lag - inputlag.science](https://inputlag.science/controller/results) - Comprehensive controller latency data
- [USB Input Latency - MiSTer Addons](https://misteraddons.com/pages/latency) - USB polling vs frame sync analysis
- [Polling Rate & Input Lag Guide](https://gamepadtest.app/guides/polling-rate-overclocking) - Modern controller polling explained

### Community Discussion
- [Tekken 8 - "Simultaneous buttons pressing"](https://steamcommunity.com/app/1778820/discussions/0/4202490036017551704/) - Players reporting simultaneous press failures
- [SF6 - Drive Impact input issues](https://steamcommunity.com/app/1364780/discussions/0/5911590080724088340/) - Simultaneous input timing problems
- [Skullgirls - Simultaneous press leniency](https://steamcommunity.com/app/245170/discussions/0/616198900634630066/) - 3-frame leniency window discussion
- [MVC2 Arcade vs Dreamcast](https://archive.supercombo.gg/t/mvc2-differences-between-arcade-version-dreamcast-version/142388) - Version comparison discussion
