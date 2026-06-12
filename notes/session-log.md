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

## 2026-06-12 (later) — GIT + GITHUB + BOOTLOADER INDEPENDENCE + GRUB MODE

- Git repo initialized; pushed to github.com/Mikhail-Za/Boot-Launcher-for-T-Display-P4-Liliygo-
  (main branch; public README; backups/ + clones excluded; LilyGo fixes as patches/).
  Global git identity set on this machine (was unset — root cause of "identity unknown").
- **Bootloader independence:** our 5.5.4 bootloader boots MeshOS once it carries the
  SPIRAM/MSPI config → earlier crash root-caused as missing PSRAM bootloader config,
  NOT IDF version pairing. Stock bootloader no longer needed.
- **Factory-reset GPIO abandoned with proof:** GPIO35 is the download strap (held at
  reset → ROM download mode, verified on hardware: boot:0x307) AND the bootloader
  samples the pin once ~30ms after reset (bootloader_common.c:45 returns GPIO_NOT_HOLD
  immediately) — the press-after-reset window is humanly impossible. Board has TWO
  boot buttons (P4 GPIO35 + C6 GPIO9) — fyi for C6 flashing later.
- **GRUB mode shipped + validated:** CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE makes every
  boot one-shot (mesh firmwares never confirm the image → every reset falls back to
  factory launcher). Launcher erases otadata at startup (no stale fallback targets),
  stores last-used slot in mesh_nvs namespace "launcher" (MeshOS "nvs" = read-only),
  3s tap-to-interrupt auto-boot splash. Hardware-validated: MeshOS → reset → splash
  countdown → auto-boot MeshOS. Commits: ded48bc…fc82673.

## 2026-06-12 (evening) — guard, license, Unit B fleet deployment

- Slot-0 overwrite guard shipped (confirm dialog; protects licensed MeshOS bay).
- LICENSE = GPL-3.0 (required: links LilyGo's GPL-3.0 driver libs). v0.1.0 tagged.
- Community post drafted (notes/draft-community-post.md), held for Zaid review.
- ESP-IDF v5.4.3 clone deleted (~3GB).
- **Unit B (MAC 30:ED:A0:E1:BF:57, COM8):** dumped (had to retry at 460800 baud —
  921600 died mid-transfer: "Packet content transfer stopped"), same hw rev v1.0,
  same stock layout, MeshOS app image **bit-identical to Unit A's** (sha256
  00800b59…) → image is device-agnostic, licenses live in NVS only. Full dual-boot
  layout deployed; launcher + MeshOS verified over serial — B's own identity
  (+Chessman2) and license intact. Both units now dual-boot. Commits → ecfc506.

## Plan: 3 firmwares (agreed with Zaid)

MeshOS permanently resident (ota_0, can't re-download). ota_1 = flex bay: Meshtastic
and open-source MeshCore both live on SD, launcher swaps in ~20-30s. Open MeshCore has
NO P4 support upstream either — it's a second port after Meshtastic (reuse XL9535 +
display + hosted-BLE work). Data partitions can be re-carved later without moving app
slots (only costs Meshtastic config).

## 2026-06-12 (night) — PHASE 3 KICKOFF: recon complete, toolchain proven

Recon (2 Sonnet agents + local verification), all major unknowns resolved:

**Meshtastic P4 state (better than expected):**
- `esp32p4_base` env MERGED in master (came with PR #9122): full ESP-Hosted sdkconfig,
  NimBLE-over-HCI-VHCI, MIPI-DSI SOC flags, C6 slave-OTA (CONFIG_OTA_METHOD_LITTLEFS).
  NO board variant uses it yet — ours would be the first.
- PR #9526 (crowpanel-p4 branch) ACTIVE, core maintainer involved; provides the
  variant file pattern: variants/esp32p4/<name>/{platformio.ini,variant.h,variant.cpp,
  pins_arduino.h} + boards/<name>.json (chip_variant "esp32p4_es" = v1.x silicon).
- pioarduino `custom_sdkconfig` accepts arbitrary Kconfig → our 5 chip-rev lines CAN
  be set per-variant. PR #9526 LACKS them (upstream gap; likely causes v1.x boot fails
  — our fix is contribution-worthy).
- Radio-behind-expander precedent: SenseCAP Indicator. Pattern: -DIO_EXPANDER=<i2c addr>
  -DIO_EXPANDER_IRQ=<gpio>, pins encoded (n | IO_EXPANDER). Support lives in
  mverch67's arduino-esp32 fork (pinned via platform_packages), NOT in meshtastic/src.
  Open: does that fork patch work on the P4 core, or do we wrap LockingArduinoHal.
