"""PATH B: reflash the ESP32-C6 via its own ESP-AT firmware (AT+USEROTA).

Requires the LilyGo esp32c6_at_host_sdio_uart bridge running in slot 1 —
it pipes COM6 <-> AT-over-SDIO. This script drives the AT dialog:

  scout:  python c6-at-ota.py COM6 --scout
          (AT + AT+GMR only — proves the bridge + AT firmware, no changes)
  flash:  python c6-at-ota.py COM6 --ssid NAME --pass PW --url http://IP:8000/na.bin

The URL must serve the TRIMMED network_adapter APP image (fits ESP-AT's
1856KB ota slot). 2.4GHz WiFi only (C6). OTA timeout per ESP-AT docs: 3 min.
"""
import sys
import time

import serial


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.write_timeout = 2
    s.dtr = False
    s.rts = False
    s.open()
    # best-effort reset so the launcher auto-boots the bridge fresh
    s.dtr = True
    s.rts = True
    time.sleep(0.2)
    s.dtr = False
    s.rts = False
    time.sleep(0.5)
    s.reset_input_buffer()
    return s


class At:
    def __init__(self, s):
        self.s = s
        self.buf = ""

    def feed(self):
        d = self.s.read(4096)
        if d:
            t = d.decode("utf-8", "replace")
            sys.stdout.write(t)
            sys.stdout.flush()
            self.buf += t

    def wait_for(self, tokens, timeout, fail_tokens=("ERROR",)):
        mark = len(self.buf)
        end = time.time() + timeout
        while time.time() < end:
            self.feed()
            tail = self.buf[mark:]
            for tk in tokens:
                if tk in tail:
                    return tk
            for tk in fail_tokens:
                if tk in tail:
                    return None
        return None

    def cmd(self, c, expect="OK", timeout=10):
        print(f"\n>>> {c}")
        self.s.write((c + "\r\n").encode())
        return self.wait_for([expect], timeout)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    scout = "--scout" in sys.argv

    def arg(name):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None

    ssid, pw, url = arg("--ssid"), arg("--pass"), arg("--url")
    if not scout and not (ssid and pw is not None and url):
        print("need --ssid --pass --url (or --scout)")
        sys.exit(2)

    s = open_port(port)
    at = At(s)
    print("waiting for bridge boot (draining)...")
    end = time.time() + 14
    while time.time() < end:
        at.feed()  # NEVER sleep blind: 4KB OS RX buffer overflows on boot logs

    # poke until the AT firmware answers ("\r\nOK" — bare "OK" appears in
    # boot logs like "memory test OK")
    ok = None
    for _ in range(10):
        ok = at.cmd("AT", expect="\r\nOK", timeout=3)
        if ok:
            break
    if not ok:
        print("\n!!! AT did not answer — is the bridge installed and booted in slot 1?")
        sys.exit(1)

    at.cmd("AT+GMR", timeout=5)

    if scout:
        print("\n=== scout complete: bridge + ESP-AT confirmed ===")
        s.close()
        return

    if not at.cmd("AT+CWMODE=1"):
        sys.exit(1)
    if not at.cmd(f'AT+CWJAP="{ssid}","{pw}"', expect="OK", timeout=30):
        print("\n!!! WiFi join failed")
        sys.exit(1)

    print(f"\n>>> AT+USEROTA={len(url)}")
    s.write(f"AT+USEROTA={len(url)}\r\n".encode())
    if not at.wait_for([">"], 10):
        print("\n!!! USEROTA did not prompt for URL")
        sys.exit(1)
    s.write(url.encode())  # raw, no CRLF
    if not at.wait_for([f"Recv {len(url)} bytes"], 10):
        print("\n!!! URL not acknowledged")
        sys.exit(1)
    print("\n... OTA transfer running (up to 3 minutes) ...")
    result = at.wait_for(["OK"], 240)
    if result:
        print("\n=== USEROTA reported OK — C6 reboots into network_adapter ===")
        # keep echoing a bit to catch any post-OTA output
        end = time.time() + 8
        while time.time() < end:
            at.feed()
    else:
        print("\n!!! USEROTA FAILED (ERROR or timeout). C6 is still on ESP-AT — safe to retry.")
        sys.exit(1)
    s.close()


if __name__ == "__main__":
    main()
