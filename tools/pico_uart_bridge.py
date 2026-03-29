# NOBD-DC-ONLINE — WiFi to UART Bridge
# Receives UDP MFP frames, forwards via UART to GP2040-CE PIO UART receiver
# GP4 (UART1 TX) -> GP2040-CE GPIO 23
# Baud: 115200  Frame: 0xAA 0x55 + 12 bytes MFP data = 14 bytes
# Physical buttons always work — network only active when UDP arrives

import network
import socket
import machine
import time

WIFI_SSID = "King_WiddleToes"
WIFI_PASSWORD = "Qwerty123"
MFP_PORT = 4977

# GP4 = UART1 TX — separate from boot UART0 on GP0, no boot garbage
# GP0 was causing random button presses during MicroPython boot

led = machine.Pin("LED", machine.Pin.OUT)

HEADER = b'\xAA\x55'

print("========================================")
print("  NOBD-DC-ONLINE UART Bridge")
print("  GP4 TX @ 115200 -> GP2040-CE GPIO 23")
print("========================================")

wlan = network.WLAN(network.STA_IF)
wlan.active(True)
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

# UART1 on GP4 TX — boot-safe, no garbage
uart = machine.UART(1, baudrate=115200, tx=machine.Pin(4), rx=machine.Pin(5))

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', MFP_PORT))
sock.setblocking(False)

# Flush any stale packets from socket buffer
flushed = 0
while True:
    try:
        sock.recvfrom(64)
        flushed += 1
    except OSError:
        break
if flushed:
    print(f"Flushed {flushed} stale packets")

print(f"UDP listening on {ip}:{MFP_PORT}")
print("=== READY ===")

packets = 0
uart_frames = 0
last_stat = time.ticks_ms()

while True:
    try:
        data, addr = sock.recvfrom(64)
        if len(data) >= 12:
            uart.write(HEADER + data[0:12])
            uart_frames += 1
            packets += 1
    except OSError:
        pass

    now = time.ticks_ms()
    if time.ticks_diff(now, last_stat) >= 5000:
        print(f"[{now//1000}s] UDP:{packets} UART:{uart_frames} WiFi:{'OK' if wlan.isconnected() else 'LOST'}")
        last_stat = now
