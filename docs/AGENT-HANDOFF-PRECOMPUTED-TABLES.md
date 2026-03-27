# Agent Handoff: Fully ISR-Driven Dreamcast Maple Bus + Pre-Computed Tables

## Status: APPROVED — Ready for Implementation

## What's Done (Committed & Working)

### Commit `af044668` — CMD9 lookup table + ISR fast path
- `buildCmd9LookupTable()` — dynamically allocated W3 table indexed by GPIO state
- `updateCmd9FromGpio()` — runs every tight loop cycle
- ISR arms via `setFastPath(true)` at init
- ISR PIO IRQ bug fixed (clear flag for CMD9, disable NVIC for non-CMD9)

### Commit `9f491132` — Direct PIO FIFO TX + XF fix
- ISR stuffs 6 words directly into PIO TX FIFO — no DMA, no bus idle check
- `startRx()` waits 10µs bus idle before enabling RX SM
- ZL tight loop skips `pollReceive()` when no ISR fallthrough pending
- **Response time: 1-2µs, XF: 0**

## What's Next: Full ISR Architecture

### Goal
Handle ALL Maple Bus commands from ISR. No STD/ZL mode distinction. Core 0 completely free from Maple polling.

### Zero-Computation Strategy
- Port address cached after CMD1 → all headers + CRCs pre-computed with real port
- `rebuildAllPacketsForPort(port)` called once after first CMD1
- CMD9: restore W5 (CRC) in lookup table. Digital-only = zero ISR math. Analog = `crcPartial` (~3 cycles)
- All static responses: ISR points to pre-built buffer, fires FIFO or DMA. Zero computation.

### ISR Command Dispatch
| CMD | Response | Words | Method | Pre-built? |
|-----|----------|-------|--------|------------|
| 1 (DEVICE_REQUEST) | Device info | 31 | DMA | Yes — static at init |
| 2 (ALL_STATUS_REQUEST) | Extended info | 51 | DMA | Yes — static at init |
| 3/4 (RESET/SHUTDOWN) | ACK | 3 | FIFO | Yes — static |
| 9 (GET_CONDITION) | Controller state | 6 | FIFO | Yes — lookup table |
| 10 (GET_MEDIA_INFO) | Media info | 10 | DMA | Yes — static after format |
| 11 (BLOCK_READ) | Block data | 134 | DMA | Partial — memcpy 512B from XIP |
| 12 (BLOCK_WRITE) | ACK | 3 | FIFO | Yes + memcpy 128B payload |
| 13 (COMPLETE_WRITE) | ACK | 3 | FIFO | Yes + flag Core 1 flash |
| 14 (SET_CONDITION) | ACK | 3 | FIFO | Yes — rumble ignored |

### Flash Write Guard
CMD11 during `flash_range_erase/program` → ISR sends FILE_ERROR. DC retries after Core 1 finishes.

### Full plan at:
`C:\Users\trist\.claude\plans\lazy-prancing-lake.md`

## Implementation Order
1. `maple_bus.h/cpp` — ISR helpers (isrSendFifo, isrSendDma), generalized CRC, FIFO drain
2. `DreamcastDriver.h/cpp` — full ISR dispatch, pre-build all buffers, rebuildAllPacketsForPort
3. `DreamcastVMU.h/cpp` — pre-build VMU buffers, ISR-safe VMU commands
4. `gp2040.cpp` — remove ZL tight loop, simplify main loop
5. `display.cpp` — remove mode toggle
6. Build + test incrementally

## Key Files
| File | What |
|------|------|
| `src/drivers/dreamcast/maple_bus.cpp` | ISR handler, PIO/DMA transport |
| `headers/drivers/dreamcast/maple_bus.h` | MapleBus class, flags, helpers |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Command dispatch, lookup table, process() |
| `headers/drivers/dreamcast/DreamcastDriver.h` | Packet buffers, lookup tables |
| `src/drivers/dreamcast/DreamcastVMU.cpp` | VMU command handling, flash |
| `headers/drivers/dreamcast/DreamcastVMU.h` | VMU buffers, constants |
| `src/gp2040.cpp` | Main loop, ZL tight loop |
| `src/addons/display.cpp` | S1/S2 toggle, diagnostics |
| `docs/EXPERT-CONTEXT.md` | Full protocol/architecture reference |

## Future: USB Output Mode Tables
After DC is fully ISR-driven, extend lookup table pattern to XInput, DInput, Switch, PS4. Same index (13-bit compressed button state), different output report format per mode. One table in RAM at a time.

## Design Principles
- Pre-compute everything possible at init time
- Zero computation in the ISR hot path (CMD9 especially)
- Port cached after CMD1 — rebuild all packets once with correct port+CRC
- Only CMD11 (block read) requires runtime computation (memcpy + CRC from flash)
