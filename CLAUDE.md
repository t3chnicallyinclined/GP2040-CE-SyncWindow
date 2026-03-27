# GP2040-CE NOBD — Agent Context File

## What Is This Project?

A fork of GP2040-CE v0.7.12 adding two major features:

1. **NOBD (No OBD)** — firmware-level sync window that groups near-simultaneous button presses so they arrive on the same USB frame. Built for fighting game players who need reliable dashes, throw techs, and multi-button inputs without resorting to OBD (One Button Dash) macros.

2. **Full ISR-Driven Dreamcast Maple Bus** — ALL controller commands handled from PIO end-of-packet interrupt. Pre-computed lookup table for CMD9 (1-2µs response). Zero main loop overhead for Maple Bus during gameplay. See `docs/EXPERT-CONTEXT.md` section 5 for full architecture.

**Repo:** https://github.com/t3chnicallyinclined/GP2040-CE-NOBD
**Current release:** v0.7.12-nobd-19
**Boards:** RP2040AdvancedBreakoutBoard, Pico, PicoW (RP2040 only — Pico2/RP2350 untested, removed from releases)

For anything not NOBD-specific (pin mapping, addons, SOCD, display, USB modes, etc.), refer to the upstream GP2040-CE docs: https://gp2040-ce.info/

---

## Architecture — Where NOBD Lives

### Sync Window (`src/gp2040.cpp`)
- **`syncGpioGetAll()`** (~line 288) — the NOBD algorithm
  - Static state machine: `sync_pending`, `sync_start_us`, `sync_new`
  - Reads raw GPIO directly, writes `gamepad->debouncedGpio`
  - Releases are always instant (never delayed)
  - Built-in bounce filtering via `sync_new &= raw_buttons` every cycle
  - All presses wait the full sync window before committing (no instant fire)

- **`debounceGpioGetAll()`** (~line 251) — stock GP2040-CE per-pin debounce (unchanged)

### Main Loop Dispatch (`src/gp2040.cpp` ~line 360)
```cpp
if (Storage::getInstance().getGamepadOptions().nobdSyncDelay > 0) {
    syncGpioGetAll();   // NOBD mode
} else {
    debounceGpioGetAll(); // Stock mode
}
```
**Mutually exclusive** — they both write `debouncedGpio` and would conflict if both ran.

### Configuration Pipeline
| Layer | File | What |
|-------|------|------|
| Proto | `proto/config.proto` | `nobdSyncDelay` = field 34, `nobdReleaseDebounce` = field 35 in `GamepadOptions` |
| Default | `src/config_utils.cpp` | `DEFAULT_NOBD_SYNC_DELAY = 5`, `nobdReleaseDebounce = false`, initialized via `INIT_UNSET_PROPERTY` |
| WebConfig API | `src/webconfig.cpp` | `readDoc`/`writeDoc` for `nobdSyncDelay` and `nobdReleaseDebounce` |
| Web UI | `www/src/Pages/SettingsPage.jsx` | Mode dropdown + value field + Release Debounce checkbox (NOBD mode only) |
| Translations | `www/src/Locales/en/SettingsPage.jsx` | Labels for input timing mode controls |

### Header
- **`headers/gp2040.h`** — declares `syncGpioGetAll()` and sync-related members

---

## How the Sync Window Works

```
1. New press detected (raw_gpio bit goes 0→1)
2. If no window open: start window, record press in sync_new bitmask
3. If window already open: accumulate press into sync_new
4. Every cycle: sync_new &= raw_buttons (drop any released press = bounce filtering)
5. When (now_us - sync_start_us) >= syncDelay_us → commit sync_new
6. Releases apply immediately by default (debouncedGpio &= ~just_released)
```

**Bounce filtering:** `sync_new &= raw_buttons` every cycle clears presses that flicker OFF during the window. By the time the window expires and commits, bouncing switches have settled.

### Release Debounce (opt-in via `nobdReleaseDebounce`)

When enabled, releases are buffered instead of applied instantly:
```
1. Button released → added to pending_release bitmask, timer starts
2. Every cycle: pending_release &= ~raw_buttons (cancel if button bounced back ON)
3. When timer >= syncDelay_us → commit pending releases
4. If all pending releases cancelled (bounce), timer resets
```

This prevents phantom re-presses caused by switch bounce on release. The bounce-back makes the button appear to have never released — cleaner than a lockout which blocks legitimate re-presses.

**Use case:** Rhythm games (Guitar Hero, Cadence of Hyrule, etc.) where release bounce causes phantom inputs. Not needed for fighting games where instant releases are preferred for negative edge and charge partitioning.

**Default: OFF.** Enabled via checkbox in web UI (only visible in NOBD mode).

---

## Web UI

The Settings page has a **mode dropdown** + **single value field**:

| Mode | Value Field | Config Used |
|------|-------------|-------------|
| Stock Debounce | Debounce Delay (ms), 0-5000 | `debounceDelay` |
| NOBD Sync Window | Sync Window (ms), 1-500 | `nobdSyncDelay` |

When NOBD mode is selected, a **Release Debounce** checkbox appears. This enables release bounce filtering using the same sync window timer. Recommended for rhythm games, off by default.

Switching to Stock sets `nobdSyncDelay = 0`. Switching to NOBD sets `nobdSyncDelay = 5` (default).
Both values are always stored in flash — switching modes preserves the other mode's setting.

### Changing the UI
1. Edit `www/src/Pages/SettingsPage.jsx` — the mode dropdown and value field (~line 1692)
2. Edit `www/src/Locales/en/SettingsPage.jsx` — translation labels
3. Rebuild: `cd www && npm run build && cd ..`
4. Then rebuild firmware (web assets get embedded via makefsdata)

