"""Parse an ESP-IDF partition table out of a full flash dump and report app images.

Usage: python parse_partitions.py <dump.bin> [--extract <outdir>]
"""
import hashlib
import struct
import sys
from pathlib import Path

PT_OFFSET = 0x8000
ENTRY_SIZE = 32
MAGIC_ENTRY = b"\xaa\x50"
MAGIC_MD5 = b"\xeb\xeb"
APP_IMAGE_MAGIC = 0xE9

TYPES = {0: "app", 1: "data"}
SUBTYPES_APP = {0x00: "factory", **{0x10 + i: f"ota_{i}" for i in range(16)}, 0x20: "test"}
SUBTYPES_DATA = {
    0x00: "otadata", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys",
    0x05: "efuse", 0x06: "undefined", 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs",
}


def subtype_name(ptype, sub):
    if ptype == 0:
        return SUBTYPES_APP.get(sub, hex(sub))
    return SUBTYPES_DATA.get(sub, hex(sub))


def main():
    dump_path = Path(sys.argv[1])
    extract_dir = None
    if "--extract" in sys.argv:
        extract_dir = Path(sys.argv[sys.argv.index("--extract") + 1])
        extract_dir.mkdir(parents=True, exist_ok=True)

    blob = dump_path.read_bytes()
    print(f"dump: {dump_path.name}  size={len(blob)} bytes")
    print(f"sha256: {hashlib.sha256(blob).hexdigest()}")
    print(f"\npartition table @ {PT_OFFSET:#x}:")
    print(f"{'label':<18}{'type':<6}{'subtype':<10}{'offset':<12}{'size':<12}{'end':<12}notes")

    entries = []
    for i in range(96):  # table is one 4KB sector max
        off = PT_OFFSET + i * ENTRY_SIZE
        entry = blob[off : off + ENTRY_SIZE]
        if entry[:2] == MAGIC_MD5:
            continue
        if entry[:2] != MAGIC_ENTRY:
            break
        ptype, sub = entry[2], entry[3]
        p_off, p_size = struct.unpack("<II", entry[4:12])
        label = entry[12:28].rstrip(b"\x00").decode(errors="replace")
        entries.append((label, ptype, sub, p_off, p_size))

    for label, ptype, sub, p_off, p_size in entries:
        notes = ""
        if ptype == 0:  # app partition: check image magic + describe
            magic = blob[p_off]
            if magic == APP_IMAGE_MAGIC:
                seg_count = blob[p_off + 1]
                notes = f"valid app image (0xE9, {seg_count} segments)"
                # ESP app descriptor sits at offset 0x20 into the image;
                # project name at +0x50 (32 bytes), version at +0x30 (32 bytes)
                desc = blob[p_off + 0x20 : p_off + 0x120]
                if len(desc) >= 0x70 and struct.unpack("<I", desc[:4])[0] == 0xABCD5432:
                    ver = desc[0x10:0x30].rstrip(b"\x00").decode(errors="replace")
                    proj = desc[0x30:0x50].rstrip(b"\x00").decode(errors="replace")
                    notes += f"  project='{proj}' version='{ver}'"
            elif magic == 0xFF:
                notes = "empty (0xFF)"
            else:
                notes = f"first byte {magic:#04x} (not an app image)"
            if extract_dir and magic == APP_IMAGE_MAGIC:
                out = extract_dir / f"{label}_{p_off:#x}.bin"
                out.write_bytes(blob[p_off : p_off + p_size])
                notes += f"  -> extracted {out.name}"
        print(
            f"{label:<18}{TYPES.get(ptype, hex(ptype)):<6}{subtype_name(ptype, sub):<10}"
            f"{p_off:#010x}  {p_size:#010x}  {p_off + p_size:#010x}  {notes}"
        )

    used = max((o + s for _, _, _, o, s in entries), default=0)
    print(f"\nhighest used offset: {used:#x} ({used / 1024 / 1024:.2f} MB of {len(blob) // (1024 * 1024)} MB)")


if __name__ == "__main__":
    main()
