---
name: Dreamcast Maple Bus Driver — Expert Context
description: Complete developer reference for the GP2040-CE Dreamcast Maple Bus driver — architecture, protocol, PIO, debugging, root cause history, and current status (WORKING as of 2026-03-23)
type: project
---

# Dreamcast Maple Bus Driver — Expert Context

## Overview

GP2040-CE NOBD has a native Dreamcast controller output mode that communicates via the Sega Dreamcast Maple Bus protocol. Instead of USB, the RP2040 directly speaks Maple Bus over two GPIO pins, acting as a standard controller with VMU emulation.

**Current Status (2026-03-23): WORKING.** Controller is fully functional on real Dreamcast hardware — stable connection, no disconnect cycling, all buttons/dpad/triggers confirmed working. Played MvC2 for 5+ minutes with zero disconnects. CP=0 (DC never re-probed), cr=9 (steady GET_CONDITION polling), XF=0 (zero decode errors).

**VMU Status: ENABLED, BLOCK_READ bswap bug fixed (2026-03-23).** MvC2 detects memory card and free space. Previous triple-bswap bug in sendBlockReadResponse() fixed (memcpy instead of bswap32). Format version tracking (VMU_FORMAT_VERSION=4). Awaiting real-hardware save test with latest build. See [vmu_expert_reference.md](vmu_expert_reference.md) for comprehensive VMU technical details.

**Reference Implementation:** MaplePad/DreamPicoPort at `maplepad_ref/` — a known-good RP2040 Maple Bus controller emulator. Our PIO code is originally ported from Charlie Cole's work.

---

## Architecture

### Dual-PIO Design

```
PIO0 (TX)                    PIO1 (RX)
  SM0: maple_tx_program        SM0: maple_rx_triple1 (samples pins on IRQ 7)
                                SM1: maple_rx_triple2 (watches pinA edges → IRQ 7)
                                SM2: maple_rx_triple3 (watches pinB edges → IRQ 7)
```

- **TX** uses PIO0 SM0 + DMA channel. Drives both pins via `set pins` (pinA, pinB) and `sideset` (pinB).
- **RX** uses PIO1 SM0-2. SM1/SM2 detect transitions on pinA/pinB and fire IRQ 7. SM0 samples both pins (2 bits) on each IRQ 7. Autopush at 32 bits (16 samples per FIFO word). FIFO joined for 8 entries deep.
- **RX DMA ring buffer** (256 words, 1024 bytes) continuously drains SM0's FIFO to RAM, preventing overflow during main loop latency spikes.

### GPIO Configuration

- **Default pins:** GPIO 23 (pinA/SDCKA) and GPIO 24 (pinB/SDCKB) — must be consecutive
- **Configurable** via `dreamcastPinA`/`dreamcastPinB` in `config.proto` fields 36-37
- Internal pull-ups enabled on both pins via `gpio_pull_up()`
- `pio_gpio_init(pio0, pin)` sets GPIO function to PIO0 for TX. PIO1 (RX) reads the pad directly regardless of GPIO mux.

### Main Loop Integration

```
gp2040.cpp::run() → if (dreamcastMode) → separate while(1) loop:
  1. Input pipeline (NOBD sync or stock debounce)
  2. Addon processing (display rate-limited to 10 FPS in DC mode)
  3. dreamcastDriver->process(gamepad) — replaces USB driver
```

The display addon (`src/addons/display.cpp`) is rate-limited to one draw per 100ms in Dreamcast mode to avoid I2C blocking the main loop.

---

## File Map

