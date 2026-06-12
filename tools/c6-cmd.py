"""Send one command to the already-running c6-updater console and echo output.

Opens the port without touching DTR/RTS (no reset). If the port-open happens
to pulse a reset anyway, the long read window catches the reboot + banner.

Usage: python c6-cmd.py COM6 version [read_seconds]
"""
import sys
import time

import serial


def main():
    port = sys.argv[1]
    cmd = sys.argv[2]
    window = float(sys.argv[3]) if len(sys.argv) > 3 else 8

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.write_timeout = 1
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.5)
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    end = time.time() + window
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()


if __name__ == "__main__":
    main()
