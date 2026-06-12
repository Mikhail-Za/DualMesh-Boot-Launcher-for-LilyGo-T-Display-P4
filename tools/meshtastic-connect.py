"""Meshtastic serial connection for the T-Display P4 (CH343 bridge).

pyserial's default DTR/RTS-asserted open holds this board in reset, so the
stock `meshtastic --port COMx` CLI can never connect. This shim opens the port
with DTR/RTS deasserted, drains the DualMesh launcher + meshtastic boot
(~14s; the port-open itself pulses reset), then talks to the node normally.

NOTE: self.stream must be assigned inside connect(), like the official
SerialInterface does — StreamInterface.__init__ resets self.stream to None,
so anything assigned before it is silently clobbered and all writes no-op.

Usage: python meshtastic-connect.py COM6 [--set-region US]
"""
import sys
import time

import serial
from meshtastic.stream_interface import StreamInterface


class QuietSerialInterface(StreamInterface):
    def __init__(self, port):
        self.port = port
        StreamInterface.__init__(self, connectNow=True)

    def connect(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = 115200
        s.timeout = 0.5
        s.write_timeout = 0
        s.dtr = False  # do NOT assert: holds the P4 in reset via CH343 circuit
        s.rts = False
        s.open()
        # Port-open still pulses a reset. Drain while the launcher splash +
        # meshtastic boot run — a plain sleep would overflow the OS RX buffer.
        print("waiting for device boot (launcher splash + meshtastic init)...")
        end = time.time() + 14
        while time.time() < end:
            s.read(4096)
        s.reset_input_buffer()
        self.stream = s
        super().connect()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    iface = QuietSerialInterface(port)
    print("\n=== NODE INFO ===")
    iface.showInfo()

    if "--set-region" in sys.argv:
        region = sys.argv[sys.argv.index("--set-region") + 1]
        from meshtastic.protobuf import config_pb2

        node = iface.localNode
        node.localConfig.lora.region = config_pb2.Config.LoRaConfig.RegionCode.Value(region)
        print(f"\nsetting lora.region = {region} (device will reboot)...")
        node.writeConfig("lora")
        time.sleep(2)

    iface.close()


if __name__ == "__main__":
    main()
