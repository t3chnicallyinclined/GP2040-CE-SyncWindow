# GP2040-CE NOBD — Expert Agent Context

Complete reference for an AI agent working on this repository. Covers every subsystem, where code lives, how to build, how to modify the web UI, and all domain knowledge needed to make changes without scanning the entire repo.

---

## 1. Project Overview

A fork of GP2040-CE v0.7.12 adding two major features:

1. **NOBD (No OBD)** — firmware-level sync window that groups near-simultaneous button presses onto the same USB frame. Built for fighting games (MvC2) where split inputs produce wrong moves.

2. **Native Dreamcast Maple Bus Output** — the RP2040 speaks Maple Bus directly over two GPIO pins, acting as a standard Dreamcast controller with VMU (memory card) emulation. No adapter needed.

**Repo:** https://github.com/t3chnicallyinclined/GP2040-CE-NOBD
**Boards:** RP2040AdvancedBreakoutBoard (primary), Pico, PicoW, Pico2
**Base:** GP2040-CE v0.7.12 — for anything not NOBD/DC specific, see https://gp2040-ce.info/

---

## 2. Complete File Map

### NOBD (Sync Window)

| File | What | Where to Edit |
|------|------|---------------|
| `src/gp2040.cpp` | `syncGpioGetAll()` (~line 303) — the NOBD algorithm. `debounceGpioGetAll()` (~line 267) — stock debounce. Main loop dispatch (~line 397). DC mode: `updateCmd9FromGpio()` + `updateAnalogFromGamepad()`. | Sync algorithm, main loop structure |
| `headers/gp2040.h` | Declares `syncGpioGetAll()`, `buttonGpios` mask | Function signatures |
| `proto/config.proto` | `nobdSyncDelay` field 34, `nobdReleaseDebounce` field 35 in `GamepadOptions` | Adding new config fields |
| `src/config_utils.cpp` | `DEFAULT_NOBD_SYNC_DELAY = 5`, `nobdReleaseDebounce = false`, `INIT_UNSET_PROPERTY` | Default values |
| `src/webconfig.cpp` | `readDoc`/`writeDoc` for `nobdSyncDelay` and `nobdReleaseDebounce` | Web API read/write |
| `www/src/Pages/SettingsPage.jsx` | Mode dropdown + value field + Release Debounce checkbox (~line 1692) | UI controls |
| `www/src/Locales/en/SettingsPage.jsx` | Translation labels for input timing controls | UI text |

### Dreamcast Maple Bus Driver

| File | What | Where to Edit |
|------|------|---------------|
| `src/drivers/dreamcast/maple.pio` | PIO assembly — TX program (SM0 on PIO0) + RX triple program (SM0-2 on PIO1). ~192 lines. | Wire protocol timing, bit sampling |
| `src/drivers/dreamcast/maple_bus.cpp` | Transport layer — `init()`, `sendPacket()`, `pollReceive()`, `decodeSample()`, `flushRx()`, `startRx()`, CRC. ~324 lines. | Packet send/receive, DMA, SM restart |
| `headers/drivers/dreamcast/maple_bus.h` | MapleBus class, protocol constants (`MAPLE_CMD_*`, `MAPLE_FUNC_*`), addressing, packet helpers. ~163 lines. | Adding commands, changing constants |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Application layer — ISR command dispatch (`isrCommandDispatch`), lookup table (`buildCmd9LookupTable`), `rebuildAllPacketsForPort()`, `buildGpioDcMap()`. | ISR dispatch, response building |
| `headers/drivers/dreamcast/DreamcastDriver.h` | Driver class, pre-built packet buffers, lookup table, debug counters. | Adding buffers, changing interface |

### VMU (Virtual Memory Unit)

| File | What | Where to Edit |
|------|------|---------------|
| `src/drivers/dreamcast/DreamcastVMU.cpp` | VMU sub-peripheral — filesystem init, `handleCommand()`, block read/write, FAT management, flash persistence, media info encoding. ~863 lines. | Save/load logic, filesystem |
| `headers/drivers/dreamcast/DreamcastVMU.h` | VMU constants (`VMU_NUM_BLOCKS=256`, `VMU_BYTES_PER_BLOCK=512`, etc.), class, flash offset. ~184 lines. | Constants, adding VMU features |

