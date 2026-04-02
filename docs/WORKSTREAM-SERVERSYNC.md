# GP-RETRO-ONLINE: ServerSync Workstream

## The One-Liner

Everything speaks Maple Bus. The server is a clock. The network is just a longer wire.

---

## Principles

1. **ONE PROTOCOL** — CMD9 everywhere. No translation. No abstraction. The 32 bytes from the GP2040-CE lookup table are the same 32 bytes in Flycast's DMA buffer.
2. **ONE CLOCK** — The server ticks. Clients advance when told. Both receive identical data. Desync is structurally impossible.
3. **ONE SOURCE OF TRUTH** — The CMD9 lookup table on each GP2040-CE. Real DC and Flycast are both just consumers of the same response.
4. **OVERKILL IS NECESSARY** — Sub-microsecond where we can get it. Batch syscalls. Zero-copy. Cache-line aligned. No "good enough."
5. **SIMPLIFICATION IS KEY** — No rollback. No speculation. No state correction. No checksums. Same inputs = same game. Period.

---

## Architecture

```
     Fightstick GPIO
          │
     GP2040-CE PIO → CMD9 lookup table (32 bytes, THE truth)
          │
          ├──→ Maple Bus PIO wire → Real Dreamcast (sub-1µs)
          │
          └──→ UDP (same 32 bytes) → Server
                                       │
                                  tick = {frame, p1_cmd9, p2_cmd9}
                                       │
                                 ┌─────┴─────┐
                                 ▼           ▼
                            Flycast A    Flycast B
                       MapleNetController::RawDma()
                         same bytes, same frame, same game
```

### Packet Format

```
CLIENT → SERVER (40 bytes):
  match_id:  u32       // which match
  frame:     u32       // current frame number
  cmd9:      u8[32]    // THE CMD9 response, verbatim from lookup table

SERVER → CLIENTS (68 bytes):
  frame:     u32       // authoritative frame number
  p1_cmd9:   u8[32]    // player 1's CMD9 response
  p2_cmd9:   u8[32]    // player 2's CMD9 response

Bandwidth: 68 bytes × 60fps = 4 KB/s per match
```

### Latency Budget

```
YOUR inputs → YOUR screen:
  PIO capture:          ~50ns
  CMD9 table write:     ~500ns
  UDP send:             ~10µs
  Server match + tick:  ~200ns + network RTT
  Flycast receives:     ~10µs
  SH4 emulates frame:  ~1ms
  GPU renders:          ~2ms
  ────────────────────────────
  Local overhead:       ~3.1ms (sub-frame, always)
  + network RTT (the only variable)

Same city:        ~4ms total  = 0.24 frames
Same region:      ~8ms total  = 0.48 frames
Cross-country:    ~43ms total = 2.6 frames
```

---

## Phase 0: The Server

**What:** UDP input mixer + frame clock. Not an emulator. ~100 lines of C.

**Input:** CMD9 payloads from both players.
**Output:** Timestamped tick with both CMD9 payloads.
**Logic:** Wait for both inputs → increment frame → broadcast tick.

### 0.1 — Core Server Loop

Single-threaded, non-blocking, batch I/O.

