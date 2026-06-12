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

## Milestone A status at session end (2026-06-12)
DONE: Meshtastic 2.8.0 boots from ota_1, SX1262 init result 0 (TCXO 1.6V fix), ESP-Hosted SDIO link to C6 ACTIVE (H_SDIO_DRV Card init success). Blank screen = expected (HAS_SCREEN 0; display = Milestone C).
REMAINING (exit test): client API link. Findings: (1) pyserial/CLI default DTR+RTS-asserted open HOLDS the P4 in reset via CH343 — zero bytes; tools/meshtastic-connect.py shim opens with both deasserted; (2) even via shim, protobuf API does not answer on UART0/COM6 though logs do. NEXT EXPERIMENT: enable native USB-CDC console like CrowPanel P4 envs do (ARDUINO_USB_CDC_ON_BOOT / -DARDUINO_USB_MODE) — API should appear on the SECOND USB-C port (P4 native USB OTG) as a new COM device; plug that port in. Alternative: investigate SerialConsole/PhoneAPI binding on UART0.
Boot timing note: port-open pulses reset; full boot via launcher ~12s — any client must tolerate this.

USB experiment (Zaid, 2026-06-12): second USB-C port does NOT enumerate (no COM port, nothing in Device Manager) with current CDC-less Meshtastic build — expected, since no USB stack runs. Retest after building with ARDUINO_USB_CDC_ON_BOOT; if still dead, the port may be power-only/OTG-host-wired — then fall back to UART0 PhoneAPI debugging or WiFi-TCP (Milestone B).

## 2026-06-12 (cont): client-API blockers root-caused — TWO stacked bugs
Plan revision: the "enable native USB-CDC" experiment is DEAD WRONG for this board.
CrowPanel P4 envs actually set ARDUINO_USB_CDC_ON_BOOT=0 / ARDUINO_USB_MODE=1 — the
API is served on plain UART0. And the P4's FS-USB pin (GPIO24, USB-Serial-JTAG D-)
is wired to LoRa CS on the T-Display P4, so the second USB-C can only ever be the
HS OTG PHY (TinyUSB project, not a flag). Do not revisit USB-CDC for the API.

Bug 1 — SERIAL_HAS_ON_RECEIVE (esp32-common.ini) makes SerialConsole sleep
(INT32_MAX) until HardwareSerial::onReceive fires; on the P4/ng-io-expander core it
apparently never does → device streams logs but never READS the API request. Fix:
build_unflags = -DSERIAL_HAS_ON_RECEIVE in our env → console polls (250ms idle/5ms
active), same mode NRF52/RP2040 use. (Launcher's polling IDF console on the same
pins is what proved RX hardware was fine.)

Bug 2 — REBOOT LOOP every ~19s (this was the real CLI killer, present in all prior
captures but the restart fell outside our read windows). "Card init success" is
only SDIO card-level probe; the esp_hosted protocol handshake then times out after
10s ("Not able to connect with ESP-Hosted slave device") because the stock LilyGo
C6 firmware is ESP-AT, not the esp_hosted network_adapter slave. Driver resets the
C6 (GPIO[76] = expander bit 12 | 0x40), SDIO re-init fails (0x107), then
"H_SDIO_DRV: Host is resetting itself" → os_wrapper_esp: Restarting host →
SW_CPU_RESET. 3 boots per 60s captured (notes/boot-capture.txt, not committed).
Fix for Milestone A: -DMESHTASTIC_EXCLUDE_BLUETOOTH=1 (HostedBluetooth.cpp is
gated on it) — no hosted init, no loop. Milestone B re-enables BLE after C6 swap
to network_adapter v2.12.7.

New tools: tools/boot-slot.py (spams bootN through launcher window, echoes boot
log), tools/serial-monitor.py (passive capture, DTR/RTS deasserted).

