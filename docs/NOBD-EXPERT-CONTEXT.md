# NOBD Expert Context — Fighting Games, Game Dev, and Firmware

This file provides deep domain knowledge across the three disciplines that intersect in NOBD. Load this when working on the project to make informed decisions about input handling, game engine timing, and embedded firmware behavior.

---

## Part 1: Fighting Game Expert

### The Input Problem in Fighting Games

Fighting games are unique among competitive games because they require **simultaneous multi-button inputs** with frame-perfect timing. No other genre demands this at the same intensity.

#### Common Simultaneous Inputs
| Input | Game(s) | Buttons | What Happens If Split |
|-------|---------|---------|----------------------|
| Dash (LP+HP) | MvC2, DBFZ | 2 attack buttons | Stray jab (LP comes out alone) |
| Throw tech (LP+LK) | SF6, Tekken | 2 buttons | Jab or short instead of tech |
| Roman Cancel | Guilty Gear Strive | 3 buttons | Wrong RC type or nothing |
| V-Trigger / Drive Impact | SF5/SF6 | HP+HK | Heavy normal instead of special |
| Super activation | Many games | Multiple buttons | Wrong move or nothing |
| Fly cancel (QCB+KK) | MvC2 | Direction + 2 kicks | HK comes out, fly doesn't activate |
| Throw (LP+LK) | Street Fighter | 2 buttons | Jab instead of throw |
| Tag / Assist | MvC2, DBFZ | Specific combos | Wrong character action |

#### Frame Data Context
- Fighting games typically run at **60fps** (16.67ms per frame)
- Input is read **once per frame** (some games like SF6 read 3x per frame for sub-frame detection)
- A **1-frame link** in SF4 had a 16.67ms window — considered extremely difficult
- Modern games use input buffers (3-5 frames) for special moves but NOT for simultaneous presses
- Simultaneous press detection has **zero buffer** in most games — both buttons must be in the same input read

#### The OBD Debate
**OBD (One Button Dash)** maps a dash macro to a single button. The FGC is split:
- **Pro-OBD:** "It's just quality of life, execution shouldn't be a barrier"
- **Anti-OBD:** "It's a macro, fundamentally changes the risk/reward of dashing"
- **Tournament scene:** Most majors allow leverless controllers but ban macros. OBD lives in a gray area since it's firmware-level, not a software macro.

**NOBD's position:** Make simultaneous presses reliable without macros. Same execution requirement (press two buttons), just guaranteed delivery on the same frame.

#### Platform-Specific Input Behavior

**PC (USB HID):**
- USB polls at 1000Hz (1ms interval) by default
- DirectInput, XInput, and HID all poll at the USB endpoint rate
- Steam Input adds its own pipeline processing layer
- Most fighting games on PC use 1ms polling
- Frame boundary issue: USB poll and game frame are not synchronized

**PlayStation 4/5:**
- PS4: USB polls at 1000Hz for wired, Bluetooth at 250Hz
- PS5: USB polls at 1000Hz
- Authentication required — controllers need licensed chips or passthrough
- Input latency varies by game (SF6 is ~4 frames, Tekken 8 is ~3-4 frames native)

**Xbox Series X/S:**
- USB polling at 1000Hz (wired)
- GameInput API replaced XInput for next-gen
- Latency similar to PS5

**Nintendo Switch:**
- USB polling at 125Hz (8ms) by default, some games support 1000Hz
- Pro Controller Bluetooth: ~15ms latency
- Higher input latency than other platforms generally

**Dreamcast (Maple Bus):**
- Frame-synced polling at 60Hz (16.67ms per poll)
- **No sub-frame detection** — input is read exactly once per frame
- MvC2 on Dreamcast: if your two presses straddle a 16.67ms boundary, guaranteed split frame
- This is where the problem is MOST severe and NOBD helps the most

**MiSTer FPGA:**
- USB polling through the Linux kernel (varies by implementation)
- SNAC (direct controller) has native console-accurate timing
- Community-measured latency: 1-2 frames depending on core and connection

#### What "Plinking" Was
In SF4, players discovered **priority linking (plinking)** — pressing a higher-priority button 1 frame before the desired button. The game's priority system would register both on the same frame due to input overlap. This was a **community workaround for the same frame boundary problem** NOBD solves at the firmware level.

SF5 added a 2-frame input buffer for links specifically because plinking was considered an execution barrier. NOBD takes the same philosophy and applies it to simultaneous presses.