```c
#include <sys/socket.h>
#include <poll.h>

#define MAX_MATCHES 4096

typedef struct {
    uint32_t frame;
    struct sockaddr_in players[2];
    uint8_t  cmd9[2][32];       // CMD9 response per player
    uint8_t  ready;             // bitmask: bit 0 = P1, bit 1 = P2
    uint64_t last_tick_us;
    uint64_t last_input_us[2];
} Match;

typedef struct __attribute__((packed)) {
    uint32_t match_id;
    uint32_t frame;
    uint8_t  cmd9[32];
} ClientPacket;  // 40 bytes

typedef struct __attribute__((packed)) {
    uint32_t frame;
    uint8_t  p1_cmd9[32];
    uint8_t  p2_cmd9[32];
} ServerTick;  // 68 bytes

static Match matches[MAX_MATCHES];

void server_loop(int fd) {
    ClientPacket pkt;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    while (1) {
        int n = recvfrom(fd, &pkt, sizeof(pkt), 0,
                         (struct sockaddr*)&addr, &addrlen);
        if (n != sizeof(pkt)) continue;

        Match* m = &matches[pkt.match_id % MAX_MATCHES];
        int player = identify_player(m, &addr);
        if (player < 0) continue;

        memcpy(m->cmd9[player], pkt.cmd9, 32);
        m->ready |= (1 << player);
        m->last_input_us[player] = now_us();

        if (m->ready == 0x03) {
            m->frame++;
            ServerTick tick = { .frame = m->frame };
            memcpy(tick.p1_cmd9, m->cmd9[0], 32);
            memcpy(tick.p2_cmd9, m->cmd9[1], 32);

            sendto(fd, &tick, sizeof(tick), 0,
                   (struct sockaddr*)&m->players[0], sizeof(struct sockaddr_in));
            sendto(fd, &tick, sizeof(tick), 0,
                   (struct sockaddr*)&m->players[1], sizeof(struct sockaddr_in));

            m->ready = 0;
            m->last_tick_us = now_us();
        }
    }
}
```

### 0.2 — Match Registration / Lobby

Simple REST or UDP handshake:

```
Player A: REGISTER {match_id, player:0, name:"..."}
Player B: REGISTER {match_id, player:1, name:"..."}
Server:   MATCH_READY {match_id} → both players
          (now accepting CMD9 packets for this match)
```

Match ID can be a lobby code, a matchmaking result, or just agreed upon out-of-band. Keep it dead simple for v1 — two players agree on a match ID and server address.

### 0.3 — Adaptive Frame Pacing

```c
// Track rolling average of input arrival gap
float avg_gap_ms = rolling_avg(m->last_input_us);

// Server paces its tick rate based on the slowest player
// In tournament mode: skip this, always wait (60fps or freeze)
if (!tournament_mode && both_inputs_late_by(m, 5000)) {
    // Both players laggy — slow tick rate to match
    // Don't tick until inputs arrive, natural slowdown
}
```

The beauty: there's no explicit FPS control needed. The server ticks when both inputs arrive. If inputs arrive every 16ms → 60fps. If they arrive every 20ms → 50fps. If they arrive every 33ms → 30fps. **The frame rate IS the input arrival rate.** No separate pacing logic.

### 0.4 — Packet Redundancy

```
CLIENT → SERVER (extended, 104 bytes):
  match_id:  u32
  frame:     u32
  cmd9:      u8[32]      // current frame
  cmd9_prev: u8[32]      // previous frame (recovery)
  cmd9_prev2: u8[32]     // two frames ago (recovery)

SERVER → CLIENTS (extended, 200 bytes):
  frame:      u32
  p1_cmd9:    u8[32]     // current
  p2_cmd9:    u8[32]     // current
  p1_prev:    u8[32]     // previous (recovery)
  p2_prev:    u8[32]     // previous (recovery)
  p1_prev2:   u8[32]     // two ago (recovery)
  p2_prev2:   u8[32]     // two ago (recovery)
```

200 bytes × 60fps = 12 KB/s. Still nothing. Survives 2 consecutive dropped packets without retransmission.

### 0.5 — Match Replay Logging

```c
// After each tick, append to match log
// 68 bytes/tick × 60fps × 300sec = 1.2MB per 5-minute match
FILE* log = match_logs[m->match_id];
fwrite(&tick, sizeof(ServerTick), 1, log);
```

Every match automatically recorded. Replay by feeding the log into any Flycast instance. Frame-perfect reproduction. Provable tournament results.

### 0.6 — Connection Quality Broadcast

```c
typedef struct __attribute__((packed)) {
    uint32_t match_id;
    float    fps;               // actual tick rate
    float    p1_jitter_ms;      // P1 input arrival variance
    float    p2_jitter_ms;      // P2 input arrival variance
    float    rtt_estimate_ms;   // estimated RTT between players
    uint8_t  quality;           // 0-100 tournament grade score
} MatchQuality;

// Broadcast every 60 frames (once per second)
```

