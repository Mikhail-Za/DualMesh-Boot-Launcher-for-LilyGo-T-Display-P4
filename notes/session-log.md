# T-Display P4 DualMesh — Session Log

Project goal: SD-card firmware launcher (M5-Launcher style) for the LilyGo T-Display P4 —
boot-switch between Meshtastic and MeshCore (MeshOS) without a PC, plus unlock unused
P4 silicon (camera/H.264, audio, IMU, LP core, keyboard expansion).

## Hardware inventory

- **Unit A (test/dev):** T-Display P4, TFT SKU (HI8561 540x1168, punch-hole front camera).
  All experiments happen on this unit.
  - MAC: `60:55:F9:FA:FC:5D` (MeshOS license serial derives from MAC — constant across reflashes)
  - Chip: ESP32-P4 **revision v1.0**, 16 MB GigaDevice flash (mfr c8, dev 4018)
  - REV NOTE: early-rev P4 (<v3.0) reportedly faults with bootloaders targeting newer
    steppings — set `CONFIG_ESP32P4_REV_MIN` / min-rev appropriately in all our builds.
  - USB serial: CH343 bridge → **COM6**
- **Unit B (reference):** identical, stays 100% stock MeshOS. Comparison + RF link partner.
- **SD card:** 64 GB, already FAT32, lives in Unit A. Contains MeshOS data
  (dm_history.bin, adverts.bin, notifications/, voice/, debug logs). Keep files; launcher
  adds its own /firmware/ folder alongside.
- License key + purchase emails: saved by Zaid (off-machine). Key is 88-char Ed25519,
  device-bound, offline-validated, reusable forever on same device.

## Decisions

- Architecture: factory-partition launcher + OTA slots as "active bays"; instant switch if
  resident, SD→flash copy (~10-30 s) to install. NO merged dual-stack firmware (prior art
  failed: Derek000 T-Deck TDM, shelved).
- SD card stays FAT32; MeshOS files preserved.
- MeshOS handling rule: NEVER partial-flash over an activated install (tamper canary per
  meshoskey.com RE docs). Always full-erase or clean-NVS path; re-enter key after.
- Partition naming plan: MeshOS keeps default `nvs`/`spiffs` names; our Meshtastic build
  patched to `mesh_nvs`/`mesh_fs`.

## 2026-06-12 — Phase 0: backups + flash dump

1. Installed esptool 5.3.0 (pip, Python 3.14).
2. SD snapshot → `backups/sdcard-unitA-2026-06-12/` (44.9 KB, robocopy /E, clean exit).
3. `esptool --port COM6 flash-id` → confirmed chip/flash as above.
4. Full 16 MB read-only dump → `backups/flash-dumps/unitA-meshos-stock-2026-06-12.bin`
   (esptool read-flash 0 0x1000000 @ 921600 baud).
5. Dump verified: 16,777,216 bytes, sha256
   `bfc20313fef7bbe5cb48343bb5603251e40c56bdc18179327037058bad5b78ae`.

## Findings from the dump

**Stock MeshOS partition table** (parsed @0x8000 via tools/parse_partitions.py):

| label    | type | subtype | offset  | size     | note |
|----------|------|---------|---------|----------|------|
| nvs      | data | nvs     | 0x9000  | 0x6000   | license cache lives here |
| phy_init | data | phy     | 0xF000  | 0x1000   | |
| factory  | app  | factory | 0x10000 | 0xF00000 | one giant 15MB app slot |

- **No SPIFFS/LittleFS partition at all** — MeshOS stores data on SD card. Removes the
  whole filesystem-conflict problem from the dual-boot design.