---

## Part 2: Game Dev Expert — Input Systems

### How Fighting Games Process Input

```
Physical Press → Controller Firmware → USB Report → OS Driver → Game Input Layer → Game Logic
   ~0ms           ~1-5ms debounce       ~1ms         ~0-1ms       ~0-16ms (frame)    reads here
```

#### The Frame Boundary Problem (detailed)

Game engines typically process input at the start of each frame:

```
Frame N                              Frame N+1
|-------- 16.67ms ---------|-------- 16.67ms ---------|
     ^                          ^
     Game reads input            Game reads input

USB polls: |--1ms--|--1ms--|--1ms--|--1ms--|--1ms--|--1ms--|

Scenario: Player presses LP at T=15ms, HP at T=17ms
  - Frame N reads: sees LP only (HP hasn't arrived yet)
  - Frame N+1 reads: sees LP + HP
  - Game registered LP on frame N → JAB comes out, not dash
```

This is **probabilistic** — the same physical action (pressing two buttons 2ms apart) sometimes works and sometimes doesn't, depending on where the presses land relative to the frame boundary. This randomness is what frustrates players.

#### How NOBD Fixes It

```
Frame N                              Frame N+1
|-------- 16.67ms ---------|-------- 16.67ms ---------|
     ^                          ^
     Game reads input            Game reads input

Player presses LP at T=15ms, HP at T=17ms
NOBD (5ms window): holds LP, waits until T=20ms, commits LP+HP together
  - Frame N reads: nothing yet (NOBD still buffering)
  - Frame N+1 reads: LP + HP (both committed together at T=20ms)
  - Game sees simultaneous press → DASH
```

The sync window shifts WHEN the buttons appear to the game, but both always appear TOGETHER.

#### Sub-Frame Input Reading
Some modern games read input multiple times per frame:
- **SF6:** Reads input 3 times per 60fps frame (~5.5ms intervals)
- **Tekken 8:** Single read per frame but with larger input buffer
- **Guilty Gear Strive:** Single read, 3-frame buffer for specials

Even with sub-frame reading, simultaneous press detection requires both buttons to be present in the SAME read. NOBD ensures they are.

#### Input Priority Systems
Most fighting games have button priority:
```
HP > MP > LP > HK > MK > LK (typical Street Fighter priority)
```
When two buttons are pressed on the same frame, priority determines which wins if they conflict. When they're on different frames, priority is irrelevant — the first button registers its move before the second can contest.

#### Negative Edge
Some games register button RELEASES as inputs (negative edge). This is why NOBD releases are instant — delaying a release would add latency to:
- Charge moves (hold back, release forward + button)
- Negative edge special cancels
- Piano input techniques

### Rollback Netcode Considerations
Modern fighting games use rollback netcode. The game predicts input and rolls back if wrong. Added input latency from NOBD (when > stock debounce) could theoretically increase the rollback window, but at 5ms (same as stock) there's zero impact. Even at 8ms, the 3ms difference is far below the typical rollback window (3-8 frames = 50-133ms).

---

## Part 3: Firmware Expert — RP2040 and GP2040-CE

### RP2040 Hardware

**Microcontroller specs:**
- Dual ARM Cortex-M0+ cores at 133MHz
- 264KB SRAM, 2MB flash (typical board)
- 30 GPIO pins (directly mapped to physical buttons/directions)
- Native USB 1.1 controller (12Mbps)
- PIO (Programmable I/O) state machines — can implement USB host

**GPIO and Button Reading:**
```c
// Raw GPIO reading — inverted because buttons pull low when pressed
Mask_t raw_gpio = ~gpio_get_all();  // Each bit = one button/direction
```
- GPIO reads are **instantaneous** (single cycle, <10ns)
- No ADC needed for digital buttons
- Physical switch bounce typically settles in 1-5ms

### GP2040-CE Firmware Architecture

**Main loop (`gp2040.cpp`):**
```
setup() → configures pins, USB, drivers
run() → infinite loop:
  1. syncGpioGetAll() OR debounceGpioGetAll()  ← mutually exclusive
  2. gamepad->read()
  3. PreprocessAddons()
  4. gamepad->process() (SOCD cleaning)
  5. ProcessAddons()
  6. Send USB HID report
```