| File | Purpose |
|------|---------|
| `src/drivers/dreamcast/maple.pio` | PIO assembly: TX program + RX triple program |
| `src/drivers/dreamcast/maple_bus.cpp` | Transport layer: init, sendPacket, pollReceive, decodeSample, flushRx, CRC |
| `headers/drivers/dreamcast/maple_bus.h` | MapleBus class, packet structs, protocol constants |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Application layer: packet building, button mapping, command dispatch |
| `headers/drivers/dreamcast/DreamcastDriver.h` | DreamcastDriver class, debug counters |
| `src/drivers/dreamcast/DreamcastVMU.cpp` | VMU sub-peripheral: filesystem, block read/write, flash persistence |
| `headers/drivers/dreamcast/DreamcastVMU.h` | VMU constants and class |
| `src/gp2040.cpp` | Dreamcast mode setup (~line 200) and main loop (~line 378) |
| `src/addons/display.cpp` | Display addon with DC mode rate-limiting |
| `src/display/ui/screens/ButtonLayoutScreen.cpp` | OLED debug display for DC diagnostics |
| `proto/enums.proto` | `INPUT_MODE_DREAMCAST = 16` |
| `proto/config.proto` | `dreamcastPinA` (36), `dreamcastPinB` (37), `dcSyncMode` (38), `dcSyncWindow` (39) |
| `src/config_utils.cpp` | Defaults: pinA=23, pinB=24 |
| `maplepad_ref/` | DreamPicoPort reference implementation (known-good) |
| `C:\Users\trist\.claude\plans\curried-swimming-kahn.md` | Expert analysis comparing GP2040-CE vs reference — bug list and fix priorities |

---

## Maple Bus Protocol Reference

**Spec:** http://mc.pp.se/dc/maplewire.html and http://mc.pp.se/dc/maplebus.html

### Wire Protocol
- **Two-wire:** SDCKA (pinA) and SDCKB (pinB), open-drain with pull-ups (idle = both HIGH)
- **Self-clocking:** ~2 Mbps, 480ns per bit period
- **Sync:** `0b10` → 4× (`0b00`, `0b10`) → `0b11` (both high)
- **Data:** Alternating Phase A (pinA clocks) and Phase B (pinB clocks), MSB first
- **End:** Tail sequence (8 transitions), then both pins idle HIGH
- **CRC:** XOR of all bytes, folded to 1 byte

### Packet Format
```
[BitPairsMinus1] [Header: numWords|origin|destination|command] [Payload] [CRC]
```

### Key Commands
| Command | Value | Direction | Our Response |
|---------|-------|-----------|--------------|
| DEVICE_REQUEST | 1 | DC→Ctrl | RESPOND_DEVICE_STATUS (5), 28 words |
| ALL_STATUS_REQUEST | 2 | DC→Ctrl | RESPOND_ALL_STATUS (6), 48 words |
| GET_CONDITION | 9 | DC→Ctrl | RESPOND_DATA_XFER (8), controller state |
| RESET_DEVICE | 3 | DC→Ctrl | RESPOND_ACK (7) |
| SHUTDOWN_DEVICE | 4 | DC→Ctrl | RESPOND_ACK (7) |
| Unknown | any | DC→Ctrl | RESPOND_UNKNOWN_CMD (-3/0xFD) |

### Addressing
- `MAPLE_DC_ADDR = 0x00` — Dreamcast host
- `MAPLE_CTRL_ADDR = 0x20` — Controller (main peripheral)
- `MAPLE_SUB0_ADDR = 0x01` — VMU sub-peripheral
- `MAPLE_PORT_MASK = 0xC0` — Top 2 bits = port number (0-3)
- Port bits from DC's request echoed back in response
- Origin includes sub-peripheral bits: `0x20 | 0x01 | port = 0x21` (when VMU enabled)

---

## What's Working (as of 2026-03-23)

- Full controller functionality: all 11 buttons, dpad, 2 triggers, analog stick
- Stable connection for 5+ minutes gameplay (tested in MvC2)
- CP=0 (DC never re-probes after initial connection)
- XF=0 (zero RX decode errors)
- RX DMA ring buffer eliminates FIFO overflow
- TXSTALL-based TX completion detection (precise, no timing guesswork)
- Proper RX SM restart after TX (the key fix for stability)
- Bus line check before TX (prevents contention)
- Display rate-limited to 10 FPS in DC mode
- All protocol responses match MaplePad reference
- **VMU enabled** — RX byte order bug fixed, funcCode/location words bswapped
- **VMU format version tracking** — VMU_FORMAT_VERSION=2 at system block offset 17, forces re-format on layout changes
- **VMU filesystem** — standard layout: system block 255, FAT 254, file info 241-253, save area blocks 0-199

## What Needs Work