- **MeshOS app image is HUGE: 10,683,632 bytes (10.19 MB)** — segment 0 is ~9MB of
  DROM (UI assets/fonts/emoji baked in). Extracted + trimmed + esptool-validated:
  `backups/flash-dumps/extracted/meshos-app-dbac5b1-trimmed.bin`
  sha256 `00800b596a9cbf6aa4bfef49c67f5ce30a18cad7214606597c1d8a4a6d90cd20`
  (project 't-display-p4', version 'dbac5b1-dirty', entry 0x4ff0040a, DIO @80MHz,
  min rev v0.1, **max rev v1.99** — consistent with our v1.0 silicon).
- This was the only way to get a MeshOS .bin (no public download exists) — acquired
  from Zaid's own licensed device for personal dual-boot use. Do NOT redistribute.

## Draft shared partition table v1 (16MB)

| label      | type | subtype  | offset   | size     | MB    | note |
|------------|------|----------|----------|----------|-------|------|
| nvs        | data | nvs      | 0x9000   | 0x6000   | 24K   | same offset/size as stock → MeshOS license |
| otadata    | data | ota      | 0xF000   | 0x2000   | 8K    | boot selector |
| phy_init   | data | phy      | 0x11000  | 0x1000   | 4K    | moved; apps find it by type, not offset |
| factory    | app  | factory  | 0x20000  | 0xE0000  | 896K  | OUR launcher (LVGL + SD + flash writer) |
| meshos     | app  | ota_0    | 0x100000 | 0xAC0000 | 10.75M| image 10.19M + ~0.5M headroom |
| meshtastic | app  | ota_1    | 0xBC0000 | 0x280000 | 2.5M  | matches upstream app slot size |
| mesh_nvs   | data | nvs      | 0xE40000 | 0x10000  | 64K   | Meshtastic, patched name |
| mesh_fs    | data | littlefs | 0xE50000 | 0x1B0000 | 1.69M | Meshtastic, patched name |

Ends exactly at 0x1000000. All app slots 64KB-aligned.

Risks / open questions:
- MeshOS headroom only ~0.5MB — a future MeshOS update >10.75MB won't fit without
  re-partitioning (acceptable: launcher can always full-restore stock layout from SD).
- Unknown: does MeshOS care that it runs from ota_0 instead of factory subtype?
  Test early. Plan B: MeshOS in factory slot, launcher in ota_0 (escape hatch =
  erase otadata → boots MeshOS; less elegant but stock-like).
- Tamper canary: first boot of relocated MeshOS happens on FULLY ERASED flash
  (clean NVS) → trial mode → re-enter key. Never partial-flash over activated install.

## 2026-06-12 — Phase 1: toolchain + LilyGo SDK

- Cloned Xinyuan-LilyGO/T-Display-P4 branch `debug2` (HEAD bbd0e1a "Update firmware")
  → `T-Display-P4/` in project dir.
- **MeshOS provenance confirmed:** repo's `partitions_esp32p4.csv` (nvs 0x6000 + phy +
  factory 15M) is byte-identical to the stock table we dumped, and the dumped app's
  project name is 't-display-p4' → MeshOS is built on this LilyGo SDK.
- Repo structure: single IDF project; `main/Kconfig.projbuild` choice selects which
  example compiles (default `screen_lvgl`). Display SKU via `CONFIG_SCREEN_TYPE_HI8561`
  (TFT, ours) vs `CONFIG_SCREEN_TYPE_RM69A10` (AMOLED).
- Useful pins from `main/idf_component.yml`: **idf >=5.5.4** (README says 5.4 — wrong
  for debug2 HEAD), esp_hosted 2.11.5 + esp_wifi_remote 1.3.2 (C6 WiFi), LVGL 9.4.0.
- `libraries/radiolib_cpp_bus_driver` = RadioLib glue for ESP-IDF with XL9535-routed
  SX1262 control lines — big head start for the Meshtastic port.
- `firmware/` ships prebuilt C6 `network_adapter` (ESP-Hosted slave) binaries.
- ESP-IDF v5.4.3 cloned first (per old docs), then **re-cloned v5.5.4** to meet the
  manifest requirement → `C:\Users\user\esp\esp-idf-v5.5.4` (delete v5.4.3 once
  5.5.4 verified). Toolchain installer (`install.ps1 esp32p4`) run per IDF version.

