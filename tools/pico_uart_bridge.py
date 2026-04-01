# NOBD-DC-ONLINE — WiFi to UART Bridge
# Receives UDP button state, forwards as 5-byte UART frame to GP2040-CE
# GP4 (UART1 TX) -> GP2040-CE GPIO 28 (PIO UART RX)
# Baud: 1000000 (1 Mbps)  Frame: 0xAA + 4 bytes W3 = 5 bytes (50us)
#
# Key: drain ALL pending UDP, only forward the LATEST packet.
# This prevents queue buildup when sender is faster than we can process.

import network
import socket
import machine
import time

WIFI_SSID = "King_WiddleToes"
WIFI_PASSWORD = "Qwerty123"
UDP_PORT = 4977
BAUD = 1000000

led = machine.Pin("LED", machine.Pin.OUT)

print("========================================")
print("  NOBD-DC-ONLINE UART Bridge")
print(f"  GP4 TX @ {BAUD} -> GP2040-CE GPIO 28")
print("  Drain-to-latest: only newest state sent")
print("========================================")

wlan = network.WLAN(network.STA_IF)
wlan.active(True)
wlan.config(pm=0xa11140)  # Disable CYW43 power management (kills 10-50ms spikes)
wlan.connect(WIFI_SSID, WIFI_PASSWORD)

timeout = 30
while not wlan.isconnected() and timeout > 0:
    led.toggle()
    time.sleep(0.5)
    timeout -= 1

if not wlan.isconnected():
    print("WiFi FAILED — idle (physical buttons active on GP2040-CE)")
    led.value(0)
    while True:
        time.sleep(10)

ip = wlan.ifconfig()[0]
print(f"WiFi OK! IP: {ip}")
led.value(1)

uart = machine.UART(1, baudrate=BAUD, tx=machine.Pin(4), rx=machine.Pin(5))

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', UDP_PORT))
sock.setblocking(False)

# Flush stale packets
flushed = 0
while True:
    try:
        sock.recvfrom(64)
        flushed += 1
    except OSError:
        break
if flushed:
    print(f"Flushed {flushed} stale packets")

print(f"UDP listening on {ip}:{UDP_PORT}")
print("=== READY ===")

SYNC = b'\xAA'
packets = 0
forwarded = 0
last_stat = time.ticks_ms()

while True:
    # Drain all pending UDP — only keep the latest
    latest = None
    try:
        while True:
            data, addr = sock.recvfrom(64)
            packets += 1
            if len(data) >= 4:
                latest = data[0:4]
    except OSError:
        pass

    # Forward only the latest state
    if latest is not None:
        led.toggle()  # Blink LED on every forwarded packet
        uart.write(SYNC)
        uart.write(latest)
        forwarded += 1

    now = time.ticks_ms()
    if time.ticks_diff(now, last_stat) >= 5000:
        print(f"[{now//1000}s] rx:{packets} fwd:{forwarded} WiFi:{'OK' if wlan.isconnected() else 'LOST'}")
        last_stat = now
