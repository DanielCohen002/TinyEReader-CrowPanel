# Rebuilds firmware/TinyEReader with arduino-cli and drops the merged binary
# where the web flasher expects it. Uses the arduino-cli bundled with the
# Arduino IDE, so no separate install is required.
#
# Run from anywhere with:
#   powershell -ExecutionPolicy Bypass -File .\tools\build-firmware.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $repoRoot "firmware\TinyEReader"
$build = Join-Path $repoRoot "firmware\TinyEReader\build"
$flasherFirmware = Join-Path $repoRoot "tools\web-flasher\firmware"
$fqbn = "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc"

$arduinoCli = Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (!(Test-Path $arduinoCli)) {
  throw "Could not find arduino-cli.exe inside the Arduino IDE install. Install Arduino IDE first."
}

New-Item -ItemType Directory -Force -Path $build | Out-Null
New-Item -ItemType Directory -Force -Path $flasherFirmware | Out-Null

& $arduinoCli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
& $arduinoCli core update-index
& $arduinoCli core install esp32:esp32

& $arduinoCli compile --fqbn $fqbn --export-binaries --output-dir $build $sketch

$merged = Get-ChildItem -Path $build -Filter "*.merged.bin" -Recurse | Select-Object -First 1
if (!$merged) {
  throw "Compile finished, but no .merged.bin firmware file was found."
}

Copy-Item -Force $merged.FullName (Join-Path $flasherFirmware "tiny-ereader-merged.bin")
Write-Host ""
Write-Host "Firmware ready:"
Write-Host (Join-Path $flasherFirmware "tiny-ereader-merged.bin")
Write-Host ""
Write-Host "To flash directly over USB instead of the browser flasher, run:"
Write-Host "  & `"$arduinoCli`" upload -p COMx --fqbn `"$fqbn`" --input-dir `"$build`" `"$sketch`""