Both players see the same neutral quality assessment. Server is the impartial observer.

### Deliverable: Standalone server binary

```
serversync-server --port 7100 --max-matches 4096 --tournament
```

Test with two netcat/socat clients sending fake CMD9 payloads. Verify tick timing, measure jitter, validate redundancy recovery.

**Deploy:** Single binary. Any Linux box. Docker one-liner. Fly.io/Railway free tier.

---

## Phase 1: Flycast Fork — MapleNetController

**What:** A new Maple Bus device in Flycast that gets its CMD9 state from the network instead of from SDL/USB.

**Fork base:** [Flycast Dojo](https://github.com/blueminder/flycast-dojo) — already has netplay infrastructure, command-line config.

### 1.1 — MapleNetController Device

```
NEW FILES:
  core/hw/maple/maple_net_controller.h
  core/hw/maple/maple_net_controller.cpp
```

A `maple_device` subclass. Flycast's Maple Bus emulation calls `RawDma()` on it every frame during `maple_vblank()` → `maple_DoDma()`. It answers CMD9 (GETCOND) from a buffer that the network thread updates.

```cpp
class MapleNetController : public maple_device {
private:
    // THE CMD9 response — written by network, read by DMA
    // Same 32 bytes that GP2040-CE sends over PIO to a real DC
    alignas(64) uint8_t cmd9_response[32];
    std::atomic<uint32_t> cmd9_sequence{0};

public:
    MapleDeviceType get_device_type() override {
        return MDT_SegaController;
    }

    // Called by maple_DoDma() every frame
    u32 RawDma(u32* buffer_in, u32 buffer_in_len,
               u32* buffer_out, u32& buffer_out_len) override {

        u32 cmd = buffer_in[0] & 0xFF;

        switch (cmd) {
            case MDCMD_DevInfo:
                // Return standard DC controller device descriptor
                // Identical to what GP2040-CE returns for CMD1
                return returnDeviceInfo(buffer_out, buffer_out_len);

            case MDCMD_GetCondition:
                // THE HOT PATH — return CMD9 data from network
                memcpy(buffer_out, cmd9_response, 32);
                buffer_out_len = 32;
                return MDRS_DataTransfer;

            default:
                return MDRS_DeviceStatusAll;
        }
    }

    // Called by ServerSync network thread when tick arrives
    void updateCmd9(const uint8_t* data) {
        memcpy(cmd9_response, data, 32);
        cmd9_sequence.fetch_add(1, std::memory_order_release);
    }
};
```

**Key point:** `RawDma()` is the exact same interface that Flycast uses for local controllers. The Maple Bus emulation layer doesn't know this controller is networked. It just calls `RawDma()`, gets CMD9 data, DMAs it to the SH4. Same path as a local USB gamepad, but the data came from across the internet via a GP2040-CE.

### 1.2 — ServerSync Network Module

```
NEW FILES:
  core/network/serversync.h
  core/network/serversync.cpp
```

Manages the UDP connection to the relay server. Receives ticks, updates MapleNetControllers, controls frame advance.

```cpp
namespace ServerSync {
    struct Config {
        std::string serverAddr;
        uint16_t serverPort;
        uint32_t matchId;
        uint8_t localPlayer;     // 0 or 1
        bool tournamentMode;
    };

    bool init(const Config& cfg);
    void shutdown();

    // THE FRAME CLOCK — blocks until server tick arrives
    // Returns false on disconnect
    bool waitForTick();

    // Send local CMD9 to server
    void sendLocalInput(const uint8_t* cmd9, size_t len);

    // Status
    bool active();
    uint32_t currentFrame();
    float currentFps();
    float rttMs();
}
```

The `waitForTick()` implementation:

```cpp
bool ServerSync::waitForTick() {
    // 1. Send our CMD9 to server
    ClientPacket pkt;
    pkt.match_id = config.matchId;
    pkt.frame = current_frame;
    getLocalCmd9(pkt.cmd9);  // from GP2040-CE or local gamepad
    sendto(sock, &pkt, sizeof(pkt), 0, &server_addr, sizeof(server_addr));

    // 2. BLOCK until server tick arrives
    ServerTick tick;
    while (true) {
        struct pollfd pfd = { sock, POLLIN, 0 };
        int r = poll(&pfd, 1, config.tournamentMode ? 100 : 2000);

        if (r > 0) {
            int n = recvfrom(sock, &tick, sizeof(tick), 0, NULL, NULL);
            if (n >= sizeof(ServerTick) && tick.frame == current_frame + 1)
                break;
            continue;  // wrong frame or garbage, keep waiting
        }

        if (r == 0) {
            if (config.tournamentMode) return false;  // disconnect
            // casual: keep waiting, game freezes
        }
    }

    // 3. Update MapleNetControllers with synced CMD9 data
    uint8_t* local_cmd9 = (config.localPlayer == 0) ? tick.p1_cmd9 : tick.p2_cmd9;
    uint8_t* remote_cmd9 = (config.localPlayer == 0) ? tick.p2_cmd9 : tick.p1_cmd9;

    maple_net_controllers[0]->updateCmd9(local_cmd9);   // port 1 = local
    maple_net_controllers[1]->updateCmd9(remote_cmd9);  // port 2 = remote

    current_frame = tick.frame;
    return true;
}
```

### 1.3 — Frame Loop Integration

The surgical insertion point. Modify `Emulator::run()`:

```cpp
// core/emulator.cpp

void Emulator::run() {
    if (ServerSync::active()) {
        // ══════════════════════════════════════════
        // SERVER-CLOCKED MODE
        // We do NOT advance until the server says so.
        // Both players block here simultaneously.
        // Both receive identical CMD9 data.
        // Both run identical frames.
        // ══════════════════════════════════════════

        if (!ServerSync::waitForTick())  {
            // Connection lost — stop
            gui_display_notification("ServerSync: connection lost", 5000);
            stopEmulation();
            return;
        }

        // Maple DMA will read from MapleNetControllers
        // which were just updated by waitForTick()
        getSh4Executor()->Run();  // advance exactly one frame
        render();
        return;
    }

    // ... existing GGPO / local play paths unchanged ...
}
```

**Total diff to Flycast's existing code: ~5 lines in emulator.cpp.** Everything else is new files that don't touch existing code.

### 1.4 — GP2040-CE CMD9 as Local Input Source

For a player using a GP2040-CE stick connected locally (USB), we want to read its CMD9 response directly rather than going through SDL → gamepad mapping → Dreamcast input conversion.

**Option A (v1, works immediately):** GP2040-CE in USB gamepad mode → SDL reads it → Flycast's existing input mapper converts to DC format → we capture that as our CMD9 payload. Not pure, but works with zero GP2040-CE firmware changes.

**Option B (v2, pure Maple):** GP2040-CE firmware adds a mode: alongside USB HID, it sends raw CMD9 payloads over a second USB endpoint (CDC serial or vendor-specific bulk). A tiny shim reads this and passes it directly to ServerSync. Zero translation. The bits that leave the GP2040-CE's lookup table are the bits that enter Flycast's Maple DMA.

**Option C (v3, network only):** GP2040-CE with W6100 sends CMD9 directly to the server over UDP. No USB at all. The stick IS a network device. Flycast reads the CMD9 from the server tick. The stick doesn't even need to be plugged into the PC running Flycast — it can be on a different continent.

```
Option A:  Stick ──USB HID──→ Flycast SDL ──→ convert to CMD9 ──→ ServerSync
Option B:  Stick ──USB Bulk──→ raw CMD9 bytes ──→ ServerSync (zero translation)
Option C:  Stick ──Ethernet/UDP──→ Server ──→ ServerSync (stick is a network node)
```

Start with A. Ship with B. End state is C.

### 1.5 — Config & CLI

```bash
# Connect to a match
flycast --serversync \
        --server relay.gp-retro.online:7100 \
        --match 42069 \
        --player 1 \
        --tournament \
        game.gdi

# Or via config file
[serversync]
enabled = yes
server = relay.gp-retro.online
port = 7100
match_id = 42069
local_player = 1
tournament_mode = yes
```

### 1.6 — HUD Overlay

Minimal, non-intrusive. Shows during gameplay:

```
┌────────────────────────┐
│ F:12847  58.2fps  R:4ms│
└────────────────────────┘
  F = frame number (proves sync — both players should match)
  fps = actual tick rate from server
  R = estimated RTT to server
```

Both players see the same frame number. If they don't, something is catastrophically wrong (and it can't be, by construction).