### Display / OLED

| File | What | Where to Edit |
|------|------|---------------|
| `src/addons/display.cpp` | Display addon — S1 hold for diagnostics toggle. DC mode runs at full speed (ISR handles Maple Bus). | Button hold detection, DC display behavior |
| `headers/addons/display.h` | Display addon class | |
| `src/display/ui/screens/ButtonLayoutScreen.cpp` | `drawScreen()` — DC diagnostic view (Rx/Tx/XF/ISR timing), normal button layout in DC mode. | What shows on OLED |
| `headers/display/ui/screens/ButtonLayoutScreen.h` | Screen class | |

### Integration / Glue

| File | What | Where to Edit |
|------|------|---------------|
| `src/drivermanager.cpp` | `getDCDriver()` — returns the DC driver instance | Adding new drivers |
| `headers/drivermanager.h` | DriverManager class | |
| `src/gp2040aux.cpp` | Core 1 loop — calls `dcDriver->vmu.doFlashWriteFromCore1()` for VMU flash writes | Core 1 tasks |
| `CMakeLists.txt` | Build config — DC source files, PIO compilation | Adding new source files |
| `configs/RP2040AdvancedBreakoutBoard/BoardConfig.h` | Board-specific pin defaults | Pin assignments |

### Web UI (VMU Manager)

| File | What | Where to Edit |
|------|------|---------------|
| `www/src/Pages/VMUManagerPage.jsx` | VMU Manager page — export .bin, import .dci, format VMU. ~149 lines. | VMU web features |
| `www/src/Locales/en/VMUManagerPage.jsx` | Translation labels | UI text |
| `www/src/App.jsx` | Route registration for VMU Manager | Adding new pages |
| `www/src/Components/Navigation.jsx` | Nav menu entry | Adding nav items |
| `www/src/Services/WebApi.js` | API client — VMU download/upload/format endpoints | Adding API calls |

### Config Proto

| File | What | Where to Edit |
|------|------|---------------|
| `proto/config.proto` | Protobuf schema — `GamepadOptions` fields 34-39 are NOBD/DC. Fields: `nobdSyncDelay`(34), `nobdReleaseDebounce`(35), `dreamcastPinA`(36), `dreamcastPinB`(37), `dcSyncMode`(38, deprecated), `dcSyncWindow`(39, deprecated) | Adding new config fields |
| `proto/enums.proto` | `INPUT_MODE_DREAMCAST = 16` | Adding input modes |

---

## 3. NOBD Sync Window

### Algorithm (`syncGpioGetAll()` in `src/gp2040.cpp`)

```
1. Read raw GPIO: raw_buttons = ~gpio_get_all() & buttonGpios
2. Detect new presses: just_pressed = raw_buttons & ~debouncedGpio
3. If new press and no window open → start window, record in sync_new
4. If window open → accumulate new presses into sync_new
5. Every cycle: sync_new &= raw_buttons (bounce filter — drop flickering bits)
6. When (now_us - sync_start_us) >= syncDelay_us → commit sync_new to debouncedGpio
7. Releases always instant: debouncedGpio &= ~just_released (no delay)
```

**Mutually exclusive** with `debounceGpioGetAll()`. Controlled by `nobdSyncDelay > 0`.

### Release Debounce (opt-in, `nobdReleaseDebounce`)

When enabled, releases are buffered using the same sync window timer. If the switch bounces back ON during the window, the release is cancelled (button appears to never have released). For rhythm games, not fighting games. Off by default.

### Why NOBD Exists

USB polls at 1000Hz (1ms). Human fingers have 2-8ms gap between "simultaneous" presses. At 1ms polling, this gap is visible as separate USB reports. Games with zero input leniency (MvC2) see this as sequential presses → wrong move (jab instead of dash). NOBD groups them so both appear on the same USB frame. See `docs/WHY-NOBD.md` for the full technical argument.

---