## Build traps found (and fixes) — publishable gotcha list

1. **debug2 examples don't compile as shipped.** `CONFIG_SCREEN_TYPE_*` and
   `SCREEN_WIDTH/HEIGHT/BITS_PER_PIXEL` are consumed by the screen examples but defined
   nowhere (screen-type Kconfig choice dropped mid-refactor; even MeshOS's version string
   `dbac5b1-dirty` shows LilyGo builds from a dirty tree). Fix: added Kconfig choice in
   `main/Kconfig.projbuild` (HI8561 default / RM69A10) + CMake-derived SCREEN_* defines
   in `main/CMakeLists.txt` mirroring `t_display_p4_config.h` values.
2. **LilyGo repo submodules:** `libraries/*` are submodules; clone with `--recursive`
   or `git submodule update --init <libraries/...>` (skip the huge `apps/esp-at`).
3. **idf.py crashes with UnicodeEncodeError (cp1252) on Windows** while printing build
   output. Fix: `$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'` before idf.py.
4. **LVGL demo font missing:** `lv_font_montserrat_16` not enabled by default →
   `CONFIG_LV_FONT_MONTSERRAT_16=y` in sdkconfig.defaults.
5. **CHIP REVISION (the big one):** IDF 5.5.4 defaults to ESP32-P4 rev v3.1 minimum;
   LilyGo boards ship **v1.0 silicon**. esptool refuses to flash ("requires chip revision
   in range [v3.1 - v3.99]"). Pre-v3 and v3+ silicon are *mutually exclusive build
   targets* (different register layouts). Fix in sdkconfig.defaults:
   `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y`.
   This also explains MeshOS's image header max-rev v1.99. ANY firmware we build for
   this board (launcher, Meshtastic port) needs these two lines.
6. **First build validated:** screen_lvgl example, 1938 steps, `t-display-p4.bin`
   906KB (0xe28f0). IDF must be ≥5.5.4 per `main/idf_component.yml`.

## 2026-06-12 (overnight) — DUAL-BOOT PROVEN END TO END

Hardware smoke test (screen_lvgl demo on Unit A, serial-verified):
- Screen auto-detected `hi8561` (TFT SKU confirmed in software); display + touch +
  backlight init success. SX1262 init success. BQ27220/PCF8563/AW86224/ES8311/L76K/
  ICM20948 all success. XL9555 fail = keyboard board not attached (expected).
  SD fail = card in PC (expected). Full log: `notes/bootlog-screen_lvgl-demo.txt`.

Launcher v0 (`launcher/`): headless serial boot-selector in factory partition.
Commands: list/boot0/boot1/erase-otadata. Uses esp_ota_set_boot_partition (validates
target image before committing — empty slot can't brick). Builds to 232KB.

**THE BOOTLOADER PAIRING GOTCHA (root-caused empirically):**
- Our clean v5.5.4 launcher bootloader boots the launcher fine but MeshOS (idf 5.4.3)
  panics immediately (Load access fault right after pmu_pvt, during MSPI/PSRAM init).
  Two suspects: app-older-than-bootloader IDF pairing, and (more likely) our launcher
  sdkconfig had no SPIRAM/MSPI bootloader config while MeshOS expects it.
- FIX: use the STOCK bootloader extracted from the dump (0x2000-0x8000, IDF
  "v5.5-dirty" Dec 20 2025, min rev v0.1 max v1.99). It boots BOTH MeshOS and our
  5.5.4 launcher. `backups/flash-dumps/extracted/stock-bootloader.bin` is canonical.

**Validated cycle (all serial-verified, logs in notes/):**
1. Custom 8-partition table flashed; bootloader defaults to factory → launcher.
2. Launcher correctly identifies slot0 MeshOS image ('t-display-p4' dbac5b1, idf 5.4.3)
   and empty slot1.
3. `boot0` → MeshOS boots from ota_0 @0x100000. Fully operational: SX1262 transmitting
   LoRa adverts, ES8311 audio, UI running. **NVS untouched → loaded Zaid's username
   "Chessman-P4" + RF params (910.525MHz SF7 22dBm) + license cache. No re-activation
   needed.**
4. Return path: erase otadata (0xF000 0x2000) → bootloader defaults to factory →
   launcher. `boot0` again → MeshOS again. Round trip complete.
5. Device left in MeshOS state (otadata → ota_0), persistent across power cycles.

Tools: `tools/flash-dualmesh.ps1` (full layout flash), `tools/restore-stock.ps1`
(write 16MB dump back = guaranteed un-brick). SD card seeded: `G:\firmware\` with
meshos-dbac5b1.bin + screen-lvgl-demo.bin + README.

Reference clone for Phase 3: `reference/meshtastic-tdisplay-p4` (Homertrix ESP-IDF port).

## 2026-06-12 (day) — LAUNCHER V1 SHIPPED & USER-VALIDATED

Morning: Zaid visually confirmed relocated MeshOS — main screen, no license padlock,
all apps usable. SD card back in device.

Launcher v1 (`launcher/main/main.cpp`, C++): LVGL 9.4 touch UI + flash-from-SD.
- Runtime screen detection (one binary for TFT HI8561 + AMOLED RM69A10/GT9895)
- Selective driver init (power/XL9535/SGM38121/screen/touch/backlight/SDMMC only)
  → ~2.5s boot vs 5.3s full init
- Slot cards read live esp_app_desc from flash (no registry to go stale)
- Install: browse /sdcard/firmware/*.bin → tap-select → Install>Slot N → erase +
  64KB-chunk esp_partition_write with progress bar; validates 0xE9 magic + size≤slot
- boot buttons use esp_ota_set_boot_partition (verifies image first)
- Serial commands kept (list/boot0/boot1/erase-otadata)
- Binary: 830KB of 896KB slot (93% — trim fonts or rebalance partitions if it grows)

Build gotchas added to the list:
7. Library headers also need CONFIG_SCREEN_PIXEL_FORMAT_* / CONFIG_CAMERA_PIXEL_FORMAT_*
   (project-level Kconfig symbols) — supplied via launcher/main/Kconfig.projbuild.
8. Standalone projects consuming the LilyGo libs: declare all five libs with
   override_path in main/idf_component.yml (the libs' own registry deps have
   conditional `matches` that don't resolve reliably) + delete build/ after manifest
   changes (stale component-manager cache) + delete sdkconfig after adding Kconfig
   options (no auto-reconfigure pickup).

USER-VALIDATED on hardware: UI renders, touch works, file select works, installed
screen-lvgl-demo.bin from SD into slot1 (progress bar OK), booted slot1 demo, serial
return to launcher, boot0 back to MeshOS with config intact. Device ends in MeshOS.

## Next steps

1. Factory-reset GPIO escape hatch: rebuild bootloader with LilyGo's PSRAM config +
   CONFIG_BOOTLOADER_FACTORY_RESET (find BOOT button GPIO); verify it boots MeshOS —
   replaces PC-side otadata erase as the no-PC return path. Until then: getting back
   to launcher from MeshOS needs the PC (or reflash ota_data via launcher install).
2. Phase 3 Meshtastic port into ota_1: official firmware base + PR #9526
   HostedBluetooth rebase vs Homertrix reference (cloned in reference/); patch
   mesh_nvs/mesh_fs partition names; serial+TCP transports first, hosted BLE later.
   MUST keep rev-less-v3 sdkconfig lines and our partition CSV.
3. Launcher polish: scrollability check on long file lists, MeshOS-bay guard
   (warn before overwriting slot0), version/date in title, publish write-up.
