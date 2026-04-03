# GP-RETRO-ONLINE MVP Workstream

## Vision
play.gp-retro.online — Open browser. Plug in controller. Play MvC2 for money. Zero install.

Server runs the game (real Dreamcast or Flycast). Client is just a video player + input sender + wallet. WebRTC for video, our relay protocol for inputs. Crypto escrow for money matches.

---

## Phase 1: Server-Side Flycast with Relay Protocol (2-3 weeks)

### Goal
Flycast running MvC2 headless on a Linux server, accepting inputs via our relay protocol (UDP port 4977), outputting raw frames for encoding.

### Tasks

#### 1.1 Emulator Selection & Fork
- [ ] Research: Compare Flycast, Redream, lxdream source architecture
  - Flycast: C++, Vulkan/GL, best compatibility, active, GPLv2
  - Redream: C, cleanest code, partially closed source
  - lxdream: C, simplest, poor compatibility
  - **Decision needed:** Flycast is likely the answer but verify MvC2 runs perfectly
- [ ] Fork Flycast into our repo (github.com/t3chnicallyinclined/gp-retro-flycast)
- [ ] Verify MvC2 runs at 60fps on target server hardware
- [ ] Document which Flycast features MvC2 uses vs doesn't use

#### 1.2 Strip Flycast for MvC2
- [ ] Remove: GD-ROM streaming, VMU LCD, serial port, modem, keyboard, mouse, fishing rod
- [ ] Remove: Modifier volumes renderer path (MvC2 doesn't use)
- [ ] Remove: Bump mapping renderer path
- [ ] Remove: Multi-disc support
- [ ] Remove: Debugger, profiler, GDB stub
- [ ] Keep: SH-4 dynarec, PowerVR2 renderer, AICA sound, VQ textures, palette textures
- [ ] Target: minimal binary that boots MvC2 and nothing else

#### 1.3 Headless Rendering Mode
- [ ] Modify Flycast to render to offscreen buffer (no window)
- [ ] Output raw frames via shared memory or pipe
- [ ] Options: Vulkan offscreen render, EGL headless, or software renderer
- [ ] Target: 480p60 raw frames available for encoding pipeline

#### 1.4 Integrate Relay Protocol
- [ ] Add UDP socket listener (port 4977) to Flycast
- [ ] Receive 4-byte W3 from each player → inject as controller input
- [ ] Send 8-byte merged state back to both players
- [ ] Map W3 button format (DC native) directly to Flycast's internal controller state
- [ ] This replaces Flycast's normal input handling (SDL/evdev/etc.)
- [ ] **Key insight:** Our W3 format IS the Dreamcast controller format. Flycast already understands it internally. Minimal translation needed.

#### 1.5 Test
- [ ] Run Flycast headless on Linux
- [ ] Connect pc_gamepad_sender.py as P1
- [ ] Connect second instance as P2
- [ ] Verify both inputs register in MvC2
- [ ] Verify frame output is clean 480p60

### Deliverable
Flycast binary that: boots MvC2, accepts relay protocol inputs, outputs raw frames.

---

## Phase 2: WebRTC Video Pipeline (1-2 weeks)

### Goal
Capture Flycast's rendered frames, encode H.264, stream via WebRTC to browsers.

### Tasks

#### 2.1 Encode Pipeline
- [ ] GStreamer pipeline: Flycast frames → NVENC/VAAPI H.264 → RTP
- [ ] Or FFmpeg pipeline: pipe/shm → libx264/nvenc → RTP
- [ ] Tune for lowest latency:
  - No B-frames
  - Slice-based encoding
  - CBR at 3-5 Mbps (480p60 doesn't need more)
  - NVENC "ultra low latency" preset
  - Keyframe interval: 1 second
- [ ] Target: <3ms encode latency per frame

#### 2.2 WebRTC Media Server
- [ ] Deploy Janus Gateway or mediasoup
- [ ] WHIP ingest (encoder → Janus)
- [ ] WHEP egress (Janus → browser)
- [ ] Or: GStreamer webrtcsink (simpler, single pipeline)
- [ ] Configure aggressive jitter buffer (10-20ms)
- [ ] Test: browser can see live video feed

#### 2.3 Audio
- [ ] Capture AICA audio output from Flycast
- [ ] Encode Opus (WebRTC native codec)
- [ ] Mux with video in WebRTC stream
- [ ] Target: synced audio+video, <50ms total

### Deliverable
Browser opens URL, sees live MvC2 gameplay with audio.

---

## Phase 3: Browser Client (2-3 weeks)

### Goal
play.gp-retro.online — functional web client with video, input, and lobby.

### Tasks

#### 3.1 Frontend Framework
- [ ] Tech choice: Svelte, React, or vanilla JS
- [ ] Recommendation: Svelte (smallest bundle, fastest)
- [ ] Deploy: Cloudflare Pages or Vercel (free tier)

#### 3.2 Video Player
- [ ] WebRTC player component
- [ ] Connect to Janus/mediasoup via WHEP or Janus API
- [ ] Fullscreen mode
- [ ] Latency overlay (show current input-to-display ms)

#### 3.3 Input Capture
- [ ] Gamepad API — detect controller, read buttons at 1kHz
- [ ] Keyboard fallback (WASD + numpad)
- [ ] Map to DC W3 format (same as pc_gamepad_sender.py)
- [ ] Send via WebRTC data channel to relay server
- [ ] Or: direct UDP via WebTransport (lower latency than data channel)
- [ ] Display input overlay (show what buttons are pressed)

#### 3.4 Lobby System
- [ ] Room list (each room = one Flycast instance or one real DC)
- [ ] Join as player or spectator
- [ ] Queue system ("put up a quarter")
- [ ] Player status (playing, spectating, in queue)
- [ ] WebSocket for lobby state (not UDP — needs reliability)

#### 3.5 Chat
- [ ] Simple text chat per room
- [ ] WebSocket-based
- [ ] Show during spectate and queue

### Deliverable
Functional website: watch game, join queue, play when it's your turn.

---

## Phase 4: Money Matches (2-3 weeks)

### Goal
Crypto escrow for match stakes. Provably fair.

### Tasks

#### 4.1 Smart Contract
- [ ] Solidity or Rust (Solana)
- [ ] Functions: deposit, create_match, submit_result, dispute, withdraw
- [ ] Escrow: both players deposit before match starts
- [ ] Payout: winner gets pot minus platform fee (5%?)
- [ ] Dispute: 24-hour window, arbiter resolves
- [ ] Chain choice: Solana (fast, cheap) or Base/Arbitrum (Ethereum L2)

#### 4.2 Wallet Integration
- [ ] WalletConnect v2 (supports most wallets)
- [ ] MetaMask, Phantom, Coinbase Wallet
- [ ] Display balance, match stake, pot size
- [ ] Transaction confirmation before match starts

#### 4.3 Input Recording
- [ ] Relay server records all inputs per frame (already have the data)
- [ ] Timestamp + sequence number each frame
- [ ] Both players sign the input log hash at match end
- [ ] Store on IPFS or Arweave (permanent, cheap)
- [ ] Anyone can download and replay to verify result

#### 4.4 Match State Machine
- [ ] Server tracks: waiting, matched, playing, result, payout
- [ ] Auto-detect match end (Flycast can report game state)
- [ ] Or: players manually confirm result (simpler MVP)
- [ ] Timeout handling (player disconnects mid-match)

### Deliverable
Play MvC2 for crypto. Winner gets paid. Disputes are provably resolvable.

---

## Phase 5: Virtual Arcade (1-2 weeks)

### Goal
The full arcade experience. Winner stays. Spectators watch. Queue up with quarters.

### Tasks

#### 5.1 Queue Manager
- [ ] FIFO queue per room
- [ ] "Put up a quarter" = join queue with micro-payment
- [ ] Winner stays as P1, next in queue becomes P2
- [ ] Loser goes to back of queue (or leaves)
- [ ] Display: who's playing, who's next, queue position

#### 5.2 Spectator Mode
- [ ] All connected browsers see the same WebRTC stream
- [ ] Spectator count displayed
- [ ] No input from spectators (read-only)
- [ ] Can join queue while spectating

#### 5.3 Streak Counter
- [ ] Track consecutive wins
- [ ] Display: "SmoothViper is on a 7-game streak!"
- [ ] Leaderboard: longest streaks, most wins, biggest earnings

### Deliverable
Full virtual arcade. Winner stays on. Quarters. Hype. Chat. Streaks.

---

## Infrastructure

### Minimum Server Hardware ($650-900)
- Dell Optiplex SFF (i5-10400, Quick Sync) — $250
- Magewell Pro Capture HDMI PCIe — $400 (only for real DC mode)
- W6100-EVB-Pico2 + Maple Bus cables — $15 (only for real DC mode)
- Dreamcast VA1 + GDEMU + Akura HDMI — $140 (only for real DC mode)

### For Emulated Mode (no real DC needed)
- Dell Optiplex SFF with NVIDIA T400 — $350-500
- NVENC does the encoding
- Flycast runs on CPU (dynarec, full speed on any modern i5+)

### Hosting
- Colocation: $50-100/month (1/4 rack, power, 100Mbps)
- Or cloud: AWS/GCP GPU instance ~$0.50-1.00/hour
- Or home server for testing: free

### Domain + CDN
- play.gp-retro.online
- Cloudflare (free tier for static assets)
- WebRTC server needs dedicated IP (not behind CDN)

---

## Timeline to MVP

| Phase | Duration | Running Total |
|-------|----------|---------------|
| Phase 1: Server-side Flycast | 2-3 weeks | 2-3 weeks |
| Phase 2: WebRTC pipeline | 1-2 weeks | 3-5 weeks |
| Phase 3: Browser client | 2-3 weeks | 5-8 weeks |
| Phase 4: Money matches | 2-3 weeks | 7-11 weeks |
| Phase 5: Virtual arcade | 1-2 weeks | 8-13 weeks |

**MVP in ~2-3 months** with focused development.

---

## Research Still Needed

- [ ] Flycast source audit: which renderer backend for headless?
- [ ] Flycast input injection API: how does it accept controller input internally?
- [ ] NVENC vs QSV latency benchmarks at 480p60
- [ ] WebRTC data channel vs WebTransport latency comparison
- [ ] Solana vs Base for smart contract (gas costs, speed)
- [ ] MvC2 ROM distribution: legal considerations
- [ ] GD-ROM image → Flycast loadable format (CHD? GDI?)

---

## The Moat

Nobody else has:
1. Real Dreamcast hardware online (<2µs input injection via W6100)
2. Zero-install browser client
3. Cross-play between real hardware and emulator
4. Built-in crypto money matches
5. Virtual arcade queue system

Fightcade requires download + ROM. We require a browser.
Fightcade uses rollback. We use single source of truth (zero desync).
Fightcade is free. We have money matches.
Fightcade runs on your PC. We run on a server (no hardware requirements).