- **VMU real-hardware testing:** Save/load in MvC2 — byte order fix is implemented and builds clean but untested on real DC
- **TX speed:** Still ~2.4x slower than spec (clkdiv=3.0 on TX). Works but suboptimal
- **Timeout protection:** No timeout on flushRx() busy-waits. Could hang on PIO fault
- **VMU flash writes:** Block erase+program disables interrupts for ~50-100ms, will cause brief disconnect during saves
- **Rumble/vibration:** Not yet implemented (SET_CONDITION with MAPLE_FUNC_VIBRATION)
- **Analog rounding:** `(uint8_t)(lx >> 8)` truncates instead of rounding
- **L2/R2 analog trigger fix:** Digital override clobbers analog values unconditionally

---

## The Root Cause Story — Disconnect/Reconnect Cycling

### Symptom
The DC would cycle between CMD 1 (DEVICE_REQUEST) and CMD 9 (GET_CONDITION) at 1-3 Hz. OLED showed `cr` flashing between 9 and 1. In MvC2, this manifested as turbo-like inputs during matches (held buttons reset each disconnect cycle). In menus, no disconnect popup appeared — the cycling was invisible.

### Investigation Timeline

**Phase 1: Suspected TX timing / electrical**
- Early builds had working RX/TX with matching counters (Rx=Tx, XF=0) but DC didn't detect controller
- Tried various flushRx delays (300µs, 200µs, 50µs) — no effect

**Phase 2: RX FIFO overflow**
- XF counter was increasing — diagnosed as PIO FIFO overflow
- A 9-byte Maple command = ~154 pin samples. With 32-bit autopush, 128-sample FIFO capacity was insufficient if main loop was slow
- **Fix:** RX DMA ring buffer (256 words continuously draining FIFO to RAM)
- **Result:** XF dropped to 0, but cycling continued

**Phase 3: Suspected display latency (WRONG)**
- Hypothesized that OLED I2C draws (10-50ms per frame) blocked the main loop past DC's ~1ms response timeout
- Added `debugLoopMaxUs` diagnostic → showed 600-800µs (well under 1ms)
- Disconnected the OLED entirely → cycling still happened
- **Conclusion:** Display was NOT the root cause

**Phase 4: Suspected VMU sub-peripheral (WRONG)**
- Disabled VMU entirely (`disableVMU = true`): no VMU init, no sub-peripheral announcement, no VMU routing
- Cycling still happened
- **Conclusion:** VMU was NOT the root cause

**Phase 5: Expert analysis identified transport-layer bugs**
- Analysis document (`curried-swimming-kahn.md`) compared GP2040-CE vs DreamPicoPort reference
- Identified critical PIO-level bugs that had been present since day one

### The Actual Root Cause: Three PIO Bugs Combined

**BUG 1 (CRITICAL): RX SMs not restarted after TX**
- `flushRx()` re-enabled RX SMs with `pio_sm_set_enabled()` but didn't restart them
- SMs resumed from the exact instruction where they were frozen by `sendPacket()`
- SM1/SM2 (edge watchers) could be frozen at `irq 7` → fires immediately on re-enable → spurious sample injected into SM0
- SM0 could be frozen at `in pins, 2` → immediately samples current pin state (idle = 0b11)
- Edge watchers could be in wrong half of their wait-pair cycle (e.g., `wait 1 pin 0` when pin already HIGH → immediately completes → fires IRQ7 → more spurious samples)
- **Result:** ~1-in-8 chance of missing the next DC command after each TX. CP=8 confirmed this — DC successfully polled 8 times, then one poll was missed, DC timed out and re-probed.

**Fix:** Full SM restart matching reference implementation:
```cpp
for (int sm = 0; sm < 3; sm++) {
    pio_sm_clear_fifos(rxPio, sm);
    pio_sm_restart(rxPio, sm);
    pio_sm_clkdiv_restart(rxPio, sm);
    pio_sm_exec(rxPio, sm, pio_encode_jmp(rxSmOffsets[sm]));
}
pio_interrupt_clear(rxPio, 7);
```

**BUG 3: RX clock divider too slow (3.0 → 1.0)**
- At clkdiv=3.0 (~42MHz), SM0 took ~72ns to respond to IRQ7 from edge watchers
- Maple bit period is ~480ns with transitions every ~240ns
- Late sampling could cause systematic bit errors on marginal transitions
- **Fix:** Changed RX clkdiv from 3.0 to 1.0 (full 125MHz), matching reference