## 4. Dreamcast Maple Bus Driver

### Architecture

**Dual-PIO design:**
```
PIO0 (TX): SM0 — DMA-fed, drives pinA+pinB, bit-bangs Maple protocol at 480ns/bit
PIO1 (RX): SM0 — samples both pins (2 bits) on each IRQ 7, autopush at 32 bits
            SM1 — watches pinA edges → fires IRQ 7
            SM2 — watches pinB edges → fires IRQ 7
```

RX uses 256-word DMA ring buffer to prevent FIFO overflow during main loop latency.

**Wire-order architecture:** All data stays in wire (big-endian/network) byte order. No DMA bswap on RX. Field extraction uses explicit `__builtin_bswap32()`. TX DMA bswap is enabled to convert LE structs to wire order.

### Maple Bus Protocol

- **Two-wire, self-clocking** at ~2 Mbps (480ns/bit). Open-drain, pull-ups, idle = both HIGH.
- **Packet:** `[BitPairsMinus1] [Header: numWords|origin|dest|cmd] [Payload...] [CRC]`
- **Addressing:** DC host=0x00, Controller=0x20, VMU sub-periph=0x01, Port mask=0xC0
- **Key commands:** DEVICE_REQUEST(1), GET_CONDITION(9), BLOCK_READ(10), BLOCK_WRITE(11)
- **Response window:** <1ms or DC considers peripheral unresponsive

### GPIO Configuration

- Default: GPIO 2 (pinA/SDCKA), GPIO 3 (pinB/SDCKB) — must be consecutive
- **AVOID GPIO 23, 24, 26-29** — SMPS/VBUS/ADC circuitry causes Maple Bus corruption
- Configurable via web UI: Settings → Input Mode → Dreamcast
- `proto/config.proto` fields 36-37: `dreamcastPinA`, `dreamcastPinB`

### Button Mapping (GP2040-CE → Dreamcast)

| GP2040 | DC Button | DC Mask | Inverted (0=pressed) |
|--------|-----------|---------|---------------------|
| B1 | A | 0x0004 | Yes |
| B2 | B | 0x0002 | Yes |
| B3 | X | 0x0400 | Yes |
| B4 | Y | 0x0200 | Yes |
| L1 | C | 0x0001 | Yes |
| R1 | Z | 0x0100 | Yes |
| S2 | Start | 0x0008 | Yes |
| L2 | LT (digital→255) | analog | |
| R2 | RT (digital→255) | analog | |
| DPad | DPad | 0x0010-0x0080 | Yes |

### Main Loop Integration

```
gp2040.cpp::run() → if dcMode:
  1. NOBD sync or stock debounce (GPIO read)
  2. gamepad->read() / process()
  3. Addon processing (display at full speed, hotkeys, LEDs)
  4. dcDriver->updateCmd9FromGpio()      — keep lookup table current
  5. dcDriver->updateAnalogFromGamepad() — keep analog axes current
  6. dcDriver->process() — just finishIsrTx() cleanup if ISR fired
  (ISR handles all Maple commands independently — no polling needed)
```

### The Root Cause Story — Disconnect Cycling Bug

**Symptom:** DC cycled between CMD 1 (DEVICE_REQUEST) and CMD 9 (GET_CONDITION) at 1-3 Hz.

**Three bugs combined:**

1. **RX SMs not restarted after TX (CRITICAL):** `pio_sm_set_enabled(true)` resumed from frozen instruction. Edge-watcher SMs could fire spurious IRQ 7 on re-enable → corrupted next RX. CP=8 confirmed ~12% per-TX failure rate.
   **Fix:** Full SM restart: `pio_sm_restart()` + `pio_sm_exec(jmp)` + FIFO clear + IRQ clear.

2. **No bus line check before TX:** Could collide with DC's tail sequence.
   **Fix:** 10µs continuous idle verification before every TX.

3. **RX clock divider too slow:** clkdiv=3.0 (~42MHz) → late sampling at Maple speeds.
   **Fix:** clkdiv=1.0 (125MHz), matching reference.

**Key diagnostic:** CP (consecutive polls before re-probe). Before fix: CP=8. After: CP=0 (never re-probes).

