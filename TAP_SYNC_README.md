# GP2040-CE v0.7.12 — Tap Sync Window Fork

## Why This Fork Exists

**The goal is simple: make modern USB controllers feel like Dreamcast.**

On Dreamcast, dashing (LP+HP) in MVC2 just *worked*. You pressed two buttons at roughly the same time and the game read them together — every time. On modern platforms with USB controllers, that same input feels inconsistent. Sometimes you dash, sometimes you get a stray punch. The hardware changed, and it broke something that used to be effortless.

This fork replaces the default debounce logic in GP2040-CE with a **tap sync window** that recreates the Dreamcast's natural input grouping behavior. The result: simultaneous button presses feel consistent again, the way they did on original hardware.

## Why Dashing Felt Better on Dreamcast

On the original Dreamcast hardware, the Maple Bus protocol polls controllers **once per frame at 60Hz** — synced directly to VBlank (the vertical refresh interval). That means the console only checks your button states once every ~16.67ms. If you press LP and HP anywhere within that 16.67ms frame window, *both* buttons appear simultaneously to the game. The hardware itself acts as a natural sync window. You didn't need perfect timing — you just needed both buttons down before the next frame poll.

### Modern USB: Too Fast for Its Own Good

Modern USB controllers like GP2040-CE poll at **1000Hz (every 1ms)** — that's 1,000 polls per second, roughly 16 separate snapshots per single game frame. This is great for raw latency (sub-1ms input response), but it creates a problem for simultaneous presses:

```
Dreamcast (60Hz polling — once per frame):
  Frame N: [───────────16.67ms────────────]
           LP pressed ──┐    ┌── HP pressed
                        Both captured in same frame poll ✓

USB 1000Hz (polled every 1ms):
  Frame N: [─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─]
              │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │
              LP pressed     HP pressed (3ms later)
              ↑              ↑
              Poll #3        Poll #6
              Game sees LP   Game sees LP+HP
              alone first    (too late — already read LP only)
```

With 1000Hz USB, if your fingers are even 2-3ms apart, the controller reports LP *without* HP on several polls before HP arrives. Whether the game sees this as two separate inputs depends on when it reads the controller state relative to those polls — but the odds of a split read are **much higher** than on Dreamcast.

### The Math

| Platform | Poll Rate | Polls Per Frame | Window for "Simultaneous" |
|----------|-----------|-----------------|---------------------------|
| Dreamcast (Maple Bus) | 60 Hz | 1 | ~16.67ms (entire frame) |
| USB (modern controllers) | 1000 Hz | ~16 | ~1ms (single poll) |

On Dreamcast, you had a **16.67ms window** where any two presses would land together. On USB, that window shrinks to **~1ms** — about 16x harder to hit. That's why dashing felt effortless on Dreamcast but inconsistent on modern platforms, even though the controller is technically "faster."

### What This Fork Does — Bringing Back Dreamcast Feel

The Dreamcast didn't have special input code for simultaneous presses — it just polled slowly enough that your fingers didn't need to be perfect. Modern USB controllers are too fast and too precise, exposing the tiny gap between your two fingers as two separate inputs.

This fork adds a **tap sync window** that recreates what Dreamcast hardware did naturally. When a new button press is detected, the firmware holds it for a short configurable window (~8ms by default) before reporting it, giving a second near-simultaneous press time to arrive. Both are then committed together on the same poll — exactly like Dreamcast's once-per-frame read would have captured them.

**We're not adding anything new. We're restoring what the original hardware gave us for free.**

Critically, this **only affects new presses**:
- **Releases** (letting go of a button): Instant, zero delay — charge moves (Megaman buster, Sentinel drones, etc.) work perfectly
- **Holds** (keeping a button down): Completely unaffected — once committed, a button stays held indefinitely
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
- Set **Tap Sync Window** slider:
  - **0 ms** = raw passthrough, no sync (default 1000Hz behavior)
  - **~8 ms** = recommended for fighting games (MVC2, SF6, etc.)
  - **10-12 ms** = if you still get occasional dropped simultaneous presses
  - Keep it as low as works for you — higher values add latency to initial presses

## Files Changed

### `src/gp2040.cpp` — Core sync logic
Replaced the `debounceGpioGetAll()` function. The original was a simple frame-rate throttle. The new version:
1. Detects truly new presses (`raw & ~prev & ~sync_new`)
2. Passes releases through instantly (`gamepad->debouncedGpio &= ~just_released`)
3. Accumulates new presses into a sync buffer (`sync_new |= just_pressed`)
4. Commits all buffered presses when the window expires (`gamepad->debouncedGpio |= sync_new`)

### `www/src/Locales/en/SettingsPage.jsx` — UI label
Renamed "Debounce Delay in milliseconds" to "Tap Sync Window in milliseconds" in the web UI settings page.

### `build_fw.ps1` — Build script
PowerShell build script for Windows. Sets up the MSVC environment, overrides the `GP2040_BOARDCONFIG` env var (which otherwise overrides CMake `-D` flags), and builds with Ninja.

## Building

```powershell
# From the GP2040-CE directory
powershell -ExecutionPolicy Bypass -File build_fw.ps1
```

Output: `build/GP2040-CE_0.7.11_RP2040AdvancedBreakoutBoard.uf2`

## Flashing

1. Unplug the board
2. Hold BOOTSEL and plug in via USB
3. Copy the `.uf2` file to the `RPI-RP2` drive that appears
4. Board auto-reboots with new firmware

A pre-built `.uf2` is available on the [Releases page](https://github.com/t3chnicallyinclined/GP2040-CE-v7.1.1-sync/releases).

## Board

Built for **RP2040 Advanced Breakout Board**. To build for a different board, change `RP2040AdvancedBreakoutBoard` in `build_fw.ps1` to your board's config directory name under `configs/`.

## References

- [Dreamcast Maple Bus Wiki](https://dreamcast.wiki/Maple_bus) — Maple Bus protocol details, 60Hz VBlank-synced polling
- [Maple Bus Wire Protocol](http://mc.pp.se/dc/maplewire.html) — Low-level timing and signaling
- [GP2040-CE FAQ](https://gp2040-ce.info/faq/faq-general/) — 1000Hz USB polling, sub-1ms latency
- [MVC2 Arcade vs Dreamcast Differences](https://archive.supercombo.gg/t/mvc2-differences-between-arcade-version-dreamcast-version/142388) — Version comparison discussion
- [Polling Rate & Input Lag Guide](https://gamepadtest.app/guides/polling-rate-overclocking) — Modern controller polling explained
