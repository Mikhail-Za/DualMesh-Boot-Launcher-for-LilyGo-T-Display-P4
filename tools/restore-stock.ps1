# Restore Unit A to the exact stock MeshOS state captured 2026-06-12.
# Writes the full 16MB dump back — bootloader, stock partition table, MeshOS
# in factory slot, NVS with license cache and user config. Takes ~10 min.
param([string]$Port = 'COM6')

$root = Split-Path $PSScriptRoot -Parent
esptool --chip esp32p4 -p $Port -b 921600 --before default-reset --after hard-reset write-flash `
    0x0 "$root\backups\flash-dumps\unitA-meshos-stock-2026-06-12.bin"
