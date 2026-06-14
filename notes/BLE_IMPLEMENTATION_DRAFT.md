# BLE-over-esp_hosted: Milestone B Implementation Draft
# LilyGo T-Display P4 / Meshtastic variant

**Status:** DRAFT — read-only; do not commit until Wednesday C6 reflash is verified.
**Correction (verification pass):** the slave-reset GPIO must be a REAL pin, not unset/-1 —
the esp-hosted-mcu host driver hard-asserts `assert(reset_pin.pin != -1)` in `sdio_drv.c`
(`ensure_slave_bus_ready`) and the Kconfig range is `[0,100]`. We now point both reset CONFIG
keys at harmless dummy GPIO 54, gate the driver reset behind `RESET_ONLY_IF_NECESSARY=y`, set
`RESET_DELAY_MS=200`, and keep the XL9535 bit-12 release in `initVariant()` as the real reset.
The `HostedBluetooth.cpp` hook patch (former Section 5) is dropped.
**Gate:** C6 must run `network_adapter` slave firmware (not ESP-AT) before removing the
exclude flag. Flipping the flag against an ESP-AT C6 causes a host reboot loop every ~19s
(see platformio.ini line 39-44 comment).

---

## 1. Inventory: What Exists vs. What Must Be Added

### Already in tree — NO action needed

| File | Status | Key evidence |
|------|--------|--------------|
| `src/bluetooth/HostedBluetooth.h` | EXISTS, complete (43 lines) | All methods declared; matches PR exactly |
| `src/bluetooth/HostedBluetooth.cpp` | EXISTS, complete (763 lines) | More developed than PR (574 lines); full GATT + queue implementation |
| `src/platform/esp32/main-esp32.cpp` | EXISTS, wired | Lines 7-13: P4-gated `#include`; lines 58-62: `new HostedBluetooth()` under `CONFIG_IDF_TARGET_ESP32P4` |
| `variants/esp32p4/esp32p4.ini` | EXISTS, complete | Lines 44-88: all `CONFIG_ESP_HOSTED_*` base sdkconfig keys present including `CONFIG_ESP_HOSTED_ENABLED=y`, NimBLE symbols, SDIO transport, SLAVE_RESET_ON_EVERY_HOST_BOOTUP |
| `variants/esp32p4/t-display-p4/pins_arduino.h` | EXISTS, complete | Lines 43-51: `BOARD_HAS_SDIO_ESP_HOSTED`, CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17, reset macro `(12 | IO_EXPANDER)` |
| `esp_hosted` / `esp_wifi_remote` components | Kept via `esp32p4_base` | `esp32p4.ini` `custom_component_remove` intentionally omits them (comment on line 22) |
| compile guard | Correct | `HostedBluetooth.cpp` line 3: `#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_ESP_HOSTED_ENABLED) && !MESHTASTIC_EXCLUDE_BLUETOOTH` |

