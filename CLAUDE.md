# GP2040-CE NOBD — Agent Context File

## What Is This Project?

A fork of GP2040-CE v0.7.12 that adds **NOBD (No OBD)** — a firmware-level sync window that groups near-simultaneous button presses so they arrive on the same USB frame. Built for fighting game players who need reliable dashes, throw techs, and multi-button inputs without resorting to OBD (One Button Dash) macros.

## The Problem

When you press LP+HP for a dash, your fingers are naturally 1-3ms apart. USB polls every 1ms. If those two presses land on opposite sides of a USB poll boundary, the game sees them on **different frames** — LP arrives alone, then HP next frame. Result: stray jab instead of dash.

## The Solution

The sync window holds the first press for a configurable number of ms (default 5ms) so the second press can catch up. Both get committed together on the same frame.

**Key property: zero added latency at 5ms.** Stock GP2040-CE already debounces every input at 5ms. NOBD replaces that debounce with a smarter sync window — same total delay, but it groups presses instead of just filtering bounce.

---

## Architecture — Where Things Live

### Core Sync Window
- **`src/gp2040.cpp`** — `debounceGpioGetAll()` (lines ~250-302)
  - The entire NOBD algorithm lives here
  - Static state machine: `sync_pending`, `sync_start`, `sync_new`
  - Releases are instant (never delayed)
  - All inputs (directions + buttons) go through the window
  - `debounceDelay == 0` → raw passthrough, no sync

### Configuration
- **`proto/config.proto`** — `debounceDelay` is field 11 in `GamepadOptions`
- **`src/config_utils.cpp`** — `DEFAULT_DEBOUNCE_DELAY` = 5 (lines ~125-127)
  - Initialized via `INIT_UNSET_PROPERTY` at line ~302
- **`www/src/Pages/SettingsPage.jsx`** — Web UI slider (lines ~1693-1706)
  - `min={0}` `max={5000}`, yup validation: `yup.number().required()`
  - No server-side clamping — 0 is a valid value

### Finger Gap Tester (Rust GUI)
All in **`tools/finger-gap-tester/`**:
- `src/main.rs` — Entry point, eframe window, dark theme with teal accent
- `src/app.rs` — Two-tab UI: Gap Tester + Button Monitor
- `src/input.rs` — **Dedicated 8kHz polling thread** (not on UI thread). Processes one ButtonPressed per gilrs poll cycle for accurate timestamps. Sends events to UI via mpsc channel.
- `src/stats.rs` — Gap statistics, histogram, recommendation formula: `max(3, ceil(avg) + 1)`
- `src/monitor.rs` — Per-button hold duration, repress timing, activation stats

Dependencies: `eframe` 0.33, `egui` 0.33, `egui_plot` 0.34, `gilrs` 0.11

### Finger Gap Tester (Python)
- **`test_finger_gap.py`** — Lightweight alternative using pygame. Same 50ms pair window, same stats.

### Build System
- **`build_fw.bat`** — Windows build script (MSVC + Ninja + CMake)
- Board config: `GP2040_BOARDCONFIG=RP2040AdvancedBreakoutBoard`
- Output: `build/GP2040-CE-NOBD_0.7.12_RP2040AdvancedBreakoutBoard.uf2`

---

## How the Sync Window Works

```
1. New press detected (raw_gpio bit goes 0→1)
2. If no window open: start window, record press in sync_new bitmask
3. If window already open: accumulate press into sync_new
4. Every cycle: sync_new &= raw_gpio (drop any press that was released = bounce filtering)
5. When (now - sync_start) >= debounceDelay: commit sync_new to debouncedGpio
6. Releases ALWAYS apply immediately (gamepad->debouncedGpio &= ~just_released)
```

The bounce filtering is key — it's not "wait 5ms and hope bounce settled." The code continuously validates pending presses against actual GPIO state. If a switch bounces off then back on, the momentary off gets caught by step 4.

---

## Design Decisions (and why)

1. **5ms default, not 8ms** — Testing showed natural finger gaps are 1-3ms. 5ms covers virtually everyone. Same as stock debounce = zero added latency.

2. **Directions go through sync window** — Originally bypassed for "zero latency directions." Removed because QCB+KK would desync: direction arrives instant, buttons delayed by window → game sees wrong move.

