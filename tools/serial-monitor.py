"""Passive serial monitor for the T-Display P4 (CH343 @ 115200).

Opens the port with DTR/RTS deasserted (asserted lines hold the P4 in
reset); the open itself still pulses reset, so this captures a boot from
the very start. Prints everything with a relative timestamp per chunk.

Usage: python serial-monitor.py [COM6] [seconds] [reset]
  "reset" as third arg pulses DTR/RTS (asserted = P4 held in reset via the
  CH343 circuit) so the capture deterministically starts from a fresh boot.
"""
import sys
import time

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 45
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    if len(sys.argv) > 3 and sys.argv[3] == "reset":
        s.dtr = True
        s.rts = True
        time.sleep(0.2)
        s.dtr = False
        s.rts = False
        s.reset_input_buffer()
    start = time.time()
    print(f"--- monitoring {port} for {duration:.0f}s (device resets on open) ---")
    last_data = start
    while time.time() - start < duration:
        data = s.read(4096)
        if data:
            last_data = time.time()
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()
    print(f"\n--- done; last byte received at t+{last_data - start:.1f}s ---")


if __name__ == "__main__":
    main()