**BUG 2: No bus line check before TX**
- `sendPacket()` immediately disabled RX and fired DMA without checking if bus was idle
- If DC hadn't finished its tail sequence, our TX would collide
- **Fix:** 10µs open-line verification before every TX:
```cpp
uint64_t startUs = time_us_64();
while ((time_us_64() - startUs) < 10) {
    if (!gpio_get(pinA) || !gpio_get(pinB)) {
        startUs = time_us_64();  // Reset if bus not idle
    }
}
```

### Key Diagnostic: CP (Consecutive Polls)
The `debugConsecutivePolls` / `debugMaxConsecutivePolls` counter proved invaluable. Before the fix, CP=8 — the DC managed 8 successful polls before one was missed. After the fix, CP=0 (the counter never incremented because CMD 1 was only sent once at initial connection, never again).

---

## Protocol Differences from MaplePad (Resolved)

| Issue | Old Code | Fixed Code | MaplePad |
|-------|----------|------------|----------|
| CMD 2 response | cmd 5 (DEVICE_STATUS), 28 words | cmd 6 (ALL_STATUS), 48 words | cmd 6, 48 words |
| Unknown commands | Silent drop | UNKNOWN_COMMAND (0xFD) response | UNKNOWN_COMMAND |
| Sync modes | DC_SYNC_ACCUMULATE (default) | Removed — raw state always | None |
| Pre-response delay | busy_wait_us(10) in driver | Bus line check in transport | lineCheck() |
| RX SM restart | Just re-enable | Full restart + FIFO clear + JMP | prestart() |
| RX clkdiv | 3.0 (~42MHz) | 1.0 (125MHz) | 1.0 |

---

## OLED Debug Display

When `INPUT_MODE_DREAMCAST` is active, 5 debug lines shown (rate-limited to 10 FPS):

```
Row 0: Rx:123 Tx:123 XF:0       — packet counts, decode errors
Row 1: c1:1 c2:0 c9:500          — per-command breakdown
Row 2: Vr:0 Vt:0 Vw:0 f:0       — VMU counters, filtered packets
Row 3: LP:700 CP:0               — loop max µs, consecutive polls before re-probe
Row 4: DC:FFFF cr:9 co:0         — DC buttons, last cmd, other cmd count
```

**Key values to watch:**
- **XF** should be 0 (decode errors)
- **LP** should be <1000 (loop latency µs)
- **CP** should be 0 (DC never re-probes)
- **cr** should stay at 9 (GET_CONDITION)

---

## Button Mapping

GP2040-CE → Dreamcast (inverted logic: 0=pressed, 1=released):

| GP2040 | DC Button | Mask |
|--------|-----------|------|
| B1 | A | 0x0004 |
| B2 | B | 0x0002 |
| B3 | X | 0x0400 |
| B4 | Y | 0x0200 |
| L1 | C | 0x0001 |
| R1 | Z | 0x0100 |
| S2 | Start | 0x0008 |
| L2 (digital) | LT=255 | — |
| R2 (digital) | RT=255 | — |
| D-pad | D-pad | 0x0010-0x0080 |

Analog: `lx >> 8` → joyX (0x80 = center), `ly >> 8` → joyY

---

## VMU (Virtual Memory Unit) Sub-Peripheral

### Architecture
- Emulates a standard VMU (Visual Memory) at sub-peripheral address 0x01
- 256 blocks × 512 bytes = 128KB, stored in RP2040 flash at offset 0x001D8000
- Filesystem: system block (255), FAT (254), file info (241-253), save area (blocks 0-199)
- Auto-formats on first boot if flash area is blank or format version mismatch

### VMU Bug History

**Bug 1 (Fixed earlier): funcCode byte order.** RX decoder stored bytes in wire order; `uint32_t` casts were byte-reversed. FuncCode comparisons all failed → FILE_ERROR. Fixed by bswapping funcCode/location words. This was later superseded by a protocol-level fix that eliminated the need for bswap.

**Bug 2 (Fixed 2026-03-23): BLOCK_READ triple-bswap.** `sendBlockReadResponse()` added `__builtin_bswap32()` per word when copying block data. Combined with TX DMA bswap + DC RX DMA bswap = triple swap = one net swap = corrupted FAT. DC saw `0xFCFF` instead of `0xFFFC` for free blocks → "error writing" in MvC2. **Fix:** replaced bswap32 loop with `memcpy()`.