---

## 5. Full ISR-Driven Maple Bus Architecture

### Overview

ALL controller Maple Bus commands are handled from the PIO end-of-packet interrupt (ISR). No `pollReceive()` in the hot path. Core 0 main loop is completely free from Maple Bus polling — runs addons, display, NOBD sync, hotkeys without any DC overhead.

VMU commands (CMD10-13) fall through to main loop via `isrNvicDisabled` flag for now (save/load screens only, not latency-critical).

### How It Works

1. PIO RX receives a complete Maple Bus packet, sets IRQ flag
2. NVIC fires `mapleRxIrqHandler` (highest priority ISR)
3. ISR validates CRC (generic, variable-length)
4. ISR calls `isrCommandDispatch()` — switch on command byte
5. Response sent via `isrSendFifo()` (≤8 words → PIO TX FIFO) or `isrSendDma()` (>8 words → TX DMA)
6. Main loop calls `finishIsrTx()` next iteration → waits for TX completion, restarts RX

### ISR Command Dispatch

| CMD | Response | Words | Method | ISR Time |
|-----|----------|-------|--------|----------|
| 1 (DEVICE_REQUEST) | Device info | 31 | DMA | ~2µs |
| 2 (ALL_STATUS_REQUEST) | Extended info | 51 | DMA | ~2µs |
| 3/4 (RESET/SHUTDOWN) | ACK | 3 | FIFO | ~1µs |
| 9 (GET_CONDITION) | Controller state | 6 | FIFO | ~1-2µs |
| 14 (SET_CONDITION) | ACK | 3 | FIFO | ~1µs |
| 10-13 (VMU) | Various | Various | Main loop fallback | ~15-20µs |

### Zero-Computation CMD9 Response

- `buildCmd9LookupTable()` pre-computes W3 (buttons+triggers) and W5 (CRC) for every possible GPIO button state
- `updateCmd9FromGpio()` runs every main loop iteration, compresses GPIO to table index, stores `cmd9ReadyW3` + `cmd9ReadyW5`
- After first CMD1, `rebuildAllPacketsForPort()` pre-computes all headers+CRCs with the real port address
- ISR just copies pre-built W3+W5 into `controllerPacketBuf` and stuffs FIFO — zero math

### Pre-built Packet Buffers

All static responses pre-built at init with correct headers+CRCs (rebuilt once after CMD1 reveals port):
- `infoPacketBuf[31]` — CMD1 response (device info)
- `extInfoBuf[51]` — CMD2 response (extended info)
- `ackPacketBuf[3]` — CMD3/4/14 response (ACK)
- `controllerPacketBuf[6]` — CMD9 response (dynamic W3+W5 from lookup table)
- `unknownCmdBuf[3]` — unknown command response
- `resendPacketBuf[3]` — RESEND request

### Main Loop (DC Mode)

```cpp
// No tight loop. No pollReceive(). ISR handles everything.
if (dcMode) {
    dcDriver->updateCmd9FromGpio();       // Keep lookup table current
    dcDriver->updateAnalogFromGamepad();  // Keep analog axes current
}
// process() just calls finishIsrTx() if ISR fired
```

### Display

OLED runs at full speed in DC mode — no throttle. Button layout screen shows live button state, same as USB modes. Hold S1 for 3 seconds to toggle diagnostic overlay.

### Performance

| Metric | Value |
|--------|-------|
| CMD9 ISR response time | 1-2µs |
| XF (CRC failures) | 0 |
| Wire transmission time | ~65-92µs (protocol physics, all controllers) |
| Main loop overhead for DC | ~5µs/iteration (updateCmd9 + updateAnalog + finishIsrTx) |

### Other Adapter Comparison (verified from source code, March 2025)

| Adapter | Architecture | Response time |
|---------|-------------|---------------|
| **GP2040-CE NOBD** | Full ISR — pre-computed lookup table + PIO FIFO TX | **~1-2µs** |
| MaplePad (RP2040) | GPIO read at response time in main loop | ~10-50µs |
| GmanModz (PIC32) | Pre-built buffer from earlier poll | ~100-500µs |
| BlueRetro (ESP32) | Bluetooth buffer, not at response time | ~1-5ms |