### Deliverable: Flycast fork with ServerSync

Two Flycast instances on the same LAN, one server, same game. Verify:
- [ ] Both screens show identical gameplay
- [ ] Frame numbers match
- [ ] Inputs from one player visible on both screens
- [ ] Lag simulation: add artificial delay, both slow down equally
- [ ] Kill server: both freeze, reconnect resumes

---

## Phase 2: GP2040-CE ServerSync Integration

**What:** The GP2040-CE firmware learns to send CMD9 payloads to the relay server directly over W6100 Ethernet. No PC middleman needed.

### 2.1 — CMD9 UDP Broadcast from GP2040-CE

The lookup table already exists. `updateCmd9FromGpio()` builds it every PIO ISR. We just need to also send it out the W6100.

```c
// In the network send path (already exists for P2 injection):

void sendCmd9ToServer(void) {
    // cmd9_response[] already built by updateCmd9FromGpio()
    // Same bytes that go into the PIO TX FIFO for a real DC

    ClientPacket pkt;
    pkt.match_id = config.matchId;
    pkt.frame = current_frame;
    memcpy(pkt.cmd9, cmd9_response, 32);

    w6100_sendto(server_sock, &pkt, sizeof(pkt),
                 config.server_ip, config.server_port);
}
```

This runs AFTER the PIO ISR updates the lookup table, BEFORE the next DC poll. The W6100 sends the packet in ~50-100µs. The GP2040-CE doesn't wait for a response — it keeps answering DC polls from the local console while the packet flies to the server.

