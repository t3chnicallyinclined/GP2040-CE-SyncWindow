#!/usr/bin/env python3
"""
GP-RETRO-ONLINE Relay Server — Single Source of Truth

Both sticks send their local P1 buttons here.
Server merges and broadcasts identical {P1, P2} to both.
Zero desync by design.

Protocol:
  Stick → Server:  4 bytes (local W3 button state)
  Server → Stick:  8 bytes (P1 W3 + P2 W3 merged state)

Usage:
  python relay_server.py [port]
  Default port: 4977

The first two unique IPs that send packets become Player A and Player B.
"""

import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4977

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', PORT))
sock.setblocking(False)

# Player slots
player_a = None  # (ip, port) tuple
player_b = None
p1_w3 = b'\x00\x00\xff\xff'  # All released
p2_w3 = b'\x00\x00\xff\xff'  # All released

packets_in = 0
packets_out = 0
last_stat = time.time()

print("=" * 50)
print("  GP-RETRO-ONLINE Relay Server")
print(f"  Listening on UDP port {PORT}")
print("  Waiting for two players...")
print("=" * 50)

while True:
    try:
        data, addr = sock.recvfrom(64)
    except BlockingIOError:
        # No data — check stats
        now = time.time()
        if now - last_stat >= 5.0:
            pa = f"{player_a[0]}:{player_a[1]}" if player_a else "waiting"
            pb = f"{player_b[0]}:{player_b[1]}" if player_b else "waiting"
            print(f"[{int(now)}] P1={pa} P2={pb} in:{packets_in} out:{packets_out}")
            packets_in = 0
            packets_out = 0
            last_stat = now
        continue

    if len(data) < 4:
        continue

    packets_in += 1
    w3 = data[0:4]

    # Assign players by first two unique addresses
    if player_a is None:
        player_a = addr
        print(f"Player A connected: {addr[0]}:{addr[1]}")
    elif addr == player_a:
        pass  # Already P1
    elif player_b is None:
        player_b = addr
        print(f"Player B connected: {addr[0]}:{addr[1]}")
    elif addr == player_b:
        pass  # Already P2
    else:
        continue  # Ignore unknown senders

    # Update the correct player's state
    if addr == player_a:
        p1_w3 = w3
    elif addr == player_b:
        p2_w3 = w3

    # Broadcast merged state to both players
    merged = p1_w3 + p2_w3  # 8 bytes: P1 (4) + P2 (4)

    if player_a:
        sock.sendto(merged, player_a)
        packets_out += 1
    if player_b:
        sock.sendto(merged, player_b)
        packets_out += 1