**The entire software stack is already implemented.** The PR (#9526 / crowpanel-p4 branch) adds
nothing our tree does not already have — our local copy is ahead of the PR.

### Missing — must add before Wednesday build

| What | Where | Section below |
|------|-------|---------------|
| SDIO pin sdkconfig block (T-Display P4 pins) | `t-display-p4/platformio.ini` `custom_sdkconfig` | Section 2 |
| `CONFIG_SOC_SDMMC_IO_POWER_EXTERNAL=n` | same | Section 2 |
| `CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y` | same | Section 2 |
| `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=20000` | same | Section 2 |
| Dummy slave-reset GPIO 54 (BOTH reset CONFIG keys) + reset-only-if-necessary + 200ms delay | same | Section 2 |
| C6 reset (XL9535 bit 12) release early in `initVariant` | `variant.cpp` | Section 4 |
| Remove `-DMESHTASTIC_EXCLUDE_BLUETOOTH=1` flag | `t-display-p4/platformio.ini` line 44 | Section 7 (go-live step only) |

### NOT needed (do not add)

- A strong-override hook for `setSlaveResetLine`: not required. The real C6 reset is the
  XL9535 bit-12 release in `initVariant()` (Section 4), which runs BEFORE esp_hosted init,
  so the C6 is already up when SDIO enumerates. The driver's own reset path is pointed at a
  harmless dummy GPIO (Section 2) and is gated behind `RESET_ONLY_IF_NECESSARY`, so it never
  fires in the normal case.
- Any new `lib_deps` entries for esp_hosted/esp_wifi_remote: they come from IDF component
  manager automatically when `CONFIG_ESP_HOSTED_ENABLED=y` is set and the components are
  NOT in `custom_component_remove` (already correct in `esp32p4.ini`).

---

## 2. Exact `custom_sdkconfig` Addition for `t-display-p4/platformio.ini`

Insert at the END of the existing `custom_sdkconfig = ${esp32p4_base.custom_sdkconfig}` block
(currently lines 96-100 of `variants/esp32p4/t-display-p4/platformio.ini`).

```ini
custom_sdkconfig = ${esp32p4_base.custom_sdkconfig}
  # LilyGo ships ESP32-P4 rev v1.x silicon; pre-v3 and v3+ are mutually
  # exclusive build targets. Without these the image refuses to flash/boot.
  CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
  CONFIG_ESP32P4_REV_MIN_100=y
  # ---- Milestone B: BLE over esp_hosted (C6 via SDIO Slot 1) ----
  # Disable external SDMMC IO power control — avoid GPIO conflicts with our
  # LoRa/display pins that share the LDO channel (same fix as CrowPanel).
  CONFIG_SOC_SDMMC_IO_POWER_EXTERNAL=n
  # BLE 4.2 features (required for NimBLE-over-hosted; BLE 5.0 unsupported
  # on C6 via this transport).
  CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
  # SDIO clock: 20 MHz (conservative; crowpanel-50 uses this, crowpanel-70
  # uses 40 MHz — start low for first bring-up, raise if throughput matters).
  CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=20000
  # 4-bit bus mode
  CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
  # T-Display P4 SDIO Slot 1 pins (hardware-verified — LilyGo schematic)
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=18
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=19
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=14
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=15
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=16
  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=17
  # Resolved aliases — prevent stale template values in generated sdkconfig
  CONFIG_ESP_HOSTED_SDIO_PIN_CLK=18
  CONFIG_ESP_HOSTED_SDIO_PIN_CMD=19
  CONFIG_ESP_HOSTED_SDIO_PIN_D0=14
  CONFIG_ESP_HOSTED_SDIO_PIN_D1=15
  CONFIG_ESP_HOSTED_SDIO_PIN_D2=16
  CONFIG_ESP_HOSTED_SDIO_PIN_D3=17
  # D1 4-bit alias
  CONFIG_ESP_HOSTED_SDIO_PRIV_PIN_D1_4BIT_BUS=15
  # Slot selection: Slot 1 (required — our pins are on SDMMC host Slot 1
  # per ESP32-P4 TRM IOMUX table; Slot 0 uses different pins).
  CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
  # ---- Slave reset GPIO: MUST be a real, valid GPIO (NOT -1) ----
  # The esp-hosted-mcu host driver hard-asserts on this in
  # sdio_drv.c -> ensure_slave_bus_ready(): `assert(reset_pin.pin != -1)`.
  # Leaving it unset/-1 HARD-CRASHES at boot. The Kconfig range for
  # CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE is [0,100], so -1 is rejected anyway.
  # Our REAL C6 reset is the XL9535 bit-12 release in initVariant() (Section 4),
  # which runs before esp_hosted init — so the C6 is already up by the time SDIO
  # enumerates. We therefore point the driver's reset line at a HARMLESS unused
  # P4 GPIO (54 — a 2x8-header pin NOT wired to the C6; verified unused in
  # variant.h / variant.cpp / pins_arduino.h). The driver may toggle GPIO 54
  # during init, but the C6 never sees it.
  CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
  CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=54
  # Try SDIO enumeration FIRST; only toggle the dummy GPIO if enumeration fails.
  # Because the C6 is already running (via I2C reset), enumeration succeeds and
  # the dummy GPIO is never driven in the normal path.
  CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y
  # Reset settle delay: 200ms (default 1500ms is unnecessary — our real reset is
  # via I2C in initVariant(), which already settled before esp_hosted_init).
  CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=200
  # Active-high reset polarity (C6 EN = HIGH to run, LOW = held in reset).
  CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

**Diff (what changes vs current file line 96-100):**

```diff
 custom_sdkconfig = ${esp32p4_base.custom_sdkconfig}
   # LilyGo ships ESP32-P4 rev v1.x silicon; ...
   CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
   CONFIG_ESP32P4_REV_MIN_100=y
+  # ---- Milestone B: BLE over esp_hosted ----
+  CONFIG_SOC_SDMMC_IO_POWER_EXTERNAL=n
+  CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
+  CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=20000
+  CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
+  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=18
+  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=19
+  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=14
+  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=15
+  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=16
+  CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=17
+  CONFIG_ESP_HOSTED_SDIO_PIN_CLK=18
+  CONFIG_ESP_HOSTED_SDIO_PIN_CMD=19
+  CONFIG_ESP_HOSTED_SDIO_PIN_D0=14
+  CONFIG_ESP_HOSTED_SDIO_PIN_D1=15
+  CONFIG_ESP_HOSTED_SDIO_PIN_D2=16
+  CONFIG_ESP_HOSTED_SDIO_PIN_D3=17
+  CONFIG_ESP_HOSTED_SDIO_PRIV_PIN_D1_4BIT_BUS=15
+  CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
+  CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
+  CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=54
+  CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y
+  CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=200
+  CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

---

## 3. No Changes Needed to `esp32p4_base` (`variants/esp32p4/esp32p4.ini`)

All base esp_hosted symbols are already present (verified lines 44-88). The base config
sets `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y` with a 1500ms delay
(line 76 / comment on line 53). Do not touch `esp32p4.ini` — the per-variant
`custom_sdkconfig` block (Section 2) layers on top and overrides the reset behavior:
`CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` takes precedence over the
"reset on every host bootup" default, and `CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=200`
overrides the 1500ms base delay. Per-variant `custom_sdkconfig` entries are appended
after the base block in the generated sdkconfig, so the later (variant) values win.
If you see the old 1500ms delay or the dummy GPIO firing on every boot, confirm the
variant block actually landed (Step 1 cache delete) rather than editing the base.

---

## 4. `variant.cpp` Changes: C6 Reset via XL9535 (REAL reset path)

The C6 reset is on the XL9535 I2C expander, register bit 12 (HIGH = run, LOW = held in
reset). The single required `variant.cpp` change is to **release the C6 from reset early in
`initVariant`**, and this is the REAL reset path for our board.

This MUST happen before esp_hosted init: `initVariant()` runs before the esp_hosted/SDIO
stack comes up, so the C6 is already booted and on the bus by the time SDIO enumerates.
Allow ~200-300ms of settle after driving bit 12 HIGH so the C6's `network_adapter` firmware
has finished its own boot before enumeration begins.

### Why no strong-override hook is needed (corrected)

Earlier drafts proposed a strong-override / weak-hook for `setSlaveResetLine()` to drive the
expander from inside the driver. That is unnecessary and is dropped:

- The esp-hosted-mcu host driver **requires a real reset GPIO** — it hard-asserts
  `assert(reset_pin.pin != -1)` in `sdio_drv.c` (`ensure_slave_bus_ready`), and the Kconfig
  range for `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` is `[0,100]`. So we cannot leave the
  reset symbol unset; we point it at a harmless dummy GPIO (54) instead (Section 2).
- Because the C6 is already released and running via the XL9535 (above) before SDIO
  enumeration, and `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` makes the driver try
  enumeration FIRST, the driver never needs to toggle GPIO 54. If it ever did (enumeration
  failure), GPIO 54 is unwired to the C6, so the toggle is harmless and the C6 stays up under
  XL9535 control.

This removes the need for any patch to `HostedBluetooth.cpp` (former Section 5) and any
`extern "C"` hook. The only code change is Patch A below.

### Patch A: `initVariant` addition (the only variant.cpp change required)

In `variants/esp32p4/t-display-p4/variant.cpp`, inside the `if (io_expander)` block, after
the existing XL9535 setup (after line 88 `delay(20);`), add the C6 release:

```cpp
        // ---- Milestone B: release C6 (ESP-Hosted slave) from reset ----
        // C6 EN is XL9535 register bit 12 (LilyGo kIo14 / port1 bit4).
        // HIGH = C6 runs, LOW = held in reset. This is the REAL C6 reset path.
        // Runs BEFORE esp_hosted init, so the C6 is already booted when SDIO
        // enumerates. The driver's own reset GPIO is a harmless dummy (GPIO 54,
        // Section 2) gated behind RESET_ONLY_IF_NECESSARY and never fires here.
        // Settle ~200-300ms so the C6 network_adapter firmware finishes booting
        // before enumeration begins.
        esp_io_expander_set_dir(io_expander, 1 << 12, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander, 1 << 12, 1); // C6 out of reset
        delay(250); // settle before esp_hosted SDIO enumeration
        printf("[variant] C6 (ESP-Hosted slave) released from reset via XL9535 bit 12\n");
```

**Exact insertion point:** after line 88 (`delay(20);`) and before line 90 (`// DIO1 (bit 15)...`).

Full context for the edit:

```cpp
        esp_io_expander_set_level(io_expander, 1 << 14, 0);
        delay(5);
        esp_io_expander_set_level(io_expander, 1 << 14, 1); // out of reset  <- existing line 88
        delay(20);
        // DIO1 (bit 15) stays input; core pinMode handles it via attachInterrupt  <- existing line 90
```

Becomes:

```cpp
        esp_io_expander_set_level(io_expander, 1 << 14, 0);
        delay(5);
        esp_io_expander_set_level(io_expander, 1 << 14, 1); // out of reset
        delay(20);
        // ---- Milestone B: release C6 (ESP-Hosted slave) from reset ----
        // C6 EN is XL9535 register bit 12 (LilyGo kIo14 / port1 bit4).
        // HIGH = C6 runs, LOW = held in reset. REAL C6 reset path; runs before
        // esp_hosted init so the C6 is up when SDIO enumerates.
        esp_io_expander_set_dir(io_expander, 1 << 12, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander, 1 << 12, 1); // C6 out of reset
        delay(250); // settle before esp_hosted SDIO enumeration
        printf("[variant] C6 (ESP-Hosted slave) released from reset via XL9535 bit 12\n");
        // DIO1 (bit 15) stays input; core pinMode handles it via attachInterrupt
```

---

## 5. REMOVED: No `HostedBluetooth.cpp` reset-hook patch needed

**Correction:** earlier drafts proposed patching `HostedBluetooth.cpp` with a weak
`variantHostedSetC6Reset` hook so the driver could drive C6 EN via the I2C expander, on the
mistaken premise that the driver's reset GPIO would be left unset / a no-op. That premise is
wrong:

- The esp-hosted-mcu host driver **requires** a real reset GPIO. It hard-asserts
  `assert(reset_pin.pin != -1)` in `sdio_drv.c` (`ensure_slave_bus_ready`) — leaving the
  reset symbol unset/-1 HARD-CRASHES at boot. The Kconfig range for
  `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` is `[0,100]`, so -1 is rejected anyway.
- We satisfy the assert with a harmless dummy GPIO (54, Section 2) and gate the driver's
  reset path behind `CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y`. The REAL reset is
  the XL9535 bit-12 release in `initVariant()` (Section 4), which runs before esp_hosted init.

Net effect: **no change to `HostedBluetooth.cpp`**, no `extern "C"` hook, no `goto`. The only
code edit is Patch A in Section 4. The C6 stays powered (under XL9535 control) whenever
Meshtastic is running; `deinit()` does not power-gate it, which is fine for Milestone B
(C6 draws ~5mA idle).

---

## 6. Nothing New from PR #9526 / `crowpanel-p4` Branch

Confirmed by direct comparison:
- `src/bluetooth/HostedBluetooth.cpp` in our tree: 763 lines (more complete).
- PR branch (`crowpanel-p4`): 574 lines.
- `src/bluetooth/HostedBluetooth.h`: identical (43 lines).
- `src/platform/esp32/main-esp32.cpp`: functionally identical for the P4/BLE path.
- `variants/esp32p4/esp32p4.ini`: functionally identical (our local has clearer comments).

The PR's variant (`crowpanel-advanced-p4`) sdkconfig block is the reference for our sdkconfig
additions (Section 2 above) — we ported the SDIO pin keys from the crowpanel-70/90/101 env
which uses the same GPIO 18/19/14/15/16/17 pins as our board.

PR source refs used:
- crowpanel-advanced-p4 `platformio.ini` lines 187-196 (Slot 1 SDIO at 18/19/14-17)
- crowpanel-advanced-p4 `platformio.ini` lines 70-102 (full sdkconfig template)

---

## 7. Step-by-Step Apply + Build + Test Sequence (Wednesday)

### Pre-conditions (must be true before starting)
- [ ] C6 has been reflashed with `network_adapter` slave firmware (not ESP-AT).
  Flash command (from ESP-IDF environment, not Arduino):
  ```
  esptool.py --chip esp32c6 --port COMx write_flash 0x0 network_adapter.bin
  ```
  The binary is from: https://github.com/espressif/esp-hosted/releases
  (match the version pulled by IDF component manager — check
  `~/.platformio/packages/framework-espidf/components/esp_hosted/` for the version).
- [ ] The T-Display P4 is powered and connected to host PC.
- [ ] You can confirm the C6 appears on a UART console with `I (xxx) network_adapter:` boot log.

### Step 1: Delete cached sdkconfig (required — stale sdkconfig will override new keys)

```powershell
Remove-Item -Force -Recurse "C:\Users\user\tdisplay-p4-dualmesh\meshtastic-firmware\.pio\build\t-display-p4\sdkconfig" -ErrorAction SilentlyContinue
Remove-Item -Force -Recurse "C:\Users\user\tdisplay-p4-dualmesh\meshtastic-firmware\.pio\build\t-display-p4\" -ErrorAction SilentlyContinue
```

### Step 2: Edit `variants/esp32p4/t-display-p4/platformio.ini`

Add the sdkconfig block from Section 2 at the end of the existing `custom_sdkconfig` block
(after `CONFIG_ESP32P4_REV_MIN_100=y`, before the blank line / next section). This block MUST
include the four reset keys: `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54`,
`CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=54`,
`CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y`, and
`CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=200`. Do NOT leave the reset GPIO unset — the driver
hard-asserts `assert(reset_pin.pin != -1)` in `sdio_drv.c` and crashes at boot otherwise.

DO NOT touch anything else in the file yet.

### Step 3: Edit `variants/esp32p4/t-display-p4/variant.cpp`

Apply the `initVariant` addition from Section 4 (Patch A — the only code edit needed; there is
no `HostedBluetooth.cpp` patch, see Section 5).
Location: after line 88 (`delay(20);`), before line 90 (`// DIO1...`).

### Step 4: Remove the BLE exclusion flag (THE go-live flip)

In `variants/esp32p4/t-display-p4/platformio.ini` line 44, change:

```ini
  -DMESHTASTIC_EXCLUDE_BLUETOOTH=1
```

to a comment:

```ini
  ; Milestone B: BLE enabled — C6 reflashed with network_adapter slave.
  ; -DMESHTASTIC_EXCLUDE_BLUETOOTH=1
```

This is the last edit. Do not do this step until Steps 2 and 3 are done.

### Step 5: Build

```powershell
cd C:\Users\user\tdisplay-p4-dualmesh\meshtastic-firmware
pio run -e t-display-p4 2>&1 | Tee-Object build-milestone-b.log
```

Expected build-time warnings/notes:
- The first build after sdkconfig changes will regenerate the full sdkconfig (~2-3 min extra).
- You may see: `WARNING: CONFIG_ESP_HOSTED_SDIO_PRIV_PIN_D1_4BIT_BUS depends on...` — this is
  harmless; it means 4-bit mode is correctly selected.
- `nimble` sources are excluded from the P4 build by `esp32p4_base` `build_src_filter` (`-<nimble>`
  on line 16 of `esp32p4.ini`) — this is correct; NimBLE runs via the hosted transport.

Watch for these errors (would indicate problems):
- `undefined reference to HostedBluetooth` -> BLE exclusion flag still set; check Step 4.
- `CONFIG_ESP_HOSTED_SDIO_SLOT_1 not found` -> esp_hosted component version mismatch; see
  Risk R1 below.
- A boot-time `assert failed: ... sdio_drv.c ... reset_pin.pin != -1` / abort + reboot loop
  -> the reset GPIO symbol is unset or -1. Confirm `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54`
  (and `CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE=54`) actually landed in
  `.pio/build/t-display-p4/sdkconfig` (Step 1 cache delete + rebuild).
- A Kconfig range error on `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` -> the value is outside
  `[0,100]` (e.g. someone set -1); set it to 54.

### Step 6: Flash

```powershell
pio run -e t-display-p4 -t upload
```

Or via esptool directly (if OTA slot targeting is needed for dual-boot):

```powershell
esptool.py --chip esp32p4 --port COMx --baud 921600 write_flash 0x200000 .pio/build/t-display-p4/firmware.bin
```

(The 0x200000 address is the `meshtastic` OTA slot per `partitions-dualmesh.csv` — verify
against your partition table before flashing.)

### Step 7: Serial Monitor + Boot Verification

```powershell
pio device monitor -e t-display-p4 -b 115200
```

Expected log sequence on successful init:

```
[variant] xl9535 expander init: err=0 handle=0x...
[variant] C6 (ESP-Hosted slave) released from reset via XL9535 bit 12
...
I (xxxx) esp_hosted: transport up
I (xxxx) HostedBluetooth: ESP-Hosted transport is up
I (xxxx) HostedBluetooth: Hosted BLE advertising started
```

The critical line is `transport up` — that confirms the SDIO handshake completed. If you instead
see:

```
E (xxxx) esp_hosted: sdio init failed
```
or the host reboots after ~10s, the C6 is not running `network_adapter`. Stop. Do not proceed.

### Step 8: BLE Functional Test

1. Open Meshtastic app (iOS or Android).
2. Scan for BLE devices; the board should appear as `Meshtastic_XXXX`.
3. Pair and connect.
4. Verify the app shows radio info (region, firmware version, node count).
5. Send a test message; confirm it appears in the Meshtastic app and on the serial log.

Expected BLE log:

```
I (xxxx) HostedBluetooth: Hosted BLE incoming connection XX:XX:XX:XX:XX:XX
I (xxxx) HostedBluetooth: Hosted BLE authentication complete
```

---

## 8. Risks and Unknowns

### R1: esp_hosted component version vs. sdkconfig key names

**Risk:** The `CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_*_SLOT_1` key names are from the version
of `esp_hosted` pulled via IDF component manager by the crowpanel-p4 branch. Our tree pulls
the same IDF version but the key names changed between esp_hosted v0.0.7 and v0.0.8.

**Mitigation:** After `pio run`, open `.pio/build/t-display-p4/sdkconfig` and search for
`ESP_HOSTED_SDIO_PIN`. If the keys are named differently (e.g., `CONFIG_ESP_HOSTED_SDIO_CLK_PIN`),
update `custom_sdkconfig` to match.

### R2: SDIO slot detection / GPIO matrix routing

**Risk:** The T-Display P4 SDIO pins (18/19/14-17) are on Slot 1 of the ESP32-P4's SDMMC
controller. If the esp_hosted driver defaults to Slot 0 and the Kconfig key
`CONFIG_ESP_HOSTED_SDIO_SLOT_1=y` is not recognized, it will try to use wrong pins.

**Mitigation:** After flash, the serial log will show the resolved SDIO pins:
`I (xxx) sdmmc_sdio: using CLK pin: xx, CMD pin: xx, D0 pin: xx`. Verify these match
18/19/14. If they show 43/44/39 (Slot 0 defaults), the `SLOT_1` key was not applied —
check sdkconfig and clean-rebuild.

### R3: C6 release in `initVariant` must complete before esp_hosted init

**Risk:** The XL9535 bit-12 release in `initVariant()` is the REAL C6 reset path and must run
(with its ~250ms settle) before the esp_hosted/SDIO stack enumerates. `initVariant()` runs
early in Arduino startup, well before `HostedBluetooth::setup()`, so ordering is correct by
construction. The risk is only if the io_expander handle fails to initialize (I2C error) — then
the C6 is never released and SDIO enumeration fails.

**Mitigation:** Watch for `[variant] xl9535 expander init: err=0` followed by
`[variant] C6 (ESP-Hosted slave) released from reset via XL9535 bit 12` in the boot log BEFORE
any esp_hosted lines. If the expander init line shows a non-zero err, fix the I2C bus first.
The dummy driver reset GPIO (54) is unwired to the C6, so it cannot substitute for this path.

### R4: C6 reset polarity

**Risk:** The C6 EN pin is documented as active-HIGH (HIGH = run, LOW = reset). If the XL9535
power-up default is LOW and the launcher hasn't run, C6 stays in reset until `initVariant`
asserts it HIGH. This is correct behavior — Milestone A builds don't need C6 at all.

**Verification:** With the Milestone B build running, probe XL9535 bit 12 (physical pin on
the expander) after boot — it should read HIGH. If it reads LOW, the `esp_io_expander_set_level`
call is not executing (check io_expander handle is non-null in logs).

### R5: base `RESET_ON_EVERY_HOST_BOOTUP` vs. variant `RESET_ONLY_IF_NECESSARY`

**Risk:** The base config (`esp32p4.ini`) sets
`CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y`, which would make the driver toggle its
reset GPIO (now the dummy GPIO 54) on every boot. Our variant block sets
`CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` so the driver tries SDIO enumeration FIRST
and only toggles the dummy GPIO if enumeration fails. Because the C6 is already up (XL9535
release), enumeration succeeds and GPIO 54 is never driven.

**Mitigation:** Confirm in `.pio/build/t-display-p4/sdkconfig` that
`CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y` is present (variant entries are appended
after the base block, so the later value wins). Even if GPIO 54 does get toggled, it is unwired
to the C6, so the toggle is harmless.

**Note:** With `CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=200` (our override of the 1500ms base
default), the settle delay after any reset attempt is 200ms. Our real reset already settled in
`initVariant()`, so this delay is just margin.

### R6: dummy reset GPIO 54 must stay unused

**Risk:** GPIO 54 is assigned to both `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` and
`CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE` purely to satisfy the driver's
`assert(reset_pin.pin != -1)` (`sdio_drv.c`). It was verified unused across
`variant.h`, `variant.cpp`, and `pins_arduino.h` for the T-Display P4. If a future hardware
revision wires GPIO 54 to something, the driver toggling it (only in the enumeration-failure
path) could glitch that peripheral.

**Mitigation:** If GPIO 54 ever becomes used, pick another clearly-unused, output-capable P4
GPIO (ESP32-P4 has GPIO 0-54) for the two reset keys and re-verify it is unreferenced in the
three variant files. The choice is arbitrary as long as the pin is real, in `[0,100]`, and not
wired to the C6.

### R7: Dual-boot interaction — launcher may have already released C6

The DualMesh launcher may set XL9535 bit 12 HIGH before booting the Meshtastic OTA slot.
If so, the `initVariant` addition is a no-op (idempotent `set_level(HIGH)` on an already-HIGH
pin). This is safe; no extra delay penalty.

---

## 9. File References (all paths relative to meshtastic-firmware/)

| File | Relevance |
|------|-----------|
| `src/bluetooth/HostedBluetooth.cpp` | Full BLE implementation; compile guard line 3; `setSlaveResetLine` lines 365-390 (NO patch needed); `setup()` line 464; `deinit()` line 493 |
| esp-hosted-mcu `host/.../sdio_drv.c` (`ensure_slave_bus_ready`) | Source of `assert(reset_pin.pin != -1)` — the reason the reset GPIO must be a real pin (54), not unset/-1 |
| `src/bluetooth/HostedBluetooth.h` | Class declaration (43 lines, complete) |
| `src/platform/esp32/main-esp32.cpp` | P4-gated include lines 7-13; `new HostedBluetooth()` lines 58-62 |
| `variants/esp32p4/esp32p4.ini` | Base sdkconfig lines 44-88; component keep comment line 22 |
| `variants/esp32p4/t-display-p4/platformio.ini` | BLE exclude flag line 44; sdkconfig block lines 96-100 |
| `variants/esp32p4/t-display-p4/variant.cpp` | `initVariant` lines 30-108; io_expander handle line 12 |
| `variants/esp32p4/t-display-p4/variant.h` | Board pin defines |
| `variants/esp32p4/t-display-p4/pins_arduino.h` | SDIO pin defines lines 43-51; C6 reset macro line 51 |
| `variants/esp32p4/crowpanel-advanced-p4/platformio.ini` | Reference: Slot 1 SDIO sdkconfig lines 187-196; reset/polarity lines 195-196 |

---

*Draft prepared 2026-06-13. Apply only after C6 is confirmed running network_adapter slave.*