**Two separate functions (mutually exclusive):**
- `debounceGpioGetAll()` — stock GP2040-CE per-pin debounce (runs when `nobdSyncDelay == 0`)
- `syncGpioGetAll()` — NOBD sync window with instant fire (runs when `nobdSyncDelay > 0`)

**Input pipeline (NOBD mode):**
```
GPIO pins → syncGpioGetAll() → gamepad->debouncedGpio → SOCD clean → USB HID report
                  ↑
     sync window + instant fire
```

**Input pipeline (Stock mode):**
```
GPIO pins → debounceGpioGetAll() → gamepad->debouncedGpio → SOCD clean → USB HID report
                  ↑
         per-pin debounce timer
```

The sync/debounce is the **first** processing step. Everything downstream (SOCD cleaning, turbo, macros, etc.) sees already-processed inputs.

### Instant Fire

When the sync window detects 2+ attack buttons (B1-B4, L1-L2, R1-R2) in `sync_new`, it commits **everything** immediately — buttons AND directions. This covers:
- Dashes (LP+HP = 2 buttons)
- Triple supers (3 buttons)
- Forward + LP + HP (direction committed with the buttons)

Detection uses `attacks & (attacks - 1)` — a bit trick that is non-zero when 2+ bits are set. Single CPU cycle on Cortex-M0+.

### WebConfig Pipeline

Settings flow through this chain:
```
config.proto → config_utils.cpp → webconfig.cpp → SettingsPage.jsx
  (schema)      (defaults)         (read/write)     (UI controls)
```

- `nobdSyncDelay` = field 34 in `GamepadOptions` (proto)
- `nobdReleaseDebounce` = field 35 in `GamepadOptions` (proto) — boolean, opt-in release debounce
- `DEFAULT_NOBD_SYNC_DELAY = 5` (config_utils.cpp)
- Web UI presents a mode dropdown (Stock Debounce / NOBD Sync Window) + value field + Release Debounce checkbox

### Switch Bounce

Mechanical switches don't make clean contact. When pressed, the contact bounces:
```
Physical press:
  GPIO: 1 0 1 0 1 1 1 1 1 1 1 1 1  (bounces for ~1-3ms, then stable)

Physical release:
  GPIO: 0 1 0 1 0 0 0 0 0 0 0 0 0  (bounces for ~1-3ms, then stable)
```

**Stock GP2040-CE debounce:** Waits 5ms after first state change before accepting it. Simple but effective.

**NOBD bounce handling:**

**In-window filter:** `sync_new &= raw_buttons` every cycle. If a switch bounces OFF during the window, that bit gets cleared from the pending buffer. When it bounces back ON, it's re-detected as a new press. By the time the window expires and commits, the switch has settled. This is **continuous validation** — more robust than a simple timer.

All presses wait the full sync window. This naturally handles transitional contacts during wavedashing: if a finger brushes a button while the window is still open from a multi-button press, the contact gets accumulated into the existing window rather than creating a separate stray.

**Note:** Release-bounce lockout (per-pin timestamps preventing re-presses within N ms of release) was tried and reverted — it interfered with legitimate fast re-presses and didn't fully solve transitional contacts.

**Release debounce (opt-in):** A different approach from lockout. When `nobdReleaseDebounce` is enabled, releases are buffered into `pending_release` and only committed after the sync delay window. If the switch bounces back ON during the window (`pending_release &= ~raw_buttons`), the pending release is cancelled — the button appears to have never released. This avoids the lockout problem because it doesn't block re-presses; instead it makes the bounce invisible. Designed for rhythm games (Guitar Hero, Cadence of Hyrule) where release bounce causes phantom inputs. Off by default to preserve instant releases for fighting games (negative edge, charge partitioning).

### USB HID Reports

GP2040-CE sends HID reports to the host:
```c
typedef struct {
    uint16_t buttons;    // Bitmask: each bit = one button
    uint8_t hat;         // D-pad (hat switch)
    int16_t lx, ly;      // Left stick
    int16_t rx, ry;      // Right stick
    // ... varies by protocol (XInput, DInput, Switch, PS4)
} GamepadReport;
```

The `buttons` field is a bitmask. When NOBD commits `sync_new`, multiple bits flip simultaneously in `debouncedGpio`, which flows into the next USB report as simultaneous button presses.

### SOCD (Simultaneous Opposite Cardinal Directions)