**Format version:** VMU_FORMAT_VERSION=4 stored at system block byte offset 17. Forces re-format on layout changes.

**For comprehensive VMU technical details, see [vmu_expert_reference.md](vmu_expert_reference.md).**

### Remaining VMU Issues
- **Flash write blocking:** `flash_range_erase` + `flash_range_program` disables interrupts for ~45ms. Brief controller dropout during saves. Acceptable tradeoff.
- **Save test pending:** Build with bswap fix ready (v0.7.12-nobd-14), needs real MvC2 save test.

---

## Build Commands

### Single Board (fast iteration)
```bash
powershell.exe -Command "cmd.exe /c 'C:\Users\trist\projects\GP2040-CE\build_one.bat'" 2>&1
```

### All 4 Boards (release)
```bash
powershell.exe -Command "cmd.exe /c 'C:\Users\trist\projects\GP2040-CE\build_nobd.bat'" 2>&1
```

---

## Key Lessons Learned

1. **PIO SMs must be fully restarted, not just re-enabled.** `pio_sm_set_enabled(true)` resumes from the frozen instruction. After disable→re-enable, SMs can be in any instruction, in any phase of edge detection. This caused ~12% packet loss (CP=8) that manifested as 1-3 Hz disconnect cycling.

2. **RX clock speed matters.** clkdiv=3.0 (~42MHz) added ~72ns latency to IRQ7 response, enough to cause marginal sampling at Maple Bus speeds. clkdiv=1.0 (125MHz) eliminates this.

3. **Bus line check prevents contention.** Without verifying idle before TX, our response could collide with the DC's tail sequence. 10µs continuous idle verification is the standard approach.

4. **Display latency was a red herring.** At 600-800µs loop time, display I2C draws were not the bottleneck. Rate-limiting to 10 FPS is still good practice but wasn't the fix.

5. **VMU was a red herring.** Disabling VMU entirely didn't fix cycling. The transport layer (PIO SM restart) was the real issue.

6. **CP (consecutive polls) diagnostic was the key insight.** Knowing the DC polled 8 times before re-probing pointed directly at a per-TX-cycle failure probability, not a latency or protocol issue.

7. **Expert analysis documents are invaluable.** The `curried-swimming-kahn.md` plan identified all critical bugs ranked by priority. BUG 1 (SM restart) turned out to be the primary root cause, exactly as predicted.

8. **DMA ring buffer prevents FIFO overflow.** The RX PIO produces ~16x more data than a hardware decoder. Without DMA, the 128-sample FIFO overflows during slow main loop iterations.

9. **TXSTALL is the correct TX completion signal.** PIO's FDEBUG TXSTALL sticky bit indicates the SM has stalled at `pull` with empty FIFO — meaning all data AND tail sequence are complete. No timing guesswork needed.

10. **Disconnecting the display is a valid diagnostic.** GP2040-CE auto-detects display presence via I2C scan at boot. No display = no display addon = eliminates that variable entirely.

11. **Double-DMA bswap = identity for block data.** TX DMA bswap + DC RX DMA bswap cancel out. Block data must be `memcpy()`'d raw — no software bswap. Adding bswap32 creates a triple-swap (one net swap = corrupted data). This was the root cause of the MvC2 "error writing" bug.

12. **Consistency matters: flip both or flip neither.** MaplePad flips (bswap) on BOTH write and read (NETWORK byte order in flash). Our code flips on NEITHER (native byte order in flash). Both produce zero net swaps = correct round trip. Flipping only one direction = corruption.

---

## External References

- **Maple Bus wire protocol:** http://mc.pp.se/dc/maplewire.html
- **Maple Bus commands/packets:** http://mc.pp.se/dc/maplebus.html
- **MaplePad reference:** `maplepad_ref/` directory in this repo
- **Expert bug analysis:** `C:\Users\trist\.claude\plans\curried-swimming-kahn.md`
- **GP2040-CE docs:** https://gp2040-ce.info/
- **RP2040 PIO documentation:** https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf (Chapter 3)
