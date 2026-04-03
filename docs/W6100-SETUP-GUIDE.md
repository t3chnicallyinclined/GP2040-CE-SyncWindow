# W6100 Ethernet Setup Guide

Complete guide for getting the W6100-EVB-Pico2 board running with Dreamcast online play. Covers hardware wiring, firmware build, network setup, and troubleshooting.

**Board:** [WIZnet W6100-EVB-Pico2](https://docs.wiznet.io/Product/iEthernet/W6100/w6100-evb-pico2) (~$15)
**Chip:** RP2350 + W6100 hardwired Ethernet (on-board SPI, on-board RJ45)
**Latency:** Sub-1ms ping, deterministic

---

## 1. What You Need

| Item | Notes |
|------|-------|
| W6100-EVB-Pico2 board | RP2350 with built-in W6100 Ethernet |
| Dreamcast console | Real hardware |
| 1-2 Dreamcast controller cables | Cut and stripped — P1 required, P2 for online |
| Ethernet cable | Cat5e or better, to your router/switch |
| OLED display (optional) | 128x64 I2C, 4-pin |
| USB-C cable | For flashing firmware |

---

## 2. Wiring

These are the tested, known-working pin assignments. Wire exactly this and it works.

### Dreamcast Cables

Cut a Dreamcast controller cable. Inside are 5 wires: Data A (pin 1), Data B (pin 5), 5V (pin 3), GND (pins 2+4). Wire them to the board header pins:

```
P1 (required):
  Data A  →  GP0   (header pin 1)
  Data B  →  GP1   (header pin 2)
  5V      →  VBUS  (header pin 40) — powers the whole board
  GND     →  GND   (header pin 3)

P2 (for online play — second cable to DC Port 2):
  Data A  →  GP2   (header pin 4)
  Data B  →  GP3   (header pin 5)
  GND     →  GND   (header pin 8)
  5V      →  DO NOT CONNECT
```

**Only connect 5V from ONE cable.** The P1 cable powers the board. P2 cable still needs GND for signal reference.

### Buttons

Wire each button between the GPIO pin and any GND. No resistors needed — internal pull-ups are enabled.

```
UP     →  GP4       DOWN   →  GP5       LEFT   →  GP6       RIGHT  →  GP7
A      →  GP8       B      →  GP9       X      →  GP10      Y      →  GP11
Z(R1)  →  GP12      C(L1)  →  GP13      RT(R2) →  GP14      LT(L2) →  GP15
Select →  GP22      Start  →  GP28
```

14 buttons total. That's every Dreamcast button including analog triggers as digital.

### Ethernet

**Just plug in an Ethernet cable.** The W6100 + RJ45 jack are built into the board (GPIO 16-21, hardwired). No wiring needed.

### Display (optional)

```
SDA  →  GP26      SCL  →  GP27
```

128x64 I2C OLED. 4-pin: VCC → 3V3, GND → GND, SDA → GP26, SCL → GP27.

### Do NOT Use These Pins

| Pins | Why |
|------|-----|
| GPIO 16-21 | W6100 Ethernet (hardwired on-board) |
| GPIO 23 | Not broken out on this board |
| GPIO 24, 25, 29 | System (VBUS sense, LED, VSYS ADC) |

---

## 3. Building Firmware

### Prerequisites

- ARM GCC toolchain (`arm-none-eabi-gcc`)
- CMake + Ninja
- Node.js + npm (for web UI)

### Build Command

```bash
GP2040_BOARDCONFIG=W6100EVBPico2 cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
cmake --build build
```

The output UF2 file will be in `build/`.

### Flashing

1. Hold the BOOT button on the W6100-EVB-Pico2 board
2. Plug in USB-C — board mounts as a USB drive
3. Drag the `.uf2` file onto the drive
4. Board reboots and runs firmware

---

## 4. Network Setup

### How It Works

On boot, the firmware:

1. Initializes W6100 SPI (GPIO 16-21, 33MHz)
2. Performs hardware reset (RST pin low → high, 100ms settle)
3. Registers SPI callbacks with WIZnet ioLibrary
4. Runs DHCP to get an IP address (up to 10 seconds)
5. Falls back to `192.168.1.100` if DHCP fails
6. Opens UDP socket 0 on port `4977` for gamepad data

### Verifying Network

Once the board is powered and connected via Ethernet:

```bash
# Find the board's IP (check your router's DHCP leases, or try the fallback)
ping 192.168.1.100
```

If DHCP is working, the board gets an IP from your router. Check your router's DHCP client list.

### Online Play Modes

**Direct mode (P2 input from PC):**
```bash
python tools/pc_gamepad_sender.py <board_ip> [port]
# Default port: 4977
# Requires: pip install pygame
```

The PC gamepad sender reads an Xbox/PlayStation controller via pygame and streams button state as 4-byte UDP packets to the board. The board injects this as P2 input on the Dreamcast.

**Relay server mode (two sticks online):**
```bash
# On a server/PC both players can reach:
python tools/relay_server.py [port]
# Default port: 4977

# Each stick sends its local P1 buttons to the server.
# Server merges and broadcasts {P1, P2} to both sticks.
```

The relay server is currently hardcoded to `192.168.1.63:7100` in firmware. This will move to web UI configuration.

### Network Architecture

```
PC Gamepad → UDP 4977 → W6100-EVB-Pico2 → P2 Maple Bus → DC Port 2
     or
Stick A → UDP → Relay Server → UDP → Stick B
Stick B → UDP → Relay Server → UDP → Stick A
```

The W6100 handles hardware TCP/UDP offload — the RP2350 just calls `recvfrom()`/`sendto()` over SPI. No software TCP/IP stack needed.

---

## 5. Driver Architecture

### Software Stack

```
lib/ioLibrary_Driver/          — WIZnet official library
  Ethernet/W6100/w6100.c       — W6100 chip driver
  Ethernet/socket.c            — Berkeley socket API
  Ethernet/wizchip_conf.c      — SPI callback registration
  Internet/DHCP/dhcp.c         — DHCP client

src/net/w6100.cpp              — Our wrapper: SPI callbacks + DHCP + UDP
headers/net/w6100.h            — Public API

src/drivers/dreamcast/DreamcastDriver.cpp
  initEthernet()               — Init W6100, open UDP socket
  pollEthernet()               — Recv UDP, update P2 CMD9
  sendLocalState()             — Send P1 buttons to relay server
  pollNetwork()                — Dispatches to Ethernet or UART
```

### SPI Pin Mapping (on-board, not configurable)

| Function | GPIO | W6100 Pin |
|----------|------|-----------|
| MISO | 16 | MISO |
| CS | 17 | CSn (active low) |
| SCLK | 18 | SCLK |
| MOSI | 19 | MOSI |
| RST | 20 | RSTn |
| INT | 21 | INTn |

SPI clock: 33MHz. Uses SPI0.

### Key Constants

| Constant | Value | Notes |
|----------|-------|-------|
| DHCP socket | 7 | Socket 7 reserved for DHCP |
| Gamepad UDP socket | 0 | Socket 0, port 4977 |
| MAC address | `00:08:DC:DC:00:01` | Hardcoded |
| DHCP timeout | 10 seconds | Falls back to static IP |
| Fallback IP | `192.168.1.100` | Used if no DHCP response |
| Fallback subnet | `255.255.255.0` | |
| Fallback gateway | `192.168.1.1` | |

---

## 6. Troubleshooting

### Board doesn't get an IP / no ping response

**DHCP is required for proper W6100 initialization.** Even if you want a static IP, the DHCP negotiation flow initializes the chip's internal network state. Without it, the chip responds on SPI but is invisible on the network (no ARP, no ping).

- Make sure the Ethernet cable is plugged into a router/switch with DHCP enabled
- The board will fall back to `192.168.1.100` after 10 seconds if DHCP fails
- Check that your network's subnet includes `192.168.1.x` if using fallback

### SPI communication issues

**Never write a custom W6100 SPI driver.** Raw register writes to SHAR/SIPR/SUBR/GAR don't take effect even though SPI reads work. The chip responds to version reads but ignores network config writes without proper sequencing. Always use the WIZnet ioLibrary — it handles all SPI framing and register sequencing correctly.

### printf debugging doesn't work

`pico_enable_stdio_usb` conflicts with TinyUSB (GP2040-CE has its own USB stack). Use LED blinks or the OLED display for diagnostics instead.

### Stale flash config causes silent failure

If you previously flashed firmware for a different board, stale config in flash can prevent Ethernet init. The firmware now initializes Ethernet unconditionally (doesn't check for UART pin config). If you suspect stale config, reflash and clear settings via the web UI.

### W6100 version read returns unexpected values

`getCIDR()` returns different values depending on whether you use raw SPI or ioLibrary. This is normal. Skip version checks and proceed with init — if SPI is working, the chip is alive.

---

## 7. Advantages Over WiFi Bridge

| | WiFi (Pico 2 W bridge) | W6100 Ethernet |
|---|---|---|
| Latency | 1-5ms (unpredictable) | <1ms (deterministic) |
| Extra hardware | Pico 2 W + UART wiring + bridge script | None (built-in) |
| Setup complexity | WiFi config + UART + Python scripts | Plug in Ethernet cable |
| Reliability | WiFi drops, interference | Wired, rock solid |
| RAM | 264KB (tight on RP2040) | 520KB (RP2350, comfortable) |
| PIO blocks | 2 (exactly full on RP2040) | 3 (room to spare on RP2350) |

---

## 8. V2 Lite PCB (Future)

The V2 Lite PCB integrates RP2040 + STM32F730 + W6100 on a single 90x45mm board with 63 components. On that board, the W6100 SPI pins move to GPIO 25-29:

| Function | GPIO |
|----------|------|
| CS | 25 |
| SCK | 26 (33 ohm series termination) |
| MOSI | 27 |
| MISO | 28 |
| INT | 29 |

The V2 Lite also adds 8000Hz USB polling via STM32 internal USB HS PHY, dual Maple Bus ports (GPIO 4/5 + GPIO 23/24), and level-shifted RJ45 for retro consoles. Full spec in `docs/v2-hardware/V2-LITE-SPEC.md`.
