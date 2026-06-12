# Draft: LilyGo GitHub issue

Post at: https://github.com/Xinyuan-LilyGO/T-Display-P4/issues -> New issue

**Title:** Official procedure to reflash the ESP32-C6 co-processor on a cased unit?

Hi — T-Display P4 V1.0 (cased retail unit). While experimenting with the C6
firmware via `AT+USEROTA`, the C6 ended up booting a non-working image from
ota_1 (the factory `esp32c6_at_slave` v4.1.0.0-dev image is still intact in
ota_0, but otadata selects ota_1 and the device no longer responds on SDIO, so
nothing can switch it back from the host side).

Questions:
1. What is the **official procedure to reflash the ESP32-C6** on an
   assembled/cased unit? The README's flash_download_tool section mentions
   selecting "ESP32-C6", but the C6's UART0 doesn't appear to reach either
   USB-C port.
2. Where are the **C6 UART test pads** (C6_U0TXD / C6_U0RXD / C6_IO9) on the
   V1.0 PCB, and how do you recommend opening the case without damaging the
   rubberized back?
3. Was the factory C6 AT bootloader built with
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`? (It didn't roll back to ota_0
   after the bad image, which surprised me.)
4. Is `[esp32c6][network_adapter_v2.12.7]` (the only blob not tagged `[sdio]`)
   intended for the V1.0 board, or for the upcoming v2.0?

Happy to provide logs. Thanks!