---

## Building

### Prerequisites
- Windows with MSVC Build Tools 2022
- CMake + Ninja
- ARM GCC toolchain (arm-none-eabi-gcc 14.2)
- Node.js + npm (for web UI)

### Build All 4 Boards

**From a terminal (not from Claude Code's bash shell):**
```powershell
.\build_nobd.bat
```
This builds RP2040AdvancedBreakoutBoard, Pico, PicoW, Pico2 and copies UF2s to `release/`.

**From Claude Code's bash shell** (bash can't run .bat files directly — use full path via PowerShell):
```bash
powershell.exe -Command "cmd.exe /c 'C:\Users\trist\projects\GP2040-CE\build_nobd.bat'" 2>&1
```
Note: `cmd.exe /c build_nobd.bat` from bash often fails silently because the working directory isn't propagated. Always use the full absolute path to the batch file.

**Important:** The SDK's mbedtls submodule must be at v2.28.8 for PS4 driver compatibility:
```bash
cd build/_deps/pico_sdk-src/lib/mbedtls && git checkout v2.28.8
```

### Build Single Board (manual)
```powershell
# Set up MSVC environment first via vcvarsall.bat
set PICO_SDK_PATH=C:\Users\trist\projects\GP2040-CE\build\_deps\pico_sdk-src
set GP2040_BOARDCONFIG=RP2040AdvancedBreakoutBoard
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSKIP_WEBBUILD=on -DFETCHCONTENT_FULLY_DISCONNECTED=on
cmake --build build --config Release --parallel
```

### Web UI (must rebuild before firmware if www/ changed)
```powershell
cd www
npm install
npm run build
cd ..
# Then rebuild firmware — web assets get embedded
```

### Release
```powershell
gh release create v0.7.12-nobd-X release/GP2040-CE-NOBD_0.7.12_*.uf2 --title "Title" --notes "Notes"
```

---

## Files Changed from Stock GP2040-CE v0.7.12

| File | What Changed |
|------|-------------|
| `src/gp2040.cpp` | NOBD sync window, DC mode: `updateCmd9FromGpio()` + `updateAnalogFromGamepad()` (ISR handles Maple Bus) |
| `headers/gp2040.h` | Added `syncGpioGetAll()` declaration |
| `proto/config.proto` | Added `nobdSyncDelay` field 34, `nobdReleaseDebounce` field 35, Dreamcast pin fields 36-37 |
| `src/config_utils.cpp` | Defaults for NOBD, Dreamcast pins |
| `src/webconfig.cpp` | WebConfig for NOBD + Dreamcast settings |
| `www/src/Pages/SettingsPage.jsx` | NOBD mode dropdown + Dreamcast pin config |
| `www/src/Locales/en/SettingsPage.jsx` | Translation labels for input timing + Dreamcast settings |
| `src/drivers/dreamcast/DreamcastDriver.cpp` | Full ISR command dispatch, pre-computed lookup table, `rebuildAllPacketsForPort()` |
| `headers/drivers/dreamcast/DreamcastDriver.h` | ISR dispatch, pre-built packet buffers, lookup table |
| `src/drivers/dreamcast/maple_bus.cpp` | ISR handler (`mapleRxIrqHandler`), `isrSendFifo()`, `isrSendDma()`, bus idle check in `startRx()` |
| `headers/drivers/dreamcast/maple_bus.h` | MapleBus ISR infrastructure, `MapleIsrCallback` |
| `src/drivers/dreamcast/DreamcastVMU.cpp` | VMU save/load, flash persistence |
| `headers/drivers/dreamcast/DreamcastVMU.h` | VMU constants, class |
| `src/drivers/dreamcast/maple.pio` | PIO TX/RX programs for Maple Bus |
| `src/addons/display.cpp` | DC display at full speed, S1 diagnostic toggle |
| `src/display/ui/screens/ButtonLayoutScreen.cpp` | DC diagnostic overlay, normal button layout in DC mode |
| ~~`test_finger_gap.py`~~ | Moved to [finger-gap-tester](https://github.com/t3chnicallyinclined/finger-gap-tester) repo |
| ~~`tools/finger-gap-tester/`~~ | Moved to [finger-gap-tester](https://github.com/t3chnicallyinclined/finger-gap-tester) repo |
| `README.md` | Rewrite for NOBD documentation |
| `.gitignore` | Added tool/build artifacts |

**Everything else is stock GP2040-CE v0.7.12.**

---

## Expert Domain Knowledge

For complete expert context — every subsystem, architecture, protocol details, byte order rules, debugging history, build/release workflow, web UI modification guide, and fighting game domain knowledge — see **[`docs/EXPERT-CONTEXT.md`](docs/EXPERT-CONTEXT.md)**.

This single file replaces the previously separate `docs/NOBD-EXPERT-CONTEXT.md`, `docs/dreamcast_maple_expert.md`, and `docs/vmu_expert_reference.md`.

---

## Common Tasks

### Change the default sync window value
Edit `src/config_utils.cpp`, change `DEFAULT_NOBD_SYNC_DELAY`. Rebuild firmware.

### Change the default debounce value
Edit `src/config_utils.cpp`, change `DEFAULT_DEBOUNCE_DELAY`. Rebuild firmware.

### Add a new board
Copy an existing config from `configs/`, modify pin mappings. Add to `build_nobd.bat`. Build with `GP2040_BOARDCONFIG=YourBoardName`.

### Change the web UI
Edit `www/src/Pages/SettingsPage.jsx` and `www/src/Locales/en/SettingsPage.jsx`. Run `cd www && npm run build`. Then rebuild firmware.
