# Session handoff — MeshCore IDF firmware: TX + boot fixed, RX isolated to firmware (2026-06-20)

Status of the open-source MeshCore-on-Trail-Mate effort (firmware lives in the
`trail-mate/` clone, an AGPL-3.0 fork — gitignored here; this repo holds the
launcher + notes/patches only).

## TL;DR

- **Basic mesh works on MeshOS again.** The "two units can't talk anymore" scare
  was a **dislodged microSD card** on Unit B, not firmware. Re-seating it restored
  MeshOS TX+RX between both units.
- **Our firmware: boot + transmit are solid and committed.** Both units boot
  stable and complete real on-air transmits (genuine SX1262 TxDone).
- **Our firmware: receive does NOT work, and it is now isolated to the firmware.**
  With MeshOS having just proven both radios/antennas/environment are healthy
  between these exact units, our firmware boots + TX on both but receives zero
  packets on either. So the RX defect is in our firmware/driver, not hardware.

## The SD-card lesson (why "basic mesh broke")

A long arc was spent treating a mesh regression as an RX firmware bug. It was a
loose SD card on Unit B (it had lost its saved contacts/config). Tells that were
under-weighted: the shipping commercial firmware (MeshOS) failed identically (a
shipping firmware does not self-regress); both units transmitted fine and only RX
failed; Unit A still showed its SD contacts while Unit B did not. Red herrings
checked and ruled out: units stacked too close (proximity overload — separating
did not fix it) and Flipper sub-GHz jamming (it was off).

**Rule for next time:** when a previously-working RF/mesh setup regresses, triage
the physical layer first — re-seat SD/storage, check the antenna is tight, confirm
the radio channel/region config did not revert, rule out proximity and a nearby
emitter — before grinding firmware.

## Hardware quirk found this session (non-obvious, reusable)

**The SX1262 on the T-Display P4 goes dark in sustained continuous RX.** After a
few seconds parked in continuous RX it stops responding on SPI (version register
and status/IRQ reads return all-ones; packet-length reads return 255). An NRESET
pulse via the XL9535 does NOT revive it; only a full SPI teardown + re-init
(`spi_bus_remove_device` + `spi_bus_free` + `init_locked`) does. This broke both
TX (SetTx into a dead chip, no TxDone) and RX (garbage reads). The TX path now
detects and revives it.

## Firmware changes this session (in the `trail-mate` clone, branch `rx-debug`)

Commit SHAs (in the trail-mate fork; recover exact code there):

- **TX completion fix** — `startTransmit` detects a dark chip and revives it
  (reset + re-apply cached LoRa cfg), issues SetStandby before TX setup, and blocks
  until the real TxDone IRQ latches (returns it un-cleared so the adapter reads the
  genuine flag). Measured TxDone latency at SF7/BW62.5 is ~719 ms.
- **`2b0fb90`** — RX-poll guard against the dead-chip garbage (skip all-ones IRQ;
  drop max-length / floor-RSSI reads) so it can't flood the parser and fault.
- **`b274f1b`** — boot-stability: the inline dead-chip revive overflowed the LVGL
  app-loop task stack (~11 s in, at the first advert). Fix: app-loop task stack
  4096 → 16384 **for `tdisplayp4_tft` only**, and move the 255-byte RX buffer + the
  SX126x 260-byte SPI scratch buffers off that task stack. Goalkeeper-converged.
- **`fdf0cb1`** — a verified-advert serial marker (`idf-mc: rx advert verified
  from=<peer>`) that fires only after a MeshCore advert fully parses AND its Ed25519
  signature verifies AND the source node id != self. This is the genuine-reception
  signal the RX test counts (so radio noise can't be mistaken for reception).
- **`4b0930b`** — SX126x §15.4 standard-IQ register workaround: set bit 2 of
  REG_IQ (`0x0736`) on every SetPacketParams, matching RadioLib's `fixInvertedIQ`
  (we were missing it).

Files: `platform/esp/idf_common/src/sx126x_radio.cpp`,
`platform/esp/arduino_common/src/chat/infra/meshcore/meshcore_adapter.cpp`,
`apps/esp32_lvgl/src/esp32_lvgl_startup_runtime.cpp`.

## RX investigation — what is proven

A parallel diff of our IDF SX1262 RX setup against the two proven-on-this-board
references (RadioLib in `meshtastic-firmware/.pio/libdeps`, and the Meshtastic
`t-display-p4` variant) shows our RX now **matches them on every config axis**:

- Modulation: SF7, BW 62.5 kHz, CR 4/5, **LDRO auto-off** (correct at this SF/BW),
  programmed **before** frequency/CalibrateImage.
- Packet: EXPLICIT header, CRC on, **standard IQ** + the `0x0736` bit-2 workaround,
  preamble 16 (TX was 8 — fixed to match RX), sync `0x12` → register `0x1424`.
- Sensitivity reg `0x0889` bit 2 set (RadioLib fixSensitivity); RX boosted gain
  `0x08AC = 0x96`; image calibration forced after a chip reset.
- RF switch matches the reference: **DIO2-as-RF-switch OFF** (DIO2 not broken out),
  SKY13453 driven statically via **XL9535 IO1 held HIGH** for both TX and RX (LOW
  for RX disconnects the receiver), no RXEN/TXEN.
- Analog rails enabled: **VCCA = XL9535 IO10 (active-low)**, 3V3 = IO0 (active-low),
  5V = IO6. TCXO on DIO3 @ 1.6 V.

So there is no remaining static-config divergence to chase. Earlier telemetry also
showed the chip sitting stably in RX mode 0x5 (alive, real RF, RSSI bumps when the
peer transmits) yet the demodulator never correlates the preamble (GetStats rx /
crc_err / hdr_err stay 0).

## RX isolation result (the decisive datapoint)

With MeshOS having just proven both radios + antennas + the RF environment work
bidirectionally between these exact two units, our firmware was flashed fresh to
both (Unit A node F88CE3E2, Unit B node 1E2D1B14):

- both boot, both complete 3/3 transmits, no panic — **and zero received adverts on
  either unit.**

Conclusion: the receive failure is **definitively in our firmware**, not hardware,
antennas, environment, SD, or interference. The cause is therefore NOT a static
register value (those match the references) — it is most likely an init-sequence /
timing issue or a logic error in how the RX is actually armed or polled, despite
the chip reporting RX mode.

## Suggested next steps (when revisited)

1. Cross-test directions: our firmware (RX) against a known-good transmitter, and a
   known-good receiver against our firmware (TX), to confirm our framing is decodable
   and our RX path is the broken half.
2. Live register/state dump of our radio while in "RX mode 0x5" vs RadioLib's, at the
   moment a packet is on the air, to find why the demod never correlates.
3. Re-examine the keep-alive / re-arm interaction: confirm the radio is genuinely
   parked listening for the full packet airtime (~720 ms) and not being disturbed by
   the periodic dead-chip probe / re-arm during reception.

## Repo / firmware home

The firmware is an AGPL-3.0 fork of `vicliu624/trail-mate` and should live in its own
GitHub fork (pending `gh auth login`), not inside this GPL launcher repo. This note
and any patches are the launcher-repo mirror of that work, matching the existing
`notes/` + `patches/` pattern.