---

## 6. VMU (Virtual Memory Unit) Emulation

### Storage Layout

256 blocks × 512 bytes = 128KB, stored in RP2040 flash at offset `0x001D8000`.

```
Blocks 0-199:    Save area (200 blocks, user data)
Blocks 200-240:  Reserved (system/BIOS)
Blocks 241-253:  File info directory (13 blocks, 32 bytes per entry)
Block 254:       FAT (256 entries × 16 bits)
Block 255:       System block (signature, media info, format version)
```

### FAT Values

| Value | Meaning |
|-------|---------|
| 0xFFFC | Free block |
| 0xFFFA | System-reserved |
| 0x0000-0x00FF | Chain pointer to next block |

### Media Info Encoding

**On flash:** Native LE 16-bit pairs.
**Over Maple Bus (GET_MEMORY_INFO):** BE 16-bit pairs using BSWAP16 macros:
```c
#define MEDIA_HI(v) ((uint32_t)BSWAP16(v) << 16)
#define MEDIA_LO(v) ((uint32_t)BSWAP16(v))
```

### Byte Order — The Critical Rule

```
Our TX DMA bswap + DC RX DMA bswap = identity (cancels out)
```

**BLOCK_READ:** `memcpy()` flash → response. NO software bswap.
**BLOCK_WRITE:** `memcpy()` payload → write buffer. NO software bswap.

Adding `__builtin_bswap32()` to block data creates a triple-swap = corruption. This was the root cause of the MvC2 "error writing" bug.

### Flash Writes

- Read-modify-write of 4KB flash sectors (~45ms)
- Done on Core 1 via `doFlashWriteFromCore1()` in `gp2040aux.cpp`
- Core 0's Maple Bus hot path is `__no_inline_not_in_flash_func` (runs from RAM) so it stays responsive during flash ops

### System Block Self-Correction

The DC sometimes writes `0` for save area fields during formatting. Protection code detects and restores correct values (`VMU_NUM_SAVE_BLOCKS=200`, `VMU_SAVE_AREA_BLOCK_NO=31`).

### Format Version

`VMU_FORMAT_VERSION` byte at system block offset 17. Forces re-format on firmware updates that change layout. Currently version 4.

---

## 7. OLED Display in DC Mode

### Normal Mode (default)
- Shows normal button layout screen (same as USB modes) — live button state visualization
- **Full speed** — ISR handles Maple Bus independently, no display throttle needed
- Status bar, profile banner, button widgets all work

### Diagnostic Mode
- Toggle: Hold S1 (Select) 3 seconds
- Shows live counters: Rx, Tx, XF, VMU reads/writes, ISR response time
- Line 3: `ISR:X-Yus n:N` — response time range and count
- Line 4: `c9:N t:N f:N` — CMD9 count, table hits, ISR fallthroughs

### Implementation
- Button hold detection: `src/addons/display.cpp` (S1 hold timer)
- Screen drawing: `src/display/ui/screens/ButtonLayoutScreen.cpp` (`drawScreen()`)
- Drawing primitives: `getRenderer()->drawText(x, row, text)`, `drawRectangle(x, y, w, h, filled, inverted)`

---

## 8. Configuration Pipeline

Adding a new setting requires changes at every layer:

```
proto/config.proto          → Add field to GamepadOptions (pick next field number)
src/config_utils.cpp        → Add default value via INIT_UNSET_PROPERTY
src/webconfig.cpp           → Add readDoc() and writeDoc() handlers
www/src/Pages/SettingsPage.jsx → Add UI control
www/src/Locales/en/SettingsPage.jsx → Add translation label
```

### Existing NOBD/DC Fields