## MILESTONE A EXIT TEST PASSED (2026-06-12)
Bug 3 (client-side, the last one): the shim assigned self.stream BEFORE
StreamInterface.__init__, which resets self.stream = None — every write
silently no-opped against a None stream. Official SerialInterface avoids this
by assigning self.stream inside its connect() override; shim now does the same
(and drains the RX buffer during the 14s boot wait instead of sleeping, so the
4KB Windows OS buffer can't overflow).

Verified end-to-end with tools/api-probe.py (raw wake + wantConfig, no client
lib): 45 FromRadio frames — my_info, metadata, 8 channels, 10 config,
16 moduleConfig, node_infos, fileInfo, config_complete_id=42. Then
tools/meshtastic-connect.py: full nodedb download, owner Meshtastic fc5d
(!79f1e7d3), firmware 2.8.0.ab6609f, PRIVATE_HW. lora.region set to US over
the API (device self-rebooted to apply).

Meshtastic clone commit: 7c8b810d5 (polling console + EXCLUDE_BLUETOOTH).
NEXT: copy bin to SD /firmware/, deploy to Unit B flex bay, first RF pair test.
Milestone B prereq confirmed by the reboot-loop diagnosis: C6 MUST be reflashed
with esp_hosted network_adapter v2.12.7 before re-enabling BLE/WiFi.

## CRITICAL TRAP FOUND + FIXED: Meshtastic captured the boot path (2026-06-12)
Symptom (Zaid): launcher splash gone — reset always booted straight into headless
Meshtastic (bootloader log: "Loaded app from partition at offset 0xbc0000", no
factory). Root cause: the P4 Arduino framework libs bake in
CONFIG_APP_ROLLBACK_ENABLE, so initArduino() (esp32-hal-misc.c) sees
PENDING_VERIFY on first boot and calls esp_ota_mark_app_valid_cancel_rollback()
→ otadata state VALID (0x2) → the launcher one-shot contract is broken and the
factory app becomes unreachable. MeshOS (pure IDF, no such call) never did this,
which is why GRUB mode validated fine before the Meshtastic era.

Fix (commit 31e48a00a, patch 0005): override the core's weak hook in variant.cpp —
`extern "C" bool verifyRollbackLater(void) { return true; }` — defers verification
forever; Meshtastic never marks itself valid. Verified on hardware twice: launcher
one-shot → meshtastic runs → reset → bootloader loads 0x20000 (factory launcher) →
splash → 3s → auto-boots meshtastic again. ANY future OTA-slot firmware we build
MUST carry this override (or its IDF equivalent: never cancel rollback).

Recovery recipe if a slot app ever captures boot again:
`esptool --chip esp32p4 -p COM6 erase-region 0xF000 0x2000` (or write
launcher/build/ota_data_initial.bin at 0xF000) → factory launcher returns.
Launcher serial cmd `erase-otadata` does the same when the launcher is reachable.

Debug tip that found it: otadata entry decode — bytes 0-3 ota_seq, 24-27 ota_state
(0=NEW 1=PENDING_VERIFY 2=VALID 4=ABORTED). serial-monitor.py grew a `reset` arg
(pulse DTR/RTS) for deterministic boot captures.

SD note: G:\firmware\ still holds the OLD bin (ab6609f, marks itself valid).
Replace with firmware-t-display-p4-2.8.0.31e48a0.bin next time the card is in the
PC — do NOT deploy ab6609f to Unit B.

## FIRST DEVICE-TO-DEVICE RF TEST PASSED (2026-06-12)
Unit B deployed via the launcher itself (Zaid touch-flashed 31e48a0 from SD —
field validation of flash-from-SD with a real firmware). Unit B node: Meshtastic
bf57, myNodeNum 847764431, COM8, API worked first try. Region US set on both.
tools/rf-pair-test.py: A->B and B->A broadcast texts both received,
rssi -60 dBm / snr ~6 dB at desk distance. The dual-boot Meshtastic port is now
functionally complete for headless mesh use on BOTH units.
Unit B SD card (the MeshOS data card — adverts.bin, dm_history.bin etc. left
untouched) seeded with /firmware/: 31e48a0 meshtastic + meshos-dbac5b1 recovery.
NEXT: Milestone B (C6 network_adapter swap -> BLE + WiFi/TCP), Milestone C display.

## MILESTONE B PLAN (researched + locked 2026-06-12)
Design principle (Zaid): STANDALONE-FIRST. Each firmware must be a fully working
standalone node; phone connectivity is additive, never traded against standalone
reliability. Zaid has never paired a phone to MeshOS — no regression exposure there.

Findings (2x sonnet web research + on-device probe):
- The C6 ALREADY RUNS esp-hosted (NOT ESP-AT): MeshOS boot log starts the hosted
  host stack (host_init: ESP Hosted, H_API, add_esp_wifi_remote_channels,
  hosted channels IF[1]/IF[2]). ESP-AT bootstrap fears moot.
- Our Meshtastic host driver = esp-hosted v2.12.3 (esp_hosted_host_fw_ver.h in
  framework-arduinoespressif32-libs). C6 slave is presumably the older factory
  build (v2.11.5 or v2.0.17 era) -> protocol/version mismatch is the leading
  theory for our "Not able to connect with ESP-Hosted slave" timeout.
- LilyGo ships THREE slave blobs locally in T-Display-P4\firmware\
  [T-Display-P4][esp32c6][network_adapter]\: v2.0.17, v2.11.5, v2.12.7
  (202605241554 = board-specific, matches our host minor). ESP-AT v4.0/4.1 blobs
  also present for ultimate rollback. NOTE: these are combined full-flash images;
  slave OTA needs the app-only image (extract at C6 app offset, or use ESPHome
  app-only mirror https://esphome.github.io/esp-hosted-firmware/).
- C6 reflash paths: (a) SDIO slave OTA from the P4 — works ONLY because slave
  already speaks hosted; reference impl github.com/lboshuizen/crowpanel-p4-c6-sdio-ota
  + espressif host_performs_slave_ota example; conservative 1-bit/10MHz for
  transfer. (b) Hardware fallback: C6 BOOT button (GPIO9) + USB-UART on C6
  UART pads (GPIO16 TX/17 RX, see schematic in T-Display-P4\information\),
  esptool --chip esp32c6 write-flash 0x0 <blob> (RISC-V: offset 0x0, NOT 0x1000).
  (c) P4-USB-bridge passthrough: broken upstream, do not chase
  (esp-dev-kits issue #134).

Plan:
1. Build "c6-updater" IDF app for the flex bay: connects to CURRENT slave
   (pin esp_hosted host component to a version the old slave accepts if 2.12.3
   refuses), LOGS the slave's current FW version (so we know the exact rollback
   target), then slave-OTAs the v2.12.7 app image. Install via our own launcher
   from SD — the launcher updates the C6, fully PC-free in principle.
   Watch: slave reset line is XL9535 bit 12 (expander), unreachable by the IDF
   gpio driver — disable driver-side slave reset, pulse the expander ourselves.
2. Meshtastic: remove MESHTASTIC_EXCLUDE_BLUETOOTH, rebuild, verify hosted
   handshake completes (no 19s loop), pair phone w/ official app over BLE.
3. Regression: boot MeshOS, confirm standalone works (it will — LoRa is on P4
   SPI, no C6 involvement), opportunistically try MeshCore app BLE pairing.
4. If anything regresses: OTA C6 back to the logged original version; ESP-AT/
   UART-pad recovery documented above as last resort.
5. Later (Milestone C): display/MUI. Then open MeshCore port, upstream PR.

## Milestone B session 1 (2026-06-12 afternoon): C6 truth discovered — it runs ESP-AT
Built c6-updater/ (IDF app for the flex bay: XL9535 read-modify-write power +
C6 power-cycle, esp_hosted host pinned ~2.12.3, embedded LilyGo network_adapter
v2.12.7 app image [extract via tools/extract-c6-app.ps1; image is 1.21MB after
0xFF trim], serial console: version/flash/reboot, never cancels rollback).
Build traps: CONFIG_ESP_HOSTED_ENABLED=y is the master switch (component
compiles to NOTHING without it); CP target gated on esp_wifi_remote's
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y (else silently defaults H2 → SDIO option
vanishes → SPI transport on wrong pins); esp_wifi_remote must be an explicit
dependency; after export.ps1 the IDF esptool needs underscore args
(default_reset) — silent flash failures otherwise; ccache keeps app_desc
compile-time stale (don't trust it for build identity).

DIAGNOSIS (the big one): SDIO card init succeeds but host drops frames —
len[25970]>max OR offset[25697]!=exp[12]. 0x6572/0x6461 = ASCII "re"/"da" —
the slave is sending TEXT ("ready") — **the factory C6 runs ESP-AT v4.0.0.0**
(LilyGo README line 357 confirms: "esp32c6_at factory program"). MeshOS starts
an esp-hosted HOST stack but its link must be failing the same way — its
WiFi/BLE phone features can never have worked on factory C6 (Zaid never used
them — consistent). Meshtastic's reboot-loop root cause = same.

C6 reflash paths (schematic H0405S002T002-V0 examined via pypdf):
- C6 UART0 (module pins RXD0/TXD0, nets C6_U0TXD/C6_U0RXD) goes NOWHERE except
  (probably) test pads on the C6 sheet — not on any header. 2x8P header carries
  P4 GPIOs only (26/27/32/33/ADC/53/54). Second USB-C = P4 HS-OTG (VBUS1
  USB_DP/DM on P4 sheet); CH343 owns the other (USB1_P/N). C6-MINI-1U module
  may not even expose IO12/13 (C6 USB).
- C6 BOOT button = C6_IO9 (strapping) exists on-board; C6 "reset" = power-cycle
  via XL9535 bit 12 (we control it).
- PATH A (hardware, safe): USB-UART adapter on C6 UART test pads + hold C6
  BOOT + power-cycle C6 → esptool --chip esp32c6 write-flash 0x0 <LilyGo blob>
  (combined image, offset 0x0, RISC-V). Fully reversible both directions.
- PATH B (software-only, riskier): drive ESP-AT over SDIO from a P4 app
  (LilyGo esp32c6_at_host_sdio_* examples prove AT-over-SDIO works), join WiFi
  (AT+CWJAP), then AT+USEROTA <url> serving the network_adapter APP image from
  the PC. Caveats: image would run under ESP-AT's partition table/bootloader
  (plausible but unproven); if it doesn't boot, C6 is soft-bricked until PATH A
  hardware access anyway. AT+USEROTA confirmed present in C6 AT v4.0.0.0 guide.
NEXT: Zaid inspects the PCB for labeled C6 UART test pads (decides A vs B).
c6-updater stays valuable post-swap: version query + future slave OTA tool.
