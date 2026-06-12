# Draft: MeshCore Discord post

Post in: MeshCore Discord (invite via meshcore.gg) -> T-Display P4 /
firmware-help channel

Hey all — question about MeshOS on the LilyGo T-Display P4. Am I right that
MeshOS talks to the ESP32-C6 using the **factory ESP-AT firmware** (AT over
SDIO), and the web flasher never reflashes the C6? Asking because I
experimented with putting esp-hosted `network_adapter` on the C6 (for a
Meshtastic dual-boot project), the image didn't come up, and now with the C6
unresponsive **MeshOS hard-crashes in a boot loop** (Guru Meditation, stack
protection fault, right after "MeshCore Integration Active").

Two asks:
1. Has anyone successfully reflashed/recovered the C6 on this board, and how
   did you physically get to it?
2. @Kirby — might be worth a defensive fix so MeshOS degrades gracefully
   instead of crash-looping when the C6 doesn't respond. Happy to share
   serial logs.
