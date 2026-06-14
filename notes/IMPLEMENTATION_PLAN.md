# T-Display P4 — Implementation Plan (WORKING DRAFT v2)

> **STATUS: DRAFT — NOT FINAL.** Still iterating. Updated 2026-06-13 after a fable-mode
> research pass (6 subagents). Most facts below are now SOURCE-VERIFIED (file:line / issue#).
> Open decisions are at the bottom. Saved for power-failure resilience.

---

## Verified hardware facts (sourced 2026-06-13)

**C6 SDIO (for BLE/esp_hosted):** 4-bit, SDMMC **Slot 1**. CLK=18, CMD=19, D0=14, D1=15,
D2=16, D3=17. Clock 20 MHz. Source: `T-Display-P4/.../t_display_p4_config.h:125-132`,
`c6-updater/sdkconfig:2504-2538`.

**C6 reset/enable:** XL9535 I2C expander output **kIo14** (port1 bit4 / global bit12).
HIGH = C6 runs, LOW = C6 held in reset. NOT a P4 GPIO. Source:
`t_display_p4_config.h:46`, `t_display_p4_driver.cpp:1001/1018`.

**C6 recovery (CORRECTED — prior "internal test pads / open case" belief was WRONG):**
There is a **4-pin 1mm header exposed on the TOP of the device — no case opening**.
Wires: BLACK=GND, RED=3V3 (RED is NC when powered by USB-C). Source: LilyGo issue #14.
Procedure (issue #12, community-confirmed): press C6 BOOT button (GPIO9) edges 3-5x to
enter download mode; **hold P4 reset the whole flash** (else P4 resets the C6);
`esptool --chip esp32c6 -p COMx write-flash 0x0 <bin>`. LilyGo ships BOTH binaries in
their repo `firmware/`: factory `esp32c6_at_slave` AND `esp32c6_slave_esp_hosted_mcu_
network_adapter` (v2.0.17, [sdio]). Try first: `esptool ... erase_region 0xd000 0x2000`
(wipe otadata → fall back to intact AT in ota_0) — may un-brick with no reflash.

**BQ27220 battery gauge:** present, I2C **0x55**, on shared bus SDA7/SCL8. Source:
`t_display_p4_config.h:77-80,219-221`, datasheet in SDK. SoC reg 0x2C, Voltage reg 0x08.

---

## Part A — UI via EEZ Studio (ACTIVE)

**EEZ workflow VERIFIED (Agent C):** EEZ Studio v0.27.1 (github.com/eez-open/studio/
releases), opens LVGL 9.0 projects fine. **NO auto-reflow** — every px-unit widget keeps
its old coordinate; must be manually dragged/resized (240→540 = 2.25x W, 320→1168 = 3.65x
H = real manual effort). Build = Ctrl+B → writes screens.c etc. into destinationFolder.
Output dir must EXIST first. Fonts: Fonts panel generates from TTF via bundled lv_font_conv
(px size, bpp 4, glyph ranges). Check (Ctrl+K) validates before build. First app-save
reformats the whole JSON (one-time diff).

**DONE (Stage 2, 2026-06-13):** `studio/540x1168/TFTView_540x1168.eez-project` created
(displayWidth 540, displayHeight 1168, destinationFolder ../../generated/ui_540x1168);
`generated/ui_540x1168/` dir created. Verified by grep.

**Remaining wiring (deferred until after first EEZ Build — can't compile w/o generated dir):**
- `extra_script.py` already auto-maps `VIEW_540x1168` → `generated/ui_540x1168/` (no change).
- Add `VIEW_540x1168` to ViewFactory.cpp `#if` (reuse the resolution-agnostic
  `TFTView_320x240` class) — harmless, compiles with VIEW_240x320 still active.
- Switch platformio.ini `-DVIEW_240x320` → `-DVIEW_540x1168`, `DISPLAY_SIZE`, `LGFX_*`.
- STRIP the `apply_hotfix` v>800 reflow hacks (native EEZ layout makes them redundant).

**EEZ user steps:** install v0.27.1 → open `studio/540x1168/...eez-project` → confirm
540x1168 in Settings>General → reposition widgets (home + messages first) → Ctrl+K →
Ctrl+B → hand back to Claude for wiring + flash.

---

## Part B — Feature phases

### Phase 1 — Battery telemetry (BQ27220) ✅ DONE + VERIFIED 2026-06-13
Serial confirmed: "BQ27220 fuel gauge ready (4127mV, 100%)" + "Power: battery hardware
detected". Implemented as a guarded `TDP4BatteryLevel : HasBatteryLevel` in Power.cpp
reading 0x08/0x2C over IDF i2c_master on tdp4_i2c_bus (no cpp_bus_driver). Flag
`-DHAS_TDP4_BQ27220` in platformio.ini. (UI % render pending Zaid visual confirm.)

Goal: feed real SoC/voltage into Meshtastic's `HasBatteryLevel` so the (already-enlarged)
battery icon + % populate. Meshtastic's OCV voltage table fills % from voltage alone, so
even voltage-only works (`Power.cpp:254 / 767 / 922`).

**API (verified, Agent F):** `cpp_bus_driver::Bq27220` ctor takes `shared_ptr<BusI2cGuide>`
+ 0x55; `Init()`, `GetVoltage()`→mV, `GetStatusOfCharge()`→%; share bus via
`HardwareI2c1::set_bus_handle(tdp4_i2c_bus)` BEFORE Init.

**RISK / DESIGN CHANGE (self-critique):** `cpp_bus_driver` is **ESP-IDF-only**
(idf_component_register, needs IDF≥5.5.3, config.h pulls esp_lcd_mipi_dsi.h) — painful to
compile into our pioarduino build. **Recommended instead: write a ~30-line BQ27220 reader
using the IDF `i2c_master` API directly on `tdp4_i2c_bus`** (read 0x08 voltage, 0x2C SoC),
wrapped in a `HasBatteryLevel` subclass. Avoids cpp_bus_driver entirely. Integration:
custom subclass + register in `Power::setup()` before analogInit (guarded
`-DHAS_TDP4_BQ27220`); the only shared-file edit is one `#ifdef` block in Power.cpp.
Effort: LOW. (Charger IC SGM38121 on a 2nd bus → isCharging() later, out of scope.)

### Phase 2 — BLE via esp_hosted (PR #9526)
**Prereq:** reflash Unit A's C6 to `network_adapter` via the top 4-pin header (now LOW
effort — see recovery above). **Unit B C6 NEVER touched.**

**Recipe (verified, Agent D — PR #9526 @ branch crowpanel-p4):**
- Base sdkconfig inherited from `esp32p4_base` (CONFIG_ESP_HOSTED_ENABLED=y, NimBLE-over-
  hosted, CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c6", etc.).
- Our variant sdkconfig: `CONFIG_SOC_SDMMC_IO_POWER_EXTERNAL=n`,
  `CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y`, SDIO Slot1 **4-bit** 20000 kHz, pins
  CLK18/CMD19/D0-3=14-17. Leave reset-GPIO symbols UNSET.
- **Reset substitution:** PR's `setSlaveResetLine()` no-ops when reset GPIO is NC. Add a
  weak `hostedBluetoothSlaveReset(bool)` hook in `src/bluetooth/HostedBluetooth.cpp`
  (1 small shared edit); strong override in our `variant.cpp` toggles XL9535 kIo14.
- Build selection: `CONFIG_IDF_TARGET_ESP32P4` already gates `HostedBluetooth` in
  `main-esp32.cpp`; just DON'T set MESHTASTIC_EXCLUDE_BLUETOOTH. esp_hosted/esp_wifi_remote
  come via IDF component manager (no lib_deps). NimBLE already P4-guarded.
- File changes: variant platformio.ini (sdkconfig), variant.h (XL9535 consts), variant.cpp
  (release C6 reset early + strong hook), 1 weak-hook patch to HostedBluetooth.cpp,
  boards/*.json.
**Risks (unverified gaps):** XL9535 output state at cold boot (POR level of kIo14);
behavior of `SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y` when no reset GPIO is set (needs check vs
esp_hosted IDF source); reset→SDIO 1500ms timing; shared-file merge conflicts.

### Backlog / reference
GPS L76K (UART1 115200, RMC, XL9535 wake). IMU ICM20948 @0x68 conflicts with HI8561 touch
addr — schematic check. SGM38121 charger (2nd I2C bus, 0x28) for charge state. PCF8563 RTC,
ES8311 audio (unimplemented anywhere).

---

## Part C — Unit B deployment readiness
Unit B (COM8, MAC 30:ED:A0:E1:BF:57) dual-boots MeshOS + Meshtastic; its **C6 = factory
ESP-AT and must NEVER be flashed** (MeshOS needs AT). Goal: get the feature-complete
Meshtastic (display + touch + battery + GPS + presets) onto Unit B once Unit A is signed off.
The build is the SAME image for both units — no per-unit code.

**Deploy steps (do later, deliberately — Unit B is the pristine reference w/ MeshOS license):**
1. **Partition check (prereq):** the display build (~3.2MB) needs ota_1 = 3.625MB
   (partition v2). Confirm Unit B's partition table — if still v1 (2.5MB ota_1), migrate to
   v2 first (flash partitions-dualmesh.csv layout; PRESERVE nvs@0x9000 (MeshOS license),
   launcher@0x20000, meshos ota_0). This is the delicate step — do it carefully / with Zaid.
2. Flash current Meshtastic to ota_1 @0xBC0000 (same esptool flow as Unit A).
3. Boot the slot; one-time set `display.displaymode = COLOR(3)` via the client API so the
   on-screen UI runs (same as Unit A).
4. GPS: fresh config defaults GpsMode=ENABLED now (we removed GPS_DEFAULT_NOT_PRESENT), so
   GPS should self-start; battery (BQ27220) + presets are identical hardware/firmware.

**BLE TRADEOFF on Unit B (flag for Zaid — real conflict):** esp_hosted BLE requires the C6
to run `network_adapter`, but MeshOS needs the C6 on factory AT. They are MUTUALLY
EXCLUSIVE on one device. So Unit B can have EITHER (a) MeshOS + all non-BLE Meshtastic
features (C6 stays AT), OR (b) Meshtastic BLE by reflashing its C6 (which breaks MeshOS).
Recommendation: keep Unit B's C6 as AT (full MeshOS), accept no-BLE there; pursue BLE only
on Unit A (where we're already reflashing the C6 + MeshOS is already crash-on-dead-C6).
Decision deferred to Zaid.

## LOCKED DECISION 2026-06-13 (Zaid): FEATURES FIRST, UI LAST.
Do all functional implementation before any UI/EEZ reflow (avoids constant debug+UI
context-switching). New order: **BQ27220 battery (now) → C6 recovery (Zaid HW step) →
BLE → [then] EEZ UI reflow last.** UI scaffolding (studio/540x1168) already prepped and
parked until the end.

## OPEN DECISIONS (to lock the plan)
1. Phase order — SUPERSEDED by the locked decision above (UI last).
2. Battery: do BQ27220 now (via the ~30-line direct reader, low risk) or after EEZ?
3. C6 recovery: now NON-invasive (top 4-pin header + ~$2 cable + CP2102). Do it (unblocks
   BLE), or still park BLE?
4. RESOLVED: new-dir `ui_540x1168` + `VIEW_540x1168` (scaffolding done).
5. EEZ scope: home + messages first (recommend), or all screens?
6. Fonts: reuse generated Montserrat (simplest), or generate larger sizes from TTF in EEZ?

## Residual weaknesses (fable-mode self-critique)
- Phase 1's cleanest path REJECTS the SDK's cpp_bus_driver (IDF-only build pain) in favor
  of a hand-written BQ27220 reader — needs the register reads validated on hardware.
- Phase 2 has 3 unverified esp_hosted/expander-reset unknowns; first bring-up may need iteration.
- EEZ reflow is genuinely laborious; risk of UI churn returning. Mitigate: home+messages first.
- Shared-file edits (Power.cpp, HostedBluetooth.cpp) will conflict on upstream pulls — keep
  them minimal + `#ifdef`-guarded.
