"""
NOBD Finger Gap Tester
======================
Measures the time gap between your two button presses when you try
to press them simultaneously (like LP+HP for a dash).

Usage:
  1. Plug in your stick
  2. Run: python test_finger_gap.py
  3. Press two buttons at the same time, over and over
  4. The script shows the gap in ms for each attempt
  5. Press Ctrl+C to see summary stats

Requires: pip install pygame
"""

import pygame
import time
import sys

pygame.init()
pygame.joystick.init()

if pygame.joystick.get_count() == 0:
    print("No gamepad detected! Plug in your stick and try again.")
    sys.exit(1)

joy = pygame.joystick.Joystick(0)
joy.init()
print(f"Connected: {joy.get_name()}")
print(f"Buttons: {joy.get_numbuttons()}")
print()
print("=" * 60)
print("  NOBD FINGER GAP TESTER")
print("=" * 60)
print()
print("Press two buttons at the same time (like LP+HP for a dash).")
print("This measures how far apart your fingers actually are.")
print()
print("Press Ctrl+C to stop and see stats.")
print("-" * 60)

gaps = []
first_press_time = None
first_button = None
WINDOW = 0.050  # 50ms - if second press is longer than this, treat as separate

try:
    while True:
        for event in pygame.event.get():
            if event.type == pygame.JOYBUTTONDOWN:
                now = time.perf_counter()

                if first_press_time is None:
                    # First button of a potential pair
                    first_press_time = now
                    first_button = event.button
                else:
                    # Second button arrived
                    gap_ms = (now - first_press_time) * 1000

                    if gap_ms <= WINDOW * 1000:
                        # Within window - this is a simultaneous attempt
                        gaps.append(gap_ms)
                        n = len(gaps)
                        avg = sum(gaps) / n
                        mn = min(gaps)
                        mx = max(gaps)

                        print(f"  #{n:3d}  Button {first_button}+{event.button}  "
                              f"gap: {gap_ms:5.1f}ms  "
                              f"(avg: {avg:.1f}ms  min: {mn:.1f}ms  max: {mx:.1f}ms)")

                        first_press_time = None
                        first_button = None
                    else:
                        # Too far apart - treat this as a new first press
                        first_press_time = now
                        first_button = event.button

            elif event.type == pygame.JOYBUTTONUP:
                pass

        time.sleep(0.0001)  # 0.1ms polling

except KeyboardInterrupt:
    print()
    print("=" * 60)
    print("  RESULTS")
    print("=" * 60)

    if len(gaps) == 0:
        print("  No simultaneous presses detected.")
    else:
        avg = sum(gaps) / len(gaps)
        mn = min(gaps)
        mx = max(gaps)
        gaps_sorted = sorted(gaps)
        median = gaps_sorted[len(gaps_sorted) // 2]

        # Distribution buckets
        buckets = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 50]
        counts = [0] * (len(buckets))

        for g in gaps:
            for i in range(len(buckets) - 1):
                if g < buckets[i + 1]:
                    counts[i] += 1
                    break
            else:
                counts[-1] += 1

        print(f"  Total attempts: {len(gaps)}")
        print(f"  Average gap:    {avg:.1f}ms")
        print(f"  Median gap:     {median:.1f}ms")
        print(f"  Fastest:        {mn:.1f}ms")
        print(f"  Slowest:        {mx:.1f}ms")
        print()
        print("  Distribution:")
        for i in range(len(buckets) - 1):
            if counts[i] > 0:
                bar = "#" * counts[i]
                pct = counts[i] / len(gaps) * 100
                print(f"    {buckets[i]:2d}-{buckets[i+1]:2d}ms: {counts[i]:3d} ({pct:4.1f}%) {bar}")

        print()
        import math
        recommended = max(3, math.ceil(avg) + 1)
        zero_count = sum(1 for g in gaps if g < 0.1)
        zero_pct = zero_count / len(gaps) * 100

        if zero_pct > 50:
            print(f"  *** OBD / MACRO DETECTED ***")
            print(f"  {zero_pct:.0f}% of presses had 0ms gap — likely OBD or a macro button.")
            print(f"  Turn off OBD to measure your natural finger gap.")
            print()

        print(f"  Recommended NOBD slider: {recommended}ms")
        print(f"  (based on your average gap of {avg:.1f}ms + 1ms headroom)")

    print()
    print("=" * 60)

pygame.quit()