Leverless controllers (hitbox) can press Left+Right simultaneously. SOCD cleaning resolves this:
- **Neutral:** Left+Right = nothing
- **Last Win:** Left then Right = Right (most common in FGC)
- **First Win:** Left then Right = Left

**SOCD operates AFTER the sync window.** The pipeline is:
```
GPIO → NOBD sync → debouncedGpio → SOCD clean → processedGamepad → USB
```
So NOBD can safely buffer Left+Right during the window — SOCD cleaning will resolve it before USB.

### Timing on RP2040

**Stock debounce** uses millisecond timing:
```c
uint32_t now = getMillis();  // wraps at ~49 days
```

**NOBD sync window** uses microsecond timing for higher precision:
```c
uint64_t now_us = to_us_since_boot(get_absolute_time());
uint64_t syncDelay_us = (uint64_t)nobdSyncDelay * 1000;  // ms → µs
if (now_us - sync_start_us >= syncDelay_us) { /* commit */ }
```
- 64-bit microseconds — no wrapping concerns
- Sub-millisecond commit accuracy

### USB Polling and Report Timing

- USB 1.1 Full Speed: 1ms polling interval (1000Hz)
- GP2040-CE sends one HID report per USB frame (1ms)
- The main loop runs MUCH faster than 1ms (microseconds per iteration)
- Multiple main loop iterations happen between USB reports
- The sync window expiry is checked every iteration, so commit timing is sub-ms accurate

### Config Mode

When booting into web configurator (hold S2 + plug in):
- The device presents as a USB RNDIS network device
- Web server runs on `192.168.7.1`
- Settings stored in flash via protobuf serialization
- **Config mode bypasses the input pipeline entirely** — no sync window interference

### Flash Storage

Settings are stored in the RP2040's flash:
```
protobuf serialize → flash write (on save)
flash read → protobuf deserialize (on boot)
```
- `debounceDelay` (field 11) and `nobdSyncDelay` (field 34) are both in `GamepadOptions` in `config.proto`
- Both values are always stored — the UI mode toggle just determines which one the firmware uses
- Changes require saving in web UI and potentially rebooting

---

## Cross-Domain Knowledge

### Why NOBD Must Be Firmware-Level

1. **Below the OS:** Any software solution on PC adds variable latency from the OS scheduler
2. **Below the game:** Can't rely on games to implement simultaneous press buffering (most don't)
3. **Universal:** Works on every platform the controller connects to (PC, console, arcade, FPGA)
4. **Replaces debounce:** Can use the same timing budget, adding zero net latency

### Why 5ms Is the Sweet Spot

| Window | Coverage | Latency vs Stock | Risk |
|--------|----------|-----------------|------|
| 0ms | None | -5ms (faster than stock) | All frame boundary issues |
| 3ms | ~90% of presses | -2ms (faster than stock) | Fast players may occasionally split |
| 5ms | ~99% of presses | 0ms (same as stock) | Virtually no risk |
| 8ms | ~99.9% | +3ms | Slight sluggishness on fast inputs |
| 10ms+ | 100% | +5ms+ | Noticeable delay, affects gameplay |

### Edge Cases to Be Aware Of

1. **Piano inputs:** Pressing buttons in rapid sequence (not simultaneous). The sync window should NOT group these — they're intentionally sequential. The 50ms pair window in the tester handles this, and the firmware's window (3-5ms) is short enough that 16ms+ sequential presses pass through individually.

2. **Charge partitioning:** Advanced technique where players briefly release a charge direction. Instant releases in NOBD preserve this.

3. **Kara cancels:** Canceling a normal's startup into a special/throw within 1-2 frames. These are sequential (not simultaneous) so the sync window doesn't interfere.

4. **Multi-button holds:** Holding 3+ buttons for extended periods (EX moves, V-Triggers). Once committed, buttons stay in `debouncedGpio` until released. The sync window only affects the initial press.

5. **Turbo/rapid fire:** GP2040-CE's turbo add-on operates on `processedGamepad`, downstream of the sync window. Turbo generates synthetic press/release cycles that bypass `debounceGpioGetAll()` entirely.

### Testing Methodology

When verifying NOBD behavior:

1. **[Finger gap tester](https://github.com/t3chnicallyinclined/finger-gap-tester)** (Rust GUI or Python CLI) — measures natural human timing, detects strays/bounces/pre-fire
2. **In-game testing** — try 50 dashes in training mode, count drops
3. **Logic analyzer** — attach to GPIO pins and USB data lines to verify timing at hardware level
4. **Cross-platform** — test on PC, console, and Dreamcast (worst case for frame boundaries)

The finger gap tester measures the RAW gap (what the PC sees after USB). With NOBD off, this is your natural finger gap. With NOBD on at value N, you should see gaps cluster near 0ms (both presses grouped) with occasional gaps near Nms (presses that straddled the window boundary).

---

## Part 4: Dreamcast Maple Bus — Native Controller Output

GP2040-CE NOBD includes a native Dreamcast controller output mode that bypasses USB entirely and speaks the Sega Dreamcast's Maple Bus protocol directly via PIO. This is a separate output mode (`INPUT_MODE_DREAMCAST`) — when active, the RP2040 acts as a standard Dreamcast controller with VMU (Visual Memory Unit) emulation.

### Why Native Dreamcast Matters for NOBD

The Dreamcast is the **worst case** for the frame boundary problem. The DC polls controllers at exactly 60Hz (once per VBlank, ~16.67ms). There is **zero sub-frame detection** — if your two presses straddle a 16.67ms boundary, you get a guaranteed split frame. On USB at 1000Hz, you have 1ms boundaries (a 6% chance per press pair at 1ms gap). On Dreamcast, you have 16.67ms boundaries — same problem, longer window. NOBD's sync window is equally valuable here.

### Architecture Overview

**Dual-PIO design:**
- PIO0 (TX): Sends Maple Bus packets — DMA feeds 32-bit words to SM0, which bit-bangs the two-wire protocol (SDCKA/SDCKB) at 480ns/bit
- PIO1 (RX): Three state machines decode incoming packets — SM1/SM2 detect pin transitions (edges), SM0 samples both pins on each edge. DMA ring buffer (256 words) drains SM0's FIFO to RAM continuously
- Software decoder in `pollReceive()` processes the raw pin samples into decoded packets

**Main loop integration:**
When `INPUT_MODE_DREAMCAST` is active, `gp2040.cpp` runs a separate `while(1)` loop that replaces USB with `dreamcastDriver->process(gamepad)`. The input pipeline (NOBD sync or stock debounce → addons → SOCD) is identical — only the output stage changes.

### Maple Bus Protocol Essentials

- **Two-wire, self-clocking** at ~2 Mbps (480ns/bit). Open-drain with pull-ups (idle = both HIGH)
- **Packet format:** `[BitPairsMinus1] [Header: numWords|origin|destination|command] [Payload] [CRC]`
- **Addressing:** DC host = 0x00, Controller = 0x20, VMU sub-peripheral = 0x01
- **Key commands:** DEVICE_REQUEST (1), GET_CONDITION (9), BLOCK_READ (10), BLOCK_WRITE (11)
- **Response timing:** DC expects a response within ~1ms of sending a command

### The RX Byte Order Bug (Root Cause of VMU Failures)

**Discovery:** Controller worked perfectly but VMU reported "not enough blocks available" (MvC2) and "file system error" when trying to save or load.

**Root cause:** The software RX decoder stores bytes in **wire (big-endian) order** — first received byte at lowest index. When this buffer is cast to `uint32_t*` on the little-endian RP2040, each payload word equals `bswap32(original_DC_value)`. For example, `MAPLE_FUNC_MEMORY_CARD = 0x00000002` arrives as `0x02000000` in our payload.

**Why the controller worked anyway:** Controller command handlers (DEVICE_REQUEST, GET_CONDITION, etc.) don't validate payload contents — they just respond. The header struct `{command, destination, origin, numWords}` happens to match wire byte order, so header parsing works correctly without bswap. VMU commands (BLOCK_READ, BLOCK_WRITE, GET_MEDIA_INFO) validate a function code word in the payload — this comparison failed because the value was byte-reversed.

**The fix:** Added `__builtin_bswap32()` to all function code and location word reads in `DreamcastVMU.cpp`. Raw BLOCK_WRITE data is deliberately NOT bswapped — it must stay in wire byte order so the round-trip preserves original bytes:
```
DC LE memory → wire (BE) → our flash (wire order) → TX DMA bswap → PIO MSBit-first → DC LE memory (same bytes)
```

**Reference comparison:** DreamPicoPort (the reference implementation in `maplepad_ref/`) handles this with `channel_config_set_bswap(&c, true)` on the RX DMA channel, automatically converting wire bytes to host order. Our software decoder doesn't have this luxury — bswap must be done manually at the application layer.

**Format version tracking:** Added `VMU_FORMAT_VERSION` byte (currently 2) at system block offset 17. `needsFormat()` checks this value — if it doesn't match the current version, the VMU is re-formatted. This ensures layout constant changes (like save area start block) take effect on existing devices.

### VMU (Virtual Memory Unit) Emulation

The driver emulates a standard VMU at sub-peripheral address 0x01:
- **256 blocks × 512 bytes = 128KB** stored in RP2040 flash at offset 0x001D8000
- **Filesystem layout:** System block (255), FAT (254), file info directory (241-253), save area (blocks 0-199)
- **Auto-formats** on first boot if flash area is blank or format version mismatches
- **Block writes** arrive in 4 phases (128 bytes each) — accumulated in a write buffer, then committed to flash via `processFlashWrite()` in the main loop
- **Flash safety:** `flash_range_erase` + `flash_range_program` disables interrupts for ~50-100ms. Core 1 is not yet launched during initial format (safe). During gameplay saves, this causes a brief controller dropout — acceptable tradeoff.

### Implementation Status (Epic Phases)

The Dreamcast driver was built in phases following the plan in `curried-swimming-kahn.md`:

**Phase 1: Protocol Fixes (COMPLETE)**
- SM restart after TX (the critical fix for disconnect cycling)
- RX clock divider corrected (3.0 → 1.0, matching reference)
- Bus line check before TX (prevents contention)
- RX DMA ring buffer (prevents FIFO overflow)
- TXSTALL-based TX completion detection

**Phase 2: Architecture Alignment (PARTIAL)**
- Display rate-limited to 10 FPS in DC mode ✓
- GPIO conflict guard, DriverManager integration, unified loop — deferred (working without them)

**Phase 3: Feature Completion (IN PROGRESS)**
- VMU enabled with byte order fix ✓
- VMU format version tracking ✓
- Analog stick, triggers, all buttons working ✓
- Rumble/vibration support — not yet implemented
- L2/R2 analog trigger fix, analog rounding — not yet implemented

**Phase 4: Core 1 Optimization (FUTURE)**
- Move Maple Bus I/O to Core 1 for sub-100µs response times — deferred

### Key Lessons from Dreamcast Development

1. **PIO SMs must be fully restarted, not just re-enabled.** `pio_sm_set_enabled(true)` resumes from the frozen instruction — after TX, edge-watcher SMs can be in any state. Full restart (`pio_sm_restart` + `pio_sm_exec(jmp)`) eliminated the ~12% per-TX packet loss that caused 1-3 Hz disconnect cycling.

2. **Wire byte order vs host byte order is the #1 gotcha.** Maple Bus sends MSBit-first per 32-bit word. A software decoder stores bytes in wire (BE) order. On LE hardware, `uint32_t` casts are byte-reversed. Header parsing can accidentally work (struct field order matches wire order) while payload validation fails silently.

3. **The CP (consecutive polls) diagnostic was the key insight.** Tracking how many successful GET_CONDITION polls the DC completed before re-probing (DEVICE_REQUEST) pointed directly at per-TX-cycle failure probability, not latency.

4. **DMA bswap on TX creates correct wire order from LE structs.** The TX DMA has `channel_config_set_bswap` enabled, converting LE values to BE wire order. Word 0 (bitPairsMinus1, a PIO counter not wire data) is pre-bswapped to cancel the DMA bswap. This is elegant but means raw block data from flash (stored in wire order) gets double-bswapped back to original — which is correct for the round-trip.

### Files

| File | Purpose |
|------|---------|
| `src/drivers/dreamcast/maple.pio` | PIO assembly: TX + RX triple program |
| `src/drivers/dreamcast/maple_bus.cpp` | Transport: init, sendPacket, pollReceive, decodeSample, CRC |
| `headers/drivers/dreamcast/maple_bus.h` | MapleBus class, packet structs, protocol constants |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Application: button mapping, command dispatch |
| `headers/drivers/dreamcast/DreamcastDriver.h` | Driver class, debug counters |
| `src/drivers/dreamcast/DreamcastVMU.cpp` | VMU: filesystem, block read/write, flash persistence |
| `headers/drivers/dreamcast/DreamcastVMU.h` | VMU constants and class |
| `maplepad_ref/` | DreamPicoPort reference implementation |
