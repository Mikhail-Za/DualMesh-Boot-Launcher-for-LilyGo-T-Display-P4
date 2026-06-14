# Unit B Deployment Plan (feature-complete Meshtastic) — VERIFIED RUNBOOK

> Built 2026-06-14 from a fable-mode pass (3 opus agents, all source-cited). Unit B is the
> PRISTINE REFERENCE holding the licensed MeshOS. Two hard rules at every step:
> **(1) NEVER flash Unit B's ESP32-C6** (factory ESP-AT is what MeshOS needs).
> **(2) NEVER erase/write `nvs@0x9000`** (the per-unit MeshOS license).

## Unit B current state (verified from session-log.md)
- Identity: MAC `30:ED:A0:E1:BF:57`, **COM8**, username `+Chessman2`, node `bf57`.
- Dual-boots MeshOS (ota_0) + Meshtastic `31e48a0` (the rollback-override build).
- **Partition layout = v1** (ota_1 = 2.5MB @0xBC0000). NOT yet migrated to v2. (session-log:783)
- esptool baud: **use `-b 460800`** (921600 died mid-transfer on Unit B). App/serial tools = 115200.
- A Unit-B full backup EXISTS: `backups/flash-dumps/unitB-meshos-stock-2026-06-12.bin` (16MB, has B's
  license) — but it predates dual-boot/v1, so take a FRESH dump before migrating.

## Why migration is SAFE (verified at the binary level)
v1->v2 changes ONLY Meshtastic-owned regions. These MeshOS-critical entries are byte-identical
v1==v2: `nvs@0x9000/0x6000`, `otadata@0xF000`, `phy_init`, `factory/launcher@0x20000`,
`meshos ota_0@0x100000/0xAC0000`. Only `ota_1` grows (0x280000->0x3A0000, same start 0xBC0000) and
`mesh_nvs`(->0xF60000) / `mesh_fs`(->0xF70000/0x90000) relocate (Meshtastic's own, safe to wipe).
Secure Boot + Flash Encryption are OFF in all build configs (verify eFuses on-device to be 100%).
Stock bootloader stays (do NOT reflash 0x2000 — launcher bootloader panics MeshOS).
The feature build is SAFE for the AT C6: `-DMESHTASTIC_EXCLUDE_BLUETOOTH=1` compiles out the entire
HostedBluetooth/esp_hosted path; `initVariant` never touches the C6 EN/wake bits.

## RUNBOOK (all on COM8; do in order)
0. **Confirm port = Unit B:** `python -m esptool --chip esp32p4 -p COM8 -b 460800 read_mac`
   -> must read `30:ed:a0:e1:bf:57` (Unit A is `60:55:f9:fa:fc:5d`). If not, sweep COM ports.
   Also (recommended) `espefuse.py --chip esp32p4 -p COM8 summary` -> confirm SECURE_BOOT_EN=0,
   flash-enc eFuses unburned.
1. **Full backup (read-only, SAFE):**
   `python -m esptool --chip esp32p4 -p COM8 -b 460800 read_flash 0 0x1000000 backups/flash-dumps/unitB-full-PREMIGRATION-2026-06-14.bin`
   verify size = 16,777,216; record sha256.
2. **NVS-only license backup:**
   `python -m esptool --chip esp32p4 -p COM8 -b 460800 read_flash 0x9000 0x6000 backups/flash-dumps/unitB-nvs-0x9000-2026-06-14.bin`  (keep a copy off-machine).
3. **Verify current layout = v1:** `python tools/parse_partitions.py backups/flash-dumps/unitB-full-PREMIGRATION-2026-06-14.bin`
   -> expect ota_1 @0xBC0000 size 0x280000. (If it reads 0x3A0000 it's already v2: skip steps 4-5.)
4. **Migrate partition table to v2 (table only @0x8000):**
   `python -m esptool --chip esp32p4 -p COM8 -b 460800 write-flash 0x8000 meshtastic-firmware/.pio/build/t-display-p4/partitions.bin`
   (use THIS v2 binary — NOT launcher/build/partition_table/partition-table.bin, which is STALE v1).
5. **Erase otadata (launcher regains control):**
   `python -m esptool --chip esp32p4 -p COM8 -b 460800 erase-region 0xF000 0x2000`
6. **Flash the Meshtastic app to ota_1 @0xBC0000:**
   `python -m esptool --chip esp32p4 -p COM8 -b 460800 write-flash 0xBC0000 meshtastic-firmware/.pio/build/t-display-p4/firmware-t-display-p4-2.8.0.ddfd796.bin`
   (the app .bin, NOT .factory.bin)
7. **displaymode = COLOR is AUTOMATIC** here: step 4 wiped mesh_nvs, so Meshtastic boots with no
   saved config -> installDefaultConfig() runs -> `-DHAS_TFT=1` defaults displaymode=COLOR
   (NodeDB.cpp:809). No manual API step needed (unlike Unit A, which had a stale pre-HAS_TFT config).
8. **Boot meshtastic slot + set region:**
   `python tools/boot-slot.py COM8 1` then `python tools/meshtastic-connect.py COM8 --set-region US`
   (GRUB one-shot preserved by verifyRollbackLater()=true in our build.)

## VERIFY (in order)
1. Launcher boots (parses v2 table). 2. **MeshOS (ota_0) boots = C6 AT intact** (`boot-slot.py COM8 0`;
   +Chessman2, no license padlock) -- THE critical safety check. 3. Meshtastic slot boots. 4. Display
   renders (MUI/COLOR). 5. Touch accurate. 6. Battery %. 7. Charging/VBUS. 8. All 9 LoRa presets in the
   dropdown. 9. GPS (Unit B's L76K -- UNKNOWN; Unit A's was silent/dead; if silent, same hardware
   question, NOT a deploy blocker). 10. RF pair test: `python tools/rf-pair-test.py COM6 COM8` -> A<->B PASS.

## GUARDRAILS — esptool on Unit B
SAFE: read_flash (backup); write-flash 0xBC0000 (app); write-flash 0x8000 (v2 partitions.bin);
erase-region 0xF000 0x2000 (otadata); recovery write-flash 0x0 unitB-*.bin (full restore).
FORBIDDEN: erase-flash (whole chip); erase/write anything spanning 0x9000 (nvs/license);
ANY --chip esp32c6 / c6-* tooling; write 0x2000 (bootloader); the stale v1 partition-table.bin;
restoring ANY unitA-*.bin to Unit B (wrong per-unit license).

## RISKS / unknowns
- eFuse secure-boot/flash-enc state not read on-device yet (step 0 espefuse confirms).
- Unit B GPS module population unknown (test #9; not a blocker).
- BLE: Unit B keeps AT C6, so NO esp_hosted BLE there (mutually exclusive with MeshOS). Unit B = all
  features EXCEPT BLE.
- Optional: copy the new app .bin into Unit B SD `/firmware/` for future PC-free launcher installs
  (only works AFTER v2 migration; the 3.2MB app exceeds a v1 2.5MB slot).
