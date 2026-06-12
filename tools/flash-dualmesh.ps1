# Flash the complete DualMesh layout to a T-Display P4 (default COM6).
# Layout: stock bootloader + dualmesh partition table + clean otadata + launcher
# + MeshOS in ota_0. NVS region (0x9000) is never written — config/license survive.
param([string]$Port = 'COM6')

$root = Split-Path $PSScriptRoot -Parent
esptool --chip esp32p4 -p $Port -b 921600 --before default-reset --after hard-reset write-flash `
    --flash-mode dio --flash-size 16MB --flash-freq 80m `
    0x2000   "$root\backups\flash-dumps\extracted\stock-bootloader.bin" `
    0x8000   "$root\launcher\build\partition_table\partition-table.bin" `
    0xF000   "$root\launcher\build\ota_data_initial.bin" `
    0x20000  "$root\launcher\build\dualmesh-launcher.bin" `
    0x100000 "$root\backups\flash-dumps\extracted\meshos-app-dbac5b1-trimmed.bin"
# IMPORTANT: do NOT use the launcher build's own bootloader.bin at 0x2000 —
# it lacks the PSRAM/MSPI bootloader config MeshOS needs and panics in early
# init (Load access fault after pmu_pvt). The stock dumped bootloader
# (IDF v5.5-dirty, Dec 2025) boots both MeshOS (idf 5.4.3) and the launcher.
