# DualMesh Boot Launcher for LilyGo T-Display P4

A touch-screen firmware launcher (M5Launcher-style) for the **LilyGo T-Display P4**
(ESP32-P4). Boot-switch between two firmware bays without a PC, and install
firmware images straight from the SD card. The launcher is firmware-agnostic:
any ESP-IDF app image that fits a bay can live in either slot.

**Status: working on hardware** since 2026-06-12 (two units, daily use), and
independently validated on a third device by
[pelgraine's fork](https://github.com/pelgraine/DualMesh-Boot-Launcher-for-LilyGo-T-Display-P4),
which used it to install and boot two other firmwares. Believed to be the first
multi-firmware boot solution for this board.

## How it works

A small LVGL touch UI lives in the `factory` partition. The ESP-IDF bootloader falls
back to it whenever `otadata` is erased, so the launcher is always reachable. App
firmwares live in OTA slots and are selected with `esp_ota_set_boot_partition()`
(which verifies the image before committing — an empty or corrupt slot cannot brick
the device).

Every boot handoff is **one-shot**: app rollback is enabled, the booted firmware
never confirms itself, so the next reset lands back in the launcher. A 3-second
tap-to-interrupt splash then auto-boots your last choice, which keeps the device
fully usable with no buttons and no PC.

### Partition table (16 MB)

| label      | type | subtype  | offset   | size     | purpose |
|------------|------|----------|----------|----------|---------|
| nvs        | data | nvs      | 0x9000   | 24 KB    | stock offset — the resident firmware's settings/license data survive |
| otadata    | data | ota      | 0xF000   | 8 KB     | boot selector |
| phy_init   | data | phy      | 0x11000  | 4 KB     | |
| factory    | app  | factory  | 0x20000  | 896 KB   | **this launcher** |
| slot0      | app  | ota_0    | 0x100000 | 10.75 MB | large bay (fits the stock firmware image) |
| meshtastic | app  | ota_1    | 0xBC0000 | 3.625 MB | flex bay (Meshtastic / other firmware) |
| mesh_nvs   | data | nvs      | 0xF60000 | 64 KB    | Meshtastic NVS (patched name) |
| mesh_fs    | data | spiffs   | 0xF70000 | 576 KB   | Meshtastic filesystem (patched name) |

The stock `nvs` partition keeps its stock offset and is treated as read-only, so a
relocated stock firmware finds its settings (and any license cache) exactly where
it left them, surviving both the migration and every switch. Meshtastic is patched
to use its own `mesh_nvs`/`mesh_fs`, so the two stacks never share storage.

## What runs on it

| firmware | slot | status |
|----------|------|--------|
| The firmware your board shipped with | slot0 | relocated from the factory slot with settings + license data intact (both my units) |
| Meshtastic 2.8.x (this repo's `patches/meshtastic/`) | ota_1 | working: display, touch, LoRa RF pair test, battery gauge, GPS |
| Meck-P4 v0.7.1 | either | reported working via [pelgraine's fork](https://github.com/pelgraine/DualMesh-Boot-Launcher-for-LilyGo-T-Display-P4) |
| Wadamesh beta_53 | either | reported working via [pelgraine's fork](https://github.com/pelgraine/DualMesh-Boot-Launcher-for-LilyGo-T-Display-P4) |

## Features

- Slot cards with live app identification (reads `esp_app_desc` from flash)
- One-tap boot switching with pre-boot image verification
- Install any app-image `.bin` from `/sdcard/firmware` with progress bar
  (validates 0xE9 magic and slot fit before touching flash)
- Slot-0 overwrite lock: a tap never flashes the resident bay; a deliberate
  long-press opens an explicit overwrite confirm, so a licensed image that cannot
  be re-downloaded is never lost to a stray tap
- Runtime display detection — one binary for both the TFT (HI8561) and
  AMOLED (RM69A10) SKUs
- Factory-reset escape hatch: after a reset, hold BOOT (GPIO35) ~2 s to erase
  `otadata` and return to the launcher (data partitions are preserved)
- Serial console fallback at 115200: `list` / `boot0` / `boot1` / `erase-otadata`
- ~2.5 s boot (selective driver init)

## Preparing a firmware image

The installer expects a plain **app image** (the `.bin` a build produces for the
app partition), not a merged/factory image:

- byte 0 must be `0xE9` (the ESP image magic; the installer checks this)
- it must fit the target bay (10.75 MB for slot 0, 3.625 MB for slot 1)
- copy it to the SD card under `/firmware/`, then install from the launcher

A merged image (bootloader + partition table + app in one file) will be rejected
or will not boot from a slot. If your build system only gives you a merged image,
extract the app partition first (`tools/parse_partitions.py` can split a dump).

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

If a newer `T-Display-P4` checkout fails resolving `usp_cpp_bus_driver`, the SDK
grew a new library dependency after this project's pinned state: add another
`override_path` entry for it in `launcher/main/idf_component.yml`, mirroring the
existing ones (pelgraine's fork carries an example).

## Hard-won gotchas (read before flashing anything)

1. **Chip revision:** LilyGo ships ESP32-P4 **rev v1.x** silicon. IDF defaults target
   rev v3.1+ — the pre-v3 and v3+ lines are *mutually exclusive build targets*. Every
   project needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y`.
2. **Bootloader PSRAM config:** a bootloader built without the board's SPIRAM/MSPI
   settings (hex PSRAM @ 200 MHz, 256 KB L2 cache) boots its own app fine but crashes
   the stock firmware in early init with a Load access fault. Build the bootloader
   from this project (it carries the right config).
3. **Factory reset must not erase NVS:** `CONFIG_BOOTLOADER_DATA_FACTORY_RESET`
   defaults to `"nvs"` — which would wipe the resident firmware's settings and any
   license cache. This project sets it to empty.
4. **GPIO35 is also the download strap:** hold BOOT *during* reset and the chip enters
   ROM download mode (tap RST to recover). The factory-reset gesture is
   tap-RST-then-hold-BOOT.
5. The LilyGo `debug2` SDK does not compile as shipped — see `patches/`.

Full narrative, boot logs, and the complete trap list: `notes/session-log.md`.

## What is NOT in this repo

No firmware binaries. Build the open-source projects from their own repos. If your
board shipped with a licensed firmware, extract the app image from **your own
device's** flash dump (`tools/parse_partitions.py` does this) and keep it private.

## Related: Trail Mate for the T-Display P4

If you want a full standalone outdoor OS for this board rather than a boot-switcher,
see [my Trail Mate fork](https://github.com/Mikhail-Za/trail-mate-T-display-p4-):
an offline-first handheld system (GPS navigation, offline maps, LoRa comms, team
awareness) with a long list of added apps: waypoints, compass, trip computer,
emergency beacon, walkie PTT, games, and more. It uses its own partition layout,
so flash it standalone rather than into a DualMesh slot.

## Roadmap

- Open-source MeshCore companion port (third firmware in the SD library)
- Self-recovery: restore a full stock dump from SD without a PC

## Credits

- LilyGo for the [T-Display-P4 SDK](https://github.com/Xinyuan-LilyGO/T-Display-P4)
- The MeshCore and Meshtastic communities
- Homertrix's [out-of-tree Meshtastic P4 port](https://github.com/Homertrix/meshtastic-tdisplay-p4) (Phase 3 reference)
- [pelgraine](https://github.com/pelgraine/DualMesh-Boot-Launcher-for-LilyGo-T-Display-P4)
  for independent hardware validation with additional firmwares and for
  documentation improvements adopted back into this README

## License

GPL-3.0 (see `LICENSE`). The launcher builds against LilyGo's GPL-3.0
`lilygo_device_driver` / `cpp_bus_driver` libraries, so GPL-3.0 is both the
right and the required choice for this project.
