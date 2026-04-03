# MapleCast Architecture — What We Learned

## Principles

```
OVERKILL IS NECESSARY
OPTIMIZATION IS KEY
SIMPLICITY
ALL WE THINK ABOUT IS MVC2 PERFORMANCE AND LOWER LATENCY
```

---

## The Problem We Solved

Online Dreamcast fighting games (MVC2) need zero desync and lowest possible latency. Every existing solution (Fightcade/GGPO, delay-based) runs TWO copies of the game and tries to keep them in sync. They all fail in subtle ways — rollback visual artifacts, desync from emulator non-determinism, input prediction errors.

**Our solution: don't run two copies.** Run ONE game. Both players feed inputs into it. Stream the result.

---

## What We Tried and Why We Abandoned It

### Attempt 1: Relay Server + Two Flycast Instances (FAILED)
```
Gamepad A → Flycast A ─→ Relay Server ←─ Flycast B ← Gamepad B
                              ↓
                    tick = {P1 inputs, P2 inputs}
                              ↓
                    both Flycast advance one frame
```

**Why it failed:**
- Two emulator instances are NOT deterministic even with identical inputs
- Sources of drift: RTC (wall clock), FMA (floating point), SH4 clock speed, frame boundaries, thread scheduling
- We patched each one (fixed RTC, locked clock, disabled FMA, added endOfFrame) and it STILL drifted
- Fundamental problem: emulator determinism is a losing battle

**Lesson: If you need two things to be identical, use ONE thing.**

### Attempt 2: GGPO Determinism Hacks (FAILED)
- Set `config::GGPOEnable = true` to get GGPO's determinism flags
- Caused side effects: `ggpo::nextFrame()` broke the frame loop, save state loading blocked by `dc_savestateAllowed()` checking `settings.network.online`
- Kept peeling back layers of Flycast internals finding more entanglement

**Lesson: Don't fight the codebase. Change the architecture.**

### Attempt 3: Flycast IS the Server (WORKS)
```
Gamepad A → UDP:7101 → Flycast (runs MVC2) ← UDP:7102 ← Gamepad B
                              ↓
                         one screen
                         one state
                         zero desync (by definition)
```

**Why it works:**
- ONE game state. Nothing to sync. Nothing to drift.
- Inputs arrive over UDP, get written to `mapleInputState[]` inside `maple_DoDma()`
- Game reads those inputs, advances one frame, renders
- Both players' inputs affect the same game simultaneously
- Total code: ~120 lines in maplecast.cpp

---

## The Working Architecture

### Components
```
pc_gamepad_sender.py     → reads gamepad via pygame, sends 4-byte W3 packets at 120Hz
  ↓ UDP
maplecast-flycast        → Flycast fork, receives UDP on port 7101 (P1) and 7102 (P2)
  ↓ renders
screen / WebRTC stream   → both players see the same game (future: browser via WebRTC)
```

### The Code (entire netplay injection — 4 files, ~15 lines changed in Flycast)

**maplecast.cpp (NEW — ~120 lines):**
- `init(p1Port, p2Port)` — creates two non-blocking UDP sockets
- `getInput(mapleInputState)` — drains all pending packets, keeps latest per player, writes to `mapleInputState[]`
- `drainLatest()` — non-blocking recv loop, keeps freshest W3 packet
- `w3ToInput()` — converts 4-byte W3 format to Flycast's `MapleInputState`

**maple_if.cpp (2 lines changed):**
```cpp
// Line 156-157: inject our inputs instead of GGPO's
if (maplecast::active())
    maplecast::getInput(mapleInputState);
else
    ggpo::getInput(mapleInputState);
```

**emulator.cpp (6 lines changed):**
```cpp
// In Emulator::start(): MAPLECAST=1 env var enables server mode
if (!maplecast::active() && std::getenv("MAPLECAST"))
    maplecast::init(p1Port, p2Port);
```

**nullDC.cpp (2 lines changed):**
```cpp
// Allow save states when MapleCast is active
if (std::getenv("MAPLECAST_SERVER"))
    return !settings.content.path.empty();
```

### W3 Packet Format (from pc_gamepad_sender.py)
```
4 bytes, big-endian:
  [0] Left Trigger  (0-255)
  [1] Right Trigger  (0-255)
  [2] Buttons high byte (active-low: 0=pressed)
  [3] Buttons low byte

Button bits (DC standard):
  0x0001 = C    0x0002 = B    0x0004 = A    0x0008 = Start
  0x0010 = Up   0x0020 = Down 0x0040 = Left 0x0080 = Right
  0x0100 = Z    0x0200 = Y    0x0400 = X
```

