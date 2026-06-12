# Extract the app-only image from LilyGo's combined C6 network_adapter blob
# (bootloader@0x0, ptable@0x8000, app in ota_0 @0x10000, size 0x1E0000) into
# c6-updater/main/slave_fw.bin for EMBED_FILES. Trailing 0xFF padding is
# trimmed at runtime by the updater.
$root = Split-Path $PSScriptRoot -Parent
$blob = Get-ChildItem "$root\T-Display-P4\firmware" -Recurse -Filter '*network_adapter_v2.12.7*' | Select-Object -First 1
if (-not $blob) { throw "v2.12.7 network_adapter blob not found under T-Display-P4\firmware" }
$b = [System.IO.File]::ReadAllBytes($blob.FullName)
$app = $b[0x10000..(0x1F0000 - 1)]
$out = "$root\c6-updater\main\slave_fw.bin"
[System.IO.File]::WriteAllBytes($out, $app)
Write-Output "wrote $out ($($app.Length) bytes)"
esptool --chip esp32c6 image-info $out 2>&1 | Select-String -Pattern 'Image size|Project|App version|Validation'