### 2.2 — Server Tick Reception on GP2040-CE

For cross-play (DC hardware vs Flycast), the GP2040-CE on the hardware side also receives the server tick and updates port 2's CMD9 table from it.

```c
void recvServerTick(void) {
    ServerTick tick;
    int n = w6100_recvfrom(server_sock, &tick, sizeof(tick));
    if (n != sizeof(tick)) return;

    // Update P2 CMD9 from server tick (remote player's inputs)
    uint8_t* remote = (local_player == 0) ? tick.p2_cmd9 : tick.p1_cmd9;
    updateCmd9FromNetwork(remote, 32);

    // P2's next CMD9 response will contain the remote player's buttons
    // DC polls P2, PIO ISR fires, sends remote player's inputs
    // Local DC is now playing against the remote player
}
```

**This is `updateCmd9FromNetwork()` — the function you already built for P2 injection.** Same interface. Same code path. The data just comes from the server tick instead of directly from the remote stick.

### 2.3 — Mode Detection

The GP2040-CE auto-detects its role:

```
IF P1 Maple cable connected AND P2 Maple cable connected:
    → Local mode, both ports physical, no network

IF P1 Maple cable connected AND server configured:
    → Online mode. P1 = local GPIO. P2 = server tick.
    → Send CMD9 to server. Receive tick. Inject P2.

IF no Maple cables AND server configured:
    → Pure network mode. Stick is an input-only network node.
    → Send CMD9 to server only. No Maple Bus output.
    → Flycast on a PC somewhere consumes the tick.
```

### Deliverable: GP2040-CE ↔ Server ↔ Flycast cross-play

