# Draft: community announcement post

Target venues (post after the Meshtastic port lands, or now with "Meshtastic coming"
framing): r/meshcore, r/esp32, Xinyuan-LilyGO/T-Display-P4 GitHub Discussions,
MeshCore Discord #devices. Adjust the intro line per venue.

---

**Title: Dual-boot launcher for the LilyGo T-Display P4 — switch between MeshCore
(MeshOS) and other firmware from a touch menu, no PC needed**

I got tired of choosing one firmware for my T-Display P4, so I built a boot
launcher for it — think M5Launcher, but for this board.

**What it does:**

- On every boot you get a 3-second splash — ignore it and the device continues
  into whatever firmware you used last; tap it and you get a menu
- Two firmware bays (OTA slots) with one-tap switching. MeshOS (the licensed
  MeshCore firmware) lives in one permanently — **its license and all settings
  survive every switch**
- Install new firmware straight from the SD card (`/firmware/*.bin`) with a
  progress bar — no computer, no web flasher
- Works on both the TFT and AMOLED versions (runtime detection)

**How:** a small LVGL app in the factory partition + a custom partition table +
ESP-IDF's app-rollback feature used as a "boot once" mechanism — every reset
falls back to the launcher, which auto-continues unless you tap. MeshOS runs
relocated in an OTA slot with its NVS untouched, so activation is preserved.

**Repo (MIT, no firmware binaries included):**
https://github.com/Mikhail-Za/Boot-Launcher-for-T-Display-P4-Liliygo-

The README documents every trap we hit, including some that will bite anyone
building for this board: LilyGo ships **rev v1.x ESP32-P4 silicon** while IDF
defaults target v3.1+ (mutually exclusive register layouts — your build won't
even flash without two sdkconfig lines), and a bootloader built without the
board's PSRAM config will boot its own app fine but crash MeshOS in early init.

**Roadmap:** Meshtastic port for this board (in progress — it'll be a .bin you
drop on the SD card), then an open-source MeshCore companion port. If you're
working on either, or you have the AMOLED SKU and want to test, issues/PRs
welcome.

Not affiliated with LilyGo, Meshtastic, or either MeshCore project. MeshOS is
Andy Kirby's licensed firmware — this launcher doesn't include, modify, or
redistribute it; it just gives it a roommate.

---

Notes for Zaid before posting:
- Add 2-3 photos: launcher menu on device, the splash countdown, MeshOS running
- If posting to r/meshcore, lead with the license-survives-switching point
- If posting to LilyGo GitHub Discussions, lead with the chip-revision trap
  (it answers a question several people will eventually ask)
- Consider waiting until the Meshtastic .bin exists — "switch between MeshCore
  and Meshtastic today" is a much stronger post than "Meshtastic coming soon"
