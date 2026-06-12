"""Raw Meshtastic serial-API probe — bypasses the python client entirely.

Opens the port (DTR/RTS deasserted; the open pulses reset), waits out the
launcher + app boot while echoing logs, then sends the protocol wake bytes
and a hand-framed ToRadio{want_config_id} and prints everything received,
flagging any 0x94 0xC3 protobuf frames.

Usage: python api-probe.py [COM6]
"""
import sys
import time

import serial
from meshtastic.protobuf import mesh_pb2

START1 = 0x94
START2 = 0xC3


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()

    print("--- boot phase (16s), echoing logs ---")
    end = time.time() + 16
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode("utf-8", "replace"))
    s.reset_input_buffer()

    to_radio = mesh_pb2.ToRadio()
    to_radio.want_config_id = 42
    payload = to_radio.SerializeToString()
    frame = bytes([START1, START2, len(payload) >> 8, len(payload) & 0xFF]) + payload

    print("\n--- sending wake (32x 0xC3) + wantConfig frame:", frame.hex(), "---")
    s.write(bytes([START2]) * 32)
    time.sleep(0.2)
    s.write(frame)

    print("--- listening 15s ---")
    buf = b""
    end = time.time() + 15
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
    s.close()

    # Split protobuf frames out of the byte stream
    frames = []
    i = 0
    while i < len(buf) - 4:
        if buf[i] == START1 and buf[i + 1] == START2:
            ln = (buf[i + 2] << 8) | buf[i + 3]
            if ln <= 512 and i + 4 + ln <= len(buf):
                frames.append(buf[i + 4 : i + 4 + ln])
                i += 4 + ln
                continue
        i += 1

    print(f"\n--- received {len(buf)} bytes, {len(frames)} protobuf frame(s) ---")
    if not frames:
        print("ASCII view of last 2000 bytes:")
        print(buf[-2000:].decode("utf-8", "replace"))
    got_complete = False
    for n, fr in enumerate(frames):
        fromr = mesh_pb2.FromRadio()
        try:
            fromr.ParseFromString(fr)
            kind = fromr.WhichOneof("payload_variant")
            extra = ""
            if kind == "config_complete_id":
                got_complete = True
                extra = f" = {fromr.config_complete_id}"
            print(f"[frame {n:2}] {kind}{extra}")
        except Exception as e:
            print(f"[frame {n:2}] parse error {e}: {fr.hex()[:100]}")
    print(f"\nconfig_complete_id received: {got_complete}")


if __name__ == "__main__":
    main()