Player A: Real Dreamcast + GP2040-CE → Maple Bus → CRT
Player B: Flycast on PC + GP2040-CE (USB or networked)
Both playing through the same server tick.
Same inputs. Same game. Different rendering hardware. Same result.

---

## Phase 3: Cross-Play Matrix

Every combination works because everything speaks CMD9:

```
┌──────────────────────────────────────────────────────────────────┐
│                    PLAYER A                                       │
│             DC+GP2040   │  Flycast+GP2040  │  Flycast+USB Pad    │
├──────────────┬──────────┼──────────────────┼────────────────────┤
│ DC+GP2040    │ ✓ Both   │ ✓ Cross-play     │ ✓ Cross-play       │
│              │ on real  │ DC vs Emu        │ DC vs Emu          │
│              │ hardware │                  │                    │
│ PLAYER  ├──────────┼──────────────────┼────────────────────┤
│         │ Flycast  │ ✓ Cross-play     │ ✓ Both on          │ ✓ Both on         │
│  B      │ +GP2040  │ Emu vs DC        │ emulator           │ emulator          │
│         ├──────────┼──────────────────┼────────────────────┤
│         │ Flycast  │ ✓ Cross-play     │ ✓ Both on          │ ✓ Both on         │
│         │ +USB Pad │ Emu vs DC        │ emulator           │ emulator          │
└─────────┴──────────┴──────────────────┴────────────────────┘

ALL paths go through the server. ALL use CMD9. ALL are synced.
```

---

## Phase 4: Tournament Infrastructure

### 4.1 — Match Replay System

Server already logs every tick. Replay = feeding the log into Flycast:

```bash
# Replay a match
flycast --replay match_42069.bin game.gdi

# Flycast reads ServerTick structs from file instead of network
# Same MapleNetController path, same rendering, frame-perfect reproduction
```

216KB per 5-minute match. Store every ranked/tournament match forever.

### 4.2 — Tournament Server Mode

```bash
serversync-server --tournament \
                  --max-matches 64 \
                  --timeout-ms 100 \
                  --log-dir /matches/ \
                  --quality-threshold 90
```

- `--tournament`: 60fps lock. No adaptive pacing. Late input = freeze, not slowdown.
- `--timeout-ms 100`: Disconnect after 100ms of no input. Connection must be solid.
- `--quality-threshold 90`: Reject match start if connection quality < 90/100.

### 4.3 — Match Verification

```
Tournament organizer: "Verify match 42069"

Server replays match_42069.bin through headless Flycast (norend)
  → produces state hash at final frame
  → state hash matches both players' reported final state
  → result is cryptographically provable

No screenshots. No honor system. Math.
```

### 4.4 — Connection Pre-Check

Before a tournament match starts:

```
Both players connect to server
Server runs 300 frames of ping-pong (5 seconds)
Measures: RTT, jitter, packet loss, FPS stability
Reports: "Connection quality: 94/100 — tournament grade"
Both players see the same score
Match proceeds or is flagged
```

---

## Phase 5: Overkill Optimizations

### 5.1 — recvmmsg/sendmmsg Batch I/O (Server)

```c
// Process 64 packets per syscall instead of 1
struct mmsghdr msgs[64];
int n = recvmmsg(fd, msgs, 64, MSG_DONTWAIT, NULL);
// Process all n packets
// Batch all outgoing ticks
int sent = sendmmsg(fd, out_msgs, out_count, 0);
```

### 5.2 — CPU Core Pinning (Server)

```c
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(0, &cpuset);  // pin to core 0
sched_setaffinity(0, sizeof(cpuset), &cpuset);

struct sched_param param = { .sched_priority = 99 };
sched_setscheduler(0, SCHED_FIFO, &param);  // real-time priority
```

### 5.3 — io_uring (Server, Linux 5.1+)

Replace poll/recvfrom/sendto with io_uring for zero-copy I/O:

```c
// Submit recv and send as linked SQEs
// Kernel processes them without context switches
// Completion events arrive in CQ ring
// Sub-microsecond per-packet processing
```

