"""Select a DualMesh launcher slot over serial (CH343 @ 115200).

Opening the COM port pulses the P4 reset line, so the launcher always
restarts when this script starts. We then repeat the bootN command through
the launcher's 3s splash window until it takes effect, and keep echoing
serial output so the chosen firmware's boot log is visible.

Usage: python boot-slot.py [COM6] [slot]   (slot defaults to 1)
"""
import sys
import time

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    slot = sys.argv[2] if len(sys.argv) > 2 else "1"

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.write_timeout = 1
    s.dtr = False  # asserted DTR/RTS holds the P4 in reset via the CH343
    s.rts = False
    s.open()

    cmd = f"boot{slot}\n".encode()
    start = time.time()
    print(f"spamming '{cmd.decode().strip()}' through the launcher window on {port}...")
    while time.time() - start < 25:
        if time.time() - start < 12:  # command window: launcher boot + splash
            try:
                s.write(cmd)
            except serial.SerialTimeoutException:
                pass
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()
    print("\n--- done (port closed; device keeps running) ---")


if __name__ == "__main__":
    main()