| Field | Number | Type | Default | Location |
|-------|--------|------|---------|----------|
| nobdSyncDelay | 34 | uint32 | 5 | GamepadOptions |
| nobdReleaseDebounce | 35 | bool | false | GamepadOptions |
| dreamcastPinA | 36 | uint32 | 2 | GamepadOptions |
| dreamcastPinB | 37 | uint32 | 3 | GamepadOptions |
| dcSyncMode | 38 | uint32 | deprecated | GamepadOptions |
| dcSyncWindow | 39 | uint32 | deprecated | GamepadOptions |

---

## 9. Web UI Changes

### Modifying Existing Pages

1. Edit the page component: `www/src/Pages/<PageName>.jsx`
2. Edit translations: `www/src/Locales/en/<PageName>.jsx`
3. Rebuild: `cd www && npm run build && cd ..`
4. Rebuild firmware (web assets embedded via makefsdata)

### Adding a New Page

1. Create `www/src/Pages/NewPage.jsx`
2. Create `www/src/Locales/en/NewPage.jsx`
3. Add route in `www/src/App.jsx`
4. Add nav entry in `www/src/Components/Navigation.jsx`
5. Add locale import in `www/src/Locales/en/Index.jsx`
6. Add API endpoints in `www/src/Services/WebApi.js` if needed
7. Add backend handlers in `src/webconfig.cpp` if needed
8. Rebuild web + firmware

### Settings Page Input Timing Section

Mode dropdown + value field at ~line 1692 in `SettingsPage.jsx`. When NOBD mode is selected, Release Debounce checkbox appears. Switching modes preserves both values in flash.

---

## 10. Building

### Prerequisites
- Windows with MSVC Build Tools 2022
- CMake + Ninja
- ARM GCC toolchain (arm-none-eabi-gcc 14.2)
- Node.js + npm (for web UI)

### Quick Build (single board, for testing)

From Claude Code's bash shell:
```bash
powershell.exe -Command "cmd.exe /c 'C:\Users\trist\projects\GP2040-CE\build_one.bat'" 2>&1
```

Builds `RP2040AdvancedBreakoutBoard` only. Output: `build/GP2040-CE-NOBD_*.uf2`

### Release Build (all 4 boards)

```bash
powershell.exe -Command "cmd.exe /c 'C:\Users\trist\projects\GP2040-CE\build_nobd.bat'" 2>&1
```

Builds RP2040AdvancedBreakoutBoard, Pico, PicoW, Pico2. Copies UF2s to `release/`.

### Web UI Rebuild (required before firmware if www/ changed)

```bash
cd www && npm install && npm run build && cd ..
```

Then rebuild firmware — web assets are embedded.

### Creating a Release

```bash
gh release create v0.7.12-nobd-X release/GP2040-CE-NOBD_0.7.12_*.uf2 --title "Title" --notes "Notes"
```

### SDK Note

mbedtls submodule must be at v2.28.8 for PS4 driver compatibility:
```bash
cd build/_deps/pico_sdk-src/lib/mbedtls && git checkout v2.28.8
```

---

## 11. Common Tasks — Quick Reference

| Task | What to Edit |
|------|-------------|
| Change default sync window | `src/config_utils.cpp` → `DEFAULT_NOBD_SYNC_DELAY` |
| Change default debounce | `src/config_utils.cpp` → `DEFAULT_DEBOUNCE_DELAY` |
| Add a DC button mapping | `DreamcastDriver.cpp` → `mapButtonsToDC()` + `buildGpioDcMap()` |
| Add a VMU command handler | `DreamcastVMU.cpp` → `handleCommand()` switch |
| Change OLED display content | `ButtonLayoutScreen.cpp` → `drawScreen()` |
| Add a new config field | See Configuration Pipeline (section 8) |
| Add a new web page | See Web UI Changes (section 9) |
| Add a new board | Copy `configs/` entry, modify pins, add to `build_nobd.bat` |
| Change PIO timing | `maple.pio` — TX/RX programs |
| Debug DC connection | Enable diagnostics (hold S1 3s), check XF=0, CP=0, cr=9 |

---

## 12. Key Lessons & Gotchas

1. **PIO SMs must be fully restarted after TX.** `pio_sm_set_enabled(true)` resumes from frozen instruction → spurious samples → ~12% packet loss. Always: `pio_sm_restart()` + `pio_sm_exec(jmp)` + FIFO clear.