### 5.4 — GP2040-CE Direct UDP (No USB)

The end state. The fightstick is a network device. No PC needed for input.

```
Player A: Stick (W6100) ──Ethernet──→ Router ──→ Internet ──→ Server
Player B: Stick (W6100) ──Ethernet──→ Router ──→ Internet ──→ Server

Both sticks send CMD9 over UDP.
Server syncs and ticks.
If playing on real DC: GP2040-CE also speaks Maple Bus to local console.
If playing on Flycast: Flycast reads from server tick. Stick plugged into Ethernet only.
```

### 5.5 — Shared Memory Bridge (Local Flycast, Maximum Performance)

When GP2040-CE is connected to the PC running Flycast via USB:

```
GP2040-CE USB Bulk Endpoint
  → libusb async transfer (~125µs)
  → gp2040-bridge daemon writes to mmap'd region
  → Flycast MapleNetController reads from same mmap
  → Zero-copy, zero-syscall input path

Shared memory layout (one cache line, 64 bytes):
  sequence:  u32   (monotonic counter)
  cmd9:      u8[32] (THE response)
  _pad:      28 bytes (fill to cache line)
```

### 5.6 — Packet Redundancy (Both Sides)

Every packet carries last 3 frames. Survives 2 consecutive drops without retransmission.

```
CLIENT → SERVER: {frame, cmd9[3 frames]}     = 104 bytes
SERVER → CLIENT: {frame, p1_cmd9[3], p2_cmd9[3]} = 200 bytes

200 bytes × 60fps = 12 KB/s. Still nothing.
```

---

## Build Order

```
PHASE 0: Server                    ← CAN START IMMEDIATELY
  0.1  Core loop (C, single file)
  0.2  Match registration
  0.3  Test with fake clients
  0.4  Packet redundancy
  0.5  Replay logging
  0.6  Deploy to VPS

PHASE 1: Flycast Fork              ← CAN START IN PARALLEL WITH 0
  1.1  MapleNetController device
  1.2  ServerSync network module
  1.3  Frame loop integration (5-line diff)
  1.4  Local input via SDL (Option A, works immediately)
  1.5  CLI config
  1.6  HUD overlay
  TEST: Two Flycast instances, one server, same game

PHASE 2: GP2040-CE Integration     ← AFTER 0 + 1 PROVEN
  2.1  CMD9 UDP broadcast from stick
  2.2  Server tick reception
  2.3  Auto mode detection
  TEST: Real DC + GP2040-CE vs Flycast, through server

PHASE 3: Cross-Play Matrix         ← AFTER 2 PROVEN
  All combinations tested and validated

PHASE 4: Tournament                ← AFTER 3 STABLE
  4.1  Replay system
  4.2  Tournament server mode
  4.3  Match verification
  4.4  Connection pre-check

PHASE 5: Overkill                  ← ONGOING, NEVER DONE
  5.1  Batch I/O
  5.2  CPU pinning
  5.3  io_uring
  5.4  Direct UDP (no USB)
  5.5  Shared memory bridge
  5.6  Packet redundancy
```

---

## Success Criteria

- [ ] Two Flycast instances show identical gameplay with same frame numbers
- [ ] Input-to-screen latency overhead < 1 frame for local player
- [ ] Network lag = FPS slowdown, never desync, never visual artifacts
- [ ] GP2040-CE on real DC cross-plays with Flycast through server
- [ ] Match replay produces frame-identical reproduction
- [ ] Tournament mode: 60fps lock, sub-100ms disconnect, provable results
- [ ] Server handles 1000+ concurrent matches on single core
- [ ] Total bandwidth per match < 15 KB/s
- [ ] Same 32-byte CMD9 response format end-to-end, no translation anywhere

---

## The Principle

```
Maple Bus is the protocol.
CMD9 is the API.
The lookup table is the database.
The server is the clock.
The network is just a longer wire.
Everything else is a consumer.
```
