# DualMesh Boot Launcher for LilyGo T-Display P4

A touch-screen firmware launcher (M5Launcher-style) for the **LilyGo T-Display P4**
(ESP32-P4). Boot-switch between **MeshCore/MeshOS** and **Meshtastic** without a PC,
and install firmware images straight from the SD card.

**Status: working on hardware** (2026-06-12). MeshOS (the licensed closed-source
MeshCore firmware by Andy Kirby) runs relocated in an OTA slot with its license and
configuration fully intact; the launcher installs and boots additional firmware from
`/sdcard/firmware`. Believed to be the first multi-firmware boot solution for this board.

## How it works

A small LVGL touch UI lives in the `factory` partition. The ESP-IDF bootloader falls
back to it whenever `otadata` is erased, so the launcher is always reachable. App
firmwares live in OTA slots and are selected with `esp_ota_set_boot_partition()`
(which verifies the image before committing — an empty or corrupt slot cannot brick
the device).

### Partition table (16 MB)

| label      | type | subtype  | offset   | size     | purpose |
|------------|------|----------|----------|----------|---------|
| nvs        | data | nvs      | 0x9000   | 24 KB    | stock offset — MeshOS license + config survive |
| otadata    | data | ota      | 0xF000   | 8 KB     | boot selector |
| phy_init   | data | phy      | 0x11000  | 4 KB     | |
| factory    | app  | factory  | 0x20000  | 896 KB   | **this launcher** |
| meshos     | app  | ota_0    | 0x100000 | 10.75 MB | MeshOS bay (image is ~10.2 MB) |
| meshtastic | app  | ota_1    | 0xBC0000 | 2.5 MB   | flex bay (Meshtastic / other firmware) |
| mesh_nvs   | data | nvs      | 0xE40000 | 64 KB    | Meshtastic NVS (patched name) |
| mesh_fs    | data | spiffs   | 0xE50000 | 1.69 MB  | Meshtastic filesystem (patched name) |

MeshOS keeps the default `nvs` name at the stock offset, so its license cache and
user settings survive both the migration and every switch. MeshOS has no filesystem
partition — its data lives on the SD card.

## Features

- Slot cards with live app identification (reads `esp_app_desc` from flash)
- One-tap boot switching with pre-boot image verification
- Install any app-image `.bin` from `/sdcard/firmware` with progress bar
  (validates 0xE9 magic and slot fit before touching flash)
- Runtime display detection — one binary for both the TFT (HI8561) and
  AMOLED (RM69A10) SKUs
- Factory-reset escape hatch: after a reset, hold BOOT (GPIO35) ~2 s to erase
  `otadata` and return to the launcher (data partitions are preserved)
- Serial console fallback at 115200: `list` / `boot0` / `boot1` / `erase-otadata`
- ~2.5 s boot (selective driver init)

## Building

Requires **ESP-IDF v5.5.4** and the LilyGo SDK (as patched sibling checkout):

```
git clone --branch debug2 https://github.com/Xinyuan-LilyGO/T-Display-P4.git
cd T-Display-P4
git submodule update --init libraries/<each>   # skip apps/esp-at
git apply ../patches/lilygo-debug2-build-fixes.patch
cd ../launcher
idf.py set-target esp32p4 && idf.py build
```

On Windows, set `PYTHONUTF8=1` first or `idf.py` crashes printing build output.

## Hard-won gotchas (read before flashing anything)

1. **Chip revision:** LilyGo ships ESP32-P4 **rev v1.x** silicon. IDF defaults target
   rev v3.1+ — the pre-v3 and v3+ lines are *mutually exclusive build targets*. Every
   project needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y`.
2. **Bootloader PSRAM config:** a bootloader built without the board's SPIRAM/MSPI
   settings (hex PSRAM @ 200 MHz, 256 KB L2 cache) boots its own app fine but crashes
   MeshOS in early init with a Load access fault. Build the bootloader from this
   project (it carries the right config).
3. **Factory reset must not erase NVS:** `CONFIG_BOOTLOADER_DATA_FACTORY_RESET`
   defaults to `"nvs"` — which would wipe the MeshOS license cache. This project sets
   it to empty.
4. **GPIO35 is also the download strap:** hold BOOT *during* reset and the chip enters
   ROM download mode (tap RST to recover). The factory-reset gesture is
   tap-RST-then-hold-BOOT.
5. The LilyGo `debug2` SDK does not compile as shipped — see `patches/`.

Full narrative, boot logs, and the complete trap list: `notes/session-log.md`.

## What is NOT in this repo

No MeshOS / MeshCore / Meshtastic firmware binaries. MeshOS is Andy Kirby's
closed-source licensed firmware — extract the app image from **your own device's**
flash dump (`tools/parse_partitions.py` does this) and keep it private. The
open-source projects build from their own repos.

## Roadmap

- Meshtastic port for the T-Display P4 (into the flex bay; serial + WiFi-TCP
  transports first, BLE-over-ESP-Hosted after)
- Open-source MeshCore companion port (third firmware in the SD library)
- "GRUB mode": launcher always boots first with a tap-to-interrupt splash
- Self-recovery: restore a full stock dump from SD without a PC

## Credits

- LilyGo for the [T-Display-P4 SDK](https://github.com/Xinyuan-LilyGO/T-Display-P4)
- The MeshCore and Meshtastic communities
- Homertrix's [out-of-tree Meshtastic P4 port](https://github.com/Homertrix/meshtastic-tdisplay-p4) (Phase 3 reference)

## License

MIT for the code in this repository.
