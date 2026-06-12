"""One-shot check of the node's LoRa config via the connect shim."""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location(
    "mc", r"C:\Users\user\tdisplay-p4-dualmesh\tools\meshtastic-connect.py"
)
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
iface = mc.QuietSerialInterface(port)
lora = iface.localNode.localConfig.lora
from meshtastic.protobuf import config_pb2

region = config_pb2.Config.LoRaConfig.RegionCode.Name(lora.region)
preset = config_pb2.Config.LoRaConfig.ModemPreset.Name(lora.modem_preset)
print(f"REGION: {region} | usePreset: {lora.use_preset} | preset: {preset} | txEnabled: {lora.tx_enabled}")
iface.close()