- Display: LovyanGFX MIPI-DSI BROKEN on IDF 5.5.x (clk type mismatch, issue #788);
  M5GFX_Backport branch is the workaround. device-ui has elecrow-p4 branch (June 5).
  Display = last milestone, headless first.
- Confirmed local radio map: CS=24 BUSY=6 SPI=2/3/4 direct; RESET=XL9535.IO16,
  DIO1=XL9535.IO17, expander @0x20 INT=GPIO5.

**C6 question RESOLVED (clean reversible swap):**
- C6 runs ESP-AT today (MeshOS's modem); Meshtastic needs ESP-Hosted network_adapter.
  Mutually exclusive, reflash to switch — LilyGo ships prebuilts for BOTH in
  T-Display-P4/firmware/ (network_adapter v2.12.7 newest; esp32c6_at_slave v4.0.0).
- MeshOS degrades gracefully with wrong C6 fw (AT timeouts ignored; loses WiFi/NTP/map
  downloads only — already observed live in our boot logs).
- P4 can flash the C6 over SDIO, no wires: esp_hosted host_performs_slave_ota; working
  example github.com/lboshuizen/crowpanel-p4-c6-sdio-ota (must init SDIO w/o WiFi).
  Future launcher feature: auto-swap C6 fw to match booted app.
- C6 reset = XL9535.IO14 (LilyGo's CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=100 is a
  fake-pin hack); C6 wake = IO13. esp_hosted host/slave versions tightly coupled
  (v2.9.4+ baseline; mismatch = hard failure) — pin slave to Meshtastic's component ver.

**Toolchain proven:** unmodified `pio run -e t-deck` SUCCESS in 12m28s (clone at
meshtastic-firmware/, master 07a87a825, pio 6.1.19).

## Phase 3 plan (milestones)

A. **Variant skeleton + radio, headless** — variants/esp32p4/t-display-p4/ + board
   JSON; 5 chip-rev sdkconfig lines; radio pins w/ expander encoding (resolve the
   arduino-core-fork-vs-HAL-wrapper question); custom partition CSV matching OUR
   dualmesh table with mesh_nvs/mesh_fs names (patch Meshtastic's nvs/littlefs labels);
   USB-serial Client API. Exit test: launcher-installed meshtastic.bin in ota_1, official
   app connects over USB, radio inits. Then flash Unit B flex bay too → RF pair test.
B. **C6 + network** — flash C6 to network_adapter (UART or SDIO-OTA tool), enable
   WiFi via esp_wifi_remote, app over TCP:4403; then hosted-BLE attempt (configs
   already in esp32p4_base). Document the C6-swap procedure for MeshOS users.
C. **Display + MUI** — LovyanGFX DSI fix (M5GFX_Backport) or HI8561 esp_lcd driver
   port; device-ui config (elecrow-p4 branch as reference).

Upstream strategy: develop on local branch `t-display-p4-variant`; PR to
meshtastic/firmware once Milestone A works (they want P4 boards; our chip-rev fix
helps every v1.x P4 owner).

## 2026-06-12 — MILESTONE A IN PROGRESS: Meshtastic BOOTS on T-Display P4

Branch `t-display-p4-variant` (based on pr9526 = crowpanel-p4 + develop; fetched via
`git fetch origin pull/9526/head:pr9526`). Variant committed at bf0f5cc34.

**WORKING:** Meshtastic 2.8.0 boots from ota_1 under the launcher: NodeDB, fresh
LittleFS on mesh_fs (storage isolation works), default config saved, serial console
logs, HostedBluetooth setup runs (releases C6 from reset). XL9535 expander handle
inits OK in initVariant (err=0). Image 1.48MB = 57% of slot.

**Build recipe traps (in order hit):**
1. ng-io-expander fork (mverch67/arduino-esp32#82aee17, pinned via platform_packages)
   has broken P4 rule: espressif/cbor ^2.0.12 → custom_component_remove espressif/cbor.
2. Fork's esp32-hal-spi.c P4 LDO workaround needs BOARD_SDMMC_POWER_PIN/CHANNEL/
   ON_LEVEL defines + peri-tag block in initVariant (copied from CrowPanel).
3. variant.cpp only compiles with build_src_filter += +<../variants/esp32p4/t-display-p4>.
4. **Binary filename includes git hash** — after committing, flash the NEW
   firmware-t-display-p4-2.8.0.<hash>.bin (stale-flash cost one debug round).
5. Boot for tests: reset, wait ~3s, spam 'boot1' over serial into the launcher
   splash window (launcher auto-boots last slot after 3s otherwise).

**OPEN BUG (the only blocker): SX126x init result -707** (SPI_CMD_FAILED — chip
responds, reports command failure). Same with expander hardware reset (rst=78) and
RADIOLIB_NC. No TCXO on this board (LilyGo example AND Homertrix port both run
without TCXO/DIO2-switch; Homertrix uses cpp_bus_driver Sx126x not RadioLib;
sync word 0x2B/0x24B4). Current experiment (build running at session end): explicit
reset pulse in initVariant (bit14 low 5ms→high 20ms) + RADIOLIB_DEBUG_BASIC/SPI=1
to dump actual SPI status bytes → notes/bootlog-meshtastic-6.txt.
Next suspects if unresolved: Arduino-P4 SPI host/clock-source quirk (LilyGo example
explicitly uses SPI2_HOST, clock_source 11, 10MHz; check what Arduino SPI.begin
picks on P4), BUSY pulldown handling, compare RadioLib hal SPI transactions vs
LilyGo's working cpp_bus_driver traffic.

Expander pin map (validated): XL9535@0x20 bus SDA7/SCL8, INT=GPIO5. esp_io_expander
bit numbers: 3V3=0(active low), SKY13453=1(high=RF1), 5V=6(high), VCCA=8(low),
GPS wake=9, C6 wake=11, C6 EN=12, SD EN=13(low), SX1262 RST=14, DIO1=15.
LilyGo enum kIoN: N≤7 → bit N; N≥10 → bit N-2.

## RADIO WORKING (same session): SX126x init result 0
Root cause of -707: HPD16A has DIO3-powered TCXO @1.6V (cpp_bus_driver sx126x.h defaults enable_dio3_tcxo=true, kOutput1600Mv). Fix: SX126X_DIO3_TCXO_VOLTAGE 1.6 in variant.h. Meshtastic 2.8.0 + working SX1262 on T-Display P4 (commit in meshtastic clone). Next: set region US via meshtastic CLI over COM6 (pip install meshtastic; meshtastic --port COM6 --set lora.region US), official app over USB serial, then deploy to Unit B flex bay for RF pair test.