3. **Releases are instant** — Charge characters (Megaman, Sentinel) need immediate release detection. The sync window only applies to presses.

4. **Input thread at 8kHz** — gilrs on Windows uses XInput (polling API, no event timestamps). Processing all events on the UI thread gave 0ms gaps (tight loop) or ~4ms gaps (frame-rate limited). Dedicated thread with 0.125ms sleep matches Python's accuracy.

5. **Recommendation based on average, not max** — One slow outlier shouldn't inflate the recommendation. `max(3, ceil(avg) + 1)` gives 1ms headroom above typical gap, floor of 3ms.

6. **OBD detection** — If >50% of measured gaps are <0.1ms, the user likely has OBD or a macro button active. Warn them to turn it off for accurate measurement.

---

## Building

### Firmware
```powershell
# From repo root on Windows with MSVC installed:
.\build_fw.bat
# Or manually:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGP2040_BOARDCONFIG=RP2040AdvancedBreakoutBoard -DPICO_SDK_FETCH_FROM_GIT=on
cmake --build build
```

### Web UI (must rebuild before firmware if you changed www/)
```powershell
cd www
npm install
npm run build
cd ..
# Then rebuild firmware — web assets get embedded via makefsdata
```

### Finger Gap Tester
```powershell
cd tools/finger-gap-tester
cargo build --release
# Output: target/release/finger-gap-tester.exe
```

### Release Upload
```powershell
gh release upload v0.7.12-NOBD build/GP2040-CE-NOBD_*.uf2 --clobber
gh release upload v0.7.12-NOBD tools/finger-gap-tester/target/release/finger-gap-tester.exe --clobber
```

---

## Files Changed from Stock GP2040-CE v0.7.12

| File | What Changed |
|------|-------------|
| `src/gp2040.cpp` | Replaced `debounceGpioGetAll()` with sync window algorithm |
| `src/config_utils.cpp` | `DEFAULT_DEBOUNCE_DELAY` remains 5 (unchanged, but worth noting) |
| `README.md` | Complete rewrite for NOBD documentation |
| `.gitignore` | Added `tools/finger-gap-tester/target/` |
| `test_finger_gap.py` | New file — Python gap tester |
| `tools/finger-gap-tester/` | New directory — Rust GUI gap tester |

**Everything else is stock GP2040-CE v0.7.12.** The sync window is entirely self-contained in `debounceGpioGetAll()`.

---

## Common Tasks

### Change the default sync window value
Edit `src/config_utils.cpp`, change `DEFAULT_DEBOUNCE_DELAY`. Rebuild firmware.

### Change the recommendation formula
Edit `tools/finger-gap-tester/src/stats.rs` → `recommended_nobd()` and `test_finger_gap.py` → bottom section.

### Add a new board
Copy an existing config from `configs/`, modify pin mappings. Build with `-DGP2040_BOARDCONFIG=YourBoardName`.

### Change the web UI label for the setting
Edit `www/src/Locales/en/SettingsPage.jsx` — find the debounce delay translation key.

---

## Expert Domain Knowledge

For deep context on fighting game mechanics, game engine input systems, and RP2040 firmware internals, see **[`docs/NOBD-EXPERT-CONTEXT.md`](docs/NOBD-EXPERT-CONTEXT.md)**. This covers:

- **Fighting Game Expert** — simultaneous inputs, frame data, OBD debate, platform-specific input behavior (PC, PS4/5, Xbox, Switch, Dreamcast, MiSTer), plinking history, negative edge
- **Game Dev Expert** — frame boundary problem in detail, input pipelines, sub-frame reading, rollback netcode implications, input priority systems
- **Firmware Expert** — RP2040 hardware, GPIO reading, switch bounce physics, USB HID reports, SOCD cleaning pipeline, timing functions, config mode, flash storage

Load this file when making decisions that touch game mechanics, input timing, or hardware behavior.

---

## Release Info
- **Current release:** v0.7.12-NOBD
- **Repo:** https://github.com/t3chnicallyinclined/GP2040-CE-NOBD
- **Assets:** `.uf2` firmware + `finger-gap-tester.exe`
- **56 board configs** available in `configs/` (only RP2040AdvancedBreakoutBoard has pre-built .uf2)
