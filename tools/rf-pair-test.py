"""First device-to-device RF test for the DualMesh Meshtastic port.

Connects Unit A (COM6) and Unit B (COM8) via the QuietSerialInterface shim,
sends a broadcast text from each, and verifies the other unit hears it over
LoRa (US / LongFast). Prints RSSI/SNR for each received packet.

Usage: python rf-pair-test.py [portA] [portB]
"""
import importlib.util
import sys
import time

from pubsub import pub

spec = importlib.util.spec_from_file_location(
    "mc", r"C:\Users\user\tdisplay-p4-dualmesh\tools\meshtastic-connect.py"
)
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

received = {}  # id(receiving interface) -> (text, rssi, snr)


def on_text(packet=None, interface=None):
    text = (packet.get("decoded") or {}).get("text")
    rssi = packet.get("rxRssi")
    snr = packet.get("rxSnr")
    received[id(interface)] = (text, rssi, snr)
    print(f"  RX on {interface.port}: {text!r}  rssi={rssi} dBm  snr={snr} dB")


def wait_rx(iface, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if id(iface) in received:
            return received.pop(id(iface))
        time.sleep(0.5)
    return None


def main():
    port_a = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    port_b = sys.argv[2] if len(sys.argv) > 2 else "COM8"

    pub.subscribe(on_text, "meshtastic.receive.text")

    print(f"connecting Unit A ({port_a})...")
    a = mc.QuietSerialInterface(port_a)
    print(f"connecting Unit B ({port_b})...")
    b = mc.QuietSerialInterface(port_b)

    print("\n--- A -> B ---")
    a.sendText("DualMesh RF test: hello from Unit A")
    r = wait_rx(b)
    ab = "PASS" if r else "FAIL (timeout)"
    print(f"A->B: {ab}")

    print("\n--- B -> A ---")
    b.sendText("DualMesh RF test: hello from Unit B")
    r2 = wait_rx(a)
    ba = "PASS" if r2 else "FAIL (timeout)"
    print(f"B->A: {ba}")

    a.close()
    b.close()
    print(f"\n=== RESULT: A->B {ab} | B->A {ba} ===")


if __name__ == "__main__":
    main()
