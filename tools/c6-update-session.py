"""Drive a c6-updater session on a DualMesh device.

Assumes the c6-updater is installed in slot 1 and slot 1 is the launcher's
last-boot choice (auto-boots after the 3s splash) — no launcher commands sent.
Pulses a reset (best effort), then polls the updater console with 'version'
until it answers; with --flash, sends 'flash' and follows the OTA through.

Usage: python c6-update-session.py [COM6] [--flash]
"""
import re
import sys
import time

import serial

VER_RE = re.compile(r"(?:CURRENT C6 FIRMWARE|C6 firmware): ?(\d+\.\d+\.\d+)")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    do_flash = "--flash" in sys.argv

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.write_timeout = 1
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = True
    s.rts = True
    time.sleep(0.2)
    s.dtr = False
    s.rts = False
    time.sleep(0.5)
    s.reset_input_buffer()

    buf = ""
    current_ver = None
    flashed = False
    new_ver = None
    last_poll = 0.0
    start = time.time()
    deadline = start + 90

    def feed():
        nonlocal buf
        d = s.read(4096)
        if d:
            text = d.decode("utf-8", "replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            buf += text

    while time.time() < deadline:
        feed()

        if current_ver is None:
            m = VER_RE.search(buf)
            if m:
                current_ver = m.group(1)
                print(f"\n>>> detected C6 version: {current_ver}")
                if not do_flash:
                    break
            elif time.time() - last_poll > 6:
                # device may be mid-boot (writes harmlessly ignored) or at
                # the prompt (answers) — poll either way
                try:
                    s.write(b"version\n")
                except serial.SerialTimeoutException:
                    pass
                last_poll = time.time()
            continue

        if do_flash and not flashed:
            print("\n>>> sending 'flash'")
            s.write(b"flash\n")
            flashed = True
            deadline = time.time() + 300
            continue

        if flashed and new_ver is None:
            m = re.search(r"NEW C6 FIRMWARE: (\d+\.\d+\.\d+)", buf)
            if m:
                new_ver = m.group(1)
                time.sleep(1)
                feed()
                break

    s.close()
    print("\n=== session summary ===")
    print(f"current: {current_ver or 'NOT DETECTED'}")
    if do_flash:
        print(f"flash sent: {flashed} | new version: {new_ver or 'NOT CONFIRMED'}")


if __name__ == "__main__":
    main()