### How Inputs Flow
```
pygame reads USB gamepad
  → build_w3() creates 4-byte packet
  → sendto(UDP) to Flycast port 7101 or 7102
  → Flycast's non-blocking socket receives it
  → drainLatest() keeps the freshest packet
  → maple_DoDma() fires (once per frame, at vblank)
  → maplecast::getInput() writes W3 data to mapleInputState[]
  → game reads mapleInputState[] via config->GetInput()
  → game processes inputs (character moves, attacks, etc.)
  → game renders frame
```

---

## What Flycast Does Internally (for the record)

### Frame Lifecycle
1. `getSh4Executor()->Run()` — SH4 CPU executes MVC2 game code
2. SH4 scheduler fires events at exact cycle counts (deterministic)
3. At vblank scanline: `maple_vblank()` → `maple_DoDma()` → reads `mapleInputState[]`
4. Game processes inputs, updates game state, submits geometry to Tile Accelerator
5. Game writes STARTRENDER register → `rend_start_render()` queues TA context
6. Renderer calls `Process()` → `Render()` → `Present()` → pixels on screen

### Key Data Structures
- `mapleInputState[4]` — global input state per player (buttons, triggers, analog)
- `MapleInputState.kcode` — button bitmask (active-low, DC standard)
- `MapleInputState.halfAxes[]` — triggers (LT, RT)
- `MapleInputState.fullAxes[]` — analog sticks

### Why Emulator Determinism Failed
- `GetRTC_now()` returns `time(NULL)` — different between instances
- FMA instructions produce different float results than multiply+add
- SH4 clock speed configurable — different cycle counts per block
- Frame boundaries defined by renderer timing — wall-clock dependent
- Threaded rendering introduces scheduling non-determinism
- GGPO addresses all of these but its fixes are deeply entangled with its rollback system

---

## Latency Analysis (MVC2 specific)

### Current (local, one machine)
```
Gamepad press:                        0ms
pygame captures + UDP send:           ~1ms
Flycast receives (non-blocking):      ~0ms (already in buffer)
Wait for next maple_DoDma:            0-16ms (vblank timing)
Game processes input:                 within same frame
Render:                               ~2ms
Display:                              ~0ms (already on local monitor)
                                      ────
Total:                                3-19ms (avg ~10ms = under 1 frame)
```

### Future (remote, WebRTC streaming)
```
Gamepad press:                        0ms
pc_gamepad_sender.py:                 ~1ms
UDP to server:                        1-3ms (same city)
Flycast processes + renders:          ~16ms (one frame)
GetLastFrame():                       ~1ms (480p readback)
H.264 encode (NVENC on 3090):        ~0.5ms (480p is trivial)
WebRTC transport:                     1-3ms (same city)
Browser decode + display:             ~1-2ms
                                      ────
Total:                                5.5-26ms
Average:                              ~12ms = under 1 frame
Same city worst case:                 ~26ms = 1.5 frames
```

### For comparison
```
Fightcade (GGPO rollback):           3-5 frames typical (50-83ms)
Delay-based netcode:                 3-4 frames (50-67ms)
MapleCast (projected):               under 2 frames worst case
```

---

## Next Steps

### Phase 1: WebRTC Streaming (enable remote play)
- Add `GetLastFrame()` capture after each render
- H.264 encode via NVENC (3090)
- WebRTC output via GStreamer webrtcbin or native WebRTC
- Browser client connects to URL, sees the game
- `pc_gamepad_sender.py` already works over network (just change IP)

### Phase 2: GP2040-CE Integration
- GP2040-CE with W6100 sends W3 packets directly to server
- Replaces pc_gamepad_sender.py — hardware gamepad → network, no PC needed
- Same UDP protocol, same 4 bytes, same ports
- Player can be on real Dreamcast hardware OR watching WebRTC stream

### Phase 3: Production Infrastructure
- Server with GPU in data center (3090/4090)
- Matchmaking service
- Multiple game sessions per server
- Input relay for players behind NAT
- CDN for WebRTC signaling

---

## Repository Map
```
github.com/t3chnicallyinclined/
├── maplecast-flycast/          ← Flycast fork, GPL-2.0+ (public)
│   branch: maplecast
│   core/network/maplecast.cpp  ← THE module (~120 lines)
│
├── maplecast-server/           ← Relay server, MIT (private, Rust)
│   (Phase 1 artifact — may repurpose for matchmaking)
│
└── GP2040-CE/                  ← Fightstick firmware, MIT
    tools/pc_gamepad_sender.py  ← Gamepad → UDP sender
    docs/MAPLECAST-ARCHITECTURE.md ← This document
```

---

## The Principle

We tried complex. Complex failed. Simple works.

```
Don't sync two games. Run one game.
Don't predict inputs. Wait for them.
Don't fix desync. Eliminate it.
Don't stream frames. Stream inputs (for now).
Don't fight the emulator. Use it as-is.
```

One Flycast. Two UDP sockets. 120 lines of C++. MVC2 online.
