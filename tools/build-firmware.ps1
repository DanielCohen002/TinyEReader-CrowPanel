# Rebuilds firmware/TinyEReader with arduino-cli and drops the merged binary
# everywhere a web flasher expects to find it. Uses the arduino-cli bundled
# with the Arduino IDE, so no separate install is required.
#
# There are TWO independent copies of the merged binary in this repo, and
# both need refreshing after every firmware change or whichever one didn't
# get updated silently keeps serving old firmware:
#   - tools/web-flasher/firmware/tiny-ereader-merged.bin -- local-only, for
#     testing via tools/start-web-flasher.ps1 (localhost:8787)
#   - docs/firmware.bin -- what GitHub Pages actually serves at
#     https://danielcohen002.github.io/TinyEReader-CrowPanel/, referenced by
#     docs/manifest.json. This is the one real users flash from.
#
# Run from anywhere with:
#   powershell -ExecutionPolicy Bypass -File .\tools\build-firmware.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $repoRoot "firmware\TinyEReader"
$build = Join-Path $repoRoot "firmware\TinyEReader\build"
$flasherFirmware = Join-Path $repoRoot "tools\web-flasher\firmware"
$docsFirmware = Join-Path $repoRoot "docs\firmware.bin"
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
Copy-Item -Force $merged.FullName $docsFirmware
Write-Host ""
Write-Host "Firmware ready (both copies refreshed):"
Write-Host (Join-Path $flasherFirmware "tiny-ereader-merged.bin")
Write-Host $docsFirmware
Write-Host ""
Write-Host "To flash directly over USB instead of the browser flasher, run:"
Write-Host "  & `"$arduinoCli`" upload -p COMx --fqbn `"$fqbn`" --input-dir `"$build`" `"$sketch`""