2. **Double-DMA bswap = identity for block data.** TX DMA bswap + DC RX DMA bswap cancel. Never add software bswap to block data — creates triple-swap = corruption. Use `memcpy()`.

3. **Media info has two encodings.** Flash: native LE. Wire: BE 16-bit pairs (BSWAP16). Mixing = DC sees wrong filesystem.

4. **GPIO 23/24 corrupt Maple Bus.** SMPS/VBUS circuitry on those pins. Use clean pins like 2/3.

5. **Display was a red herring during debugging.** At 600-800µs loop time, OLED I2C wasn't the bottleneck. Rate-limiting to 10 FPS is good practice but wasn't the disconnect fix.

6. **CP diagnostic is the key metric.** Consecutive polls before DC re-probes. Should be 0 (DC never re-probes after initial connection).

7. **System block self-correction is essential.** DC can zero out save area fields during writes. Without protection, VMU permanently can't save.

8. **Flash writes on Core 1, hot path in RAM.** `flash_range_erase` disables interrupts for ~45ms. Core 0's Maple code must be `__no_inline_not_in_flash_func` to stay responsive.

9. **NOBD sync and stock debounce are mutually exclusive.** Both write `debouncedGpio`. Controlled by `nobdSyncDelay > 0`.

10. **Web UI changes require two rebuilds.** First `npm run build` (web assets), then firmware rebuild (embeds assets via makefsdata).

---

## 13. Domain Knowledge — Fighting Games & Input Systems

### Why NOBD Matters

USB polls at 1000Hz (1ms intervals). Human fingers have 2-8ms gap between "simultaneous" presses. At 1ms polling, this gap produces separate USB reports. Games with zero input leniency (MvC2) see split presses as sequential inputs → wrong move.

NOBD groups presses within a configurable window (default 5ms) so both appear on the same USB report. Same execution requirement (press two buttons), guaranteed same-frame delivery.

### Key Simultaneous Inputs in Fighting Games

| Input | Buttons | If Split |
|-------|---------|----------|
| Dash (LP+HP) | 2 attack | Stray jab |
| Throw tech | LP+LK | Jab/short |
| Roman Cancel (GG) | 3 buttons | Wrong RC type |
| Super activation | Multiple | Wrong move |

### MvC2 Specifics

- Single input read per frame (confirmed by testing)
- Zero leniency for simultaneous presses
- Arcade-era strict input model (SF2-style)
- Dreamcast version: VBlank-synced polling (Maple DMA tied to VBlank interrupt in KallistiOS)
- Frame boundary problem is worst on USB (1ms boundaries) but also exists on DC (16.7ms boundaries)

### Dreamcast Polling

- 60Hz, tied to VBlank interrupt
- Controller reports instantaneous state when polled (no buffering, no history)
- Original Sega ASIC (315-6125) reads GPIO at response time — no staleness
- Our ISR architecture matches this — pre-computed lookup table updated every main loop cycle, ISR reads it at response time (~1-2µs)

### Platform Polling Rates

| Platform | Rate | Interval |
|----------|------|----------|
| PC/XInput | 1000Hz | 1ms |
| PS4/PS5 | 1000Hz | 1ms |
| Switch Pro | 125Hz | 8ms |
| Dreamcast | 60Hz | 16.67ms |

---

## 14. External References

- **Maple Bus wire protocol:** http://mc.pp.se/dc/maplewire.html
- **Maple Bus commands:** http://mc.pp.se/dc/maplebus.html
- **VMU filesystem:** https://mc.pp.se/dc/vms/flashmem.html
- **MaplePad reference:** `maplepad_ref/` directory in this repo
- **RP2040 PIO docs:** https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf (Chapter 3)
- **GP2040-CE upstream:** https://gp2040-ce.info/
- **Finger gap tester:** https://github.com/t3chnicallyinclined/finger-gap-tester
- **KallistiOS Maple:** http://gamedev.allusion.net/docs/kos-current/maple_8h_source.html
- **Switch bounce study:** https://www.ganssle.com/debouncing.htm
