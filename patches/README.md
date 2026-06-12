# Patches against external repos

## lilygo-debug2-build-fixes.patch

Applies to `Xinyuan-LilyGO/T-Display-P4` branch `debug2` (tested at HEAD `bbd0e1a`).
Fixes the examples not compiling as shipped:

- `main/Kconfig.projbuild`: restores the screen-type choice (`SCREEN_TYPE_HI8561`
  TFT default / `SCREEN_TYPE_RM69A10` AMOLED) that was dropped mid-refactor.
- `main/CMakeLists.txt`: derives `SCREEN_WIDTH/SCREEN_HEIGHT/SCREEN_BITS_PER_PIXEL`
  compile definitions from the Kconfig choice.
- `sdkconfig.defaults`: `CONFIG_LV_FONT_MONTSERRAT_16=y` (LVGL demo font),
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y`
  (LilyGo boards ship rev v1.x silicon; IDF defaults target v3.1+ and refuse to flash).

Re-apply after a fresh clone:

    git clone --branch debug2 https://github.com/Xinyuan-LilyGO/T-Display-P4.git
    cd T-Display-P4
    git submodule update --init libraries/...   # all libraries/ submodules, skip apps/esp-at
    git apply ../patches/lilygo-debug2-build-fixes.patch
