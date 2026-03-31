#!/usr/bin/env python3
"""
NOBD-DC-ONLINE — PC Gamepad to UDP Sender
Streams DC W3 button state to Pico 2 W bridge.

Usage: python pc_gamepad_sender.py <bridge_ip> [port]

Design: send IMMEDIATELY on state change, refresh at 120Hz (8ms).
DC polls at 60Hz (16ms). 120Hz ensures fresh state every poll.
"""

import sys
import socket
import struct
import time
import ctypes

try:
    import pygame
except ImportError:
    print("ERROR: pygame required. Install with: pip install pygame")
    sys.exit(1)

# Force Windows timer to 1ms resolution
try:
    ctypes.windll.winmm.timeBeginPeriod(1)
except Exception:
    pass

# DC button masks (active-low: 0=pressed, 1=released)
DC_BTN_C     = 0x0001
DC_BTN_B     = 0x0002
DC_BTN_A     = 0x0004
DC_BTN_START = 0x0008
DC_BTN_UP    = 0x0010
DC_BTN_DOWN  = 0x0020
DC_BTN_LEFT  = 0x0040
DC_BTN_RIGHT = 0x0080
DC_BTN_Z     = 0x0100
DC_BTN_Y     = 0x0200
DC_BTN_X     = 0x0400

def build_w3(joy):
    """Build DC W3 word from pygame joystick state."""
    buttons = 0xFFFF

    btn_map = {
        0: DC_BTN_A,
        1: DC_BTN_B,
        2: DC_BTN_X,
        3: DC_BTN_Y,
        6: DC_BTN_START,
    }
    for btn_idx, dc_mask in btn_map.items():
        if btn_idx < joy.get_numbuttons() and joy.get_button(btn_idx):
            buttons &= ~dc_mask

    if joy.get_numhats() > 0:
        hx, hy = joy.get_hat(0)
        if hy > 0:  buttons &= ~DC_BTN_UP
        if hy < 0:  buttons &= ~DC_BTN_DOWN
        if hx < 0:  buttons &= ~DC_BTN_LEFT
        if hx > 0:  buttons &= ~DC_BTN_RIGHT

    lt = 0
    rt = 0
    if joy.get_numaxes() > 4:
        lt = int(max(0, (joy.get_axis(4) + 1.0) / 2.0) * 255)
    if joy.get_numaxes() > 5:
        rt = int(max(0, (joy.get_axis(5) + 1.0) / 2.0) * 255)

    if joy.get_numaxes() <= 4:
        if 4 < joy.get_numbuttons() and joy.get_button(4): lt = 255
        if 5 < joy.get_numbuttons() and joy.get_button(5): rt = 255

    return struct.pack('>BBBB', lt, rt, (buttons >> 8) & 0xFF, buttons & 0xFF)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <bridge_ip> [port]")
        sys.exit(1)

    bridge_ip = sys.argv[1]
    bridge_port = int(sys.argv[2]) if len(sys.argv) > 2 else 4977
    dest = (bridge_ip, bridge_port)

    pygame.init()
    pygame.joystick.init()

    if pygame.joystick.get_count() == 0:
        print("ERROR: No gamepad detected")
        sys.exit(1)

    joy = pygame.joystick.Joystick(0)
    joy.init()
    print(f"Gamepad: {joy.get_name()}")
    print(f"Sending to {bridge_ip}:{bridge_port}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    prev_w3 = None
    sent = 0
    changes = 0
    last_send = time.perf_counter()
    last_stat = last_send

    REFRESH_INTERVAL = 0.008  # 8ms = 120Hz (2x DC poll rate)

    print("=== STREAMING ===")
    try:
        while True:
            pygame.event.pump()
            w3 = build_w3(joy)
            now = time.perf_counter()

            if w3 != prev_w3:
                # State changed — send IMMEDIATELY
                sock.sendto(w3, dest)
                prev_w3 = w3
                sent += 1
                changes += 1
                last_send = now
            elif now - last_send >= REFRESH_INTERVAL:
                # No change — periodic refresh to keep state fresh
                sock.sendto(w3, dest)
                sent += 1
                last_send = now
            else:
                # Nothing to send — yield briefly
                time.sleep(0.001)

            if now - last_stat >= 5.0:
                rate = sent / (now - last_stat)
                print(f"rate:{rate:.0f}/s changes:{changes} w3={w3.hex()}")
                sent = 0
                changes = 0
                last_stat = now

    except KeyboardInterrupt:
        print("\nDone.")
    finally:
        try:
            ctypes.windll.winmm.timeEndPeriod(1)
        except Exception:
            pass
        pygame.quit()


if __name__ == '__main__':
    main()
