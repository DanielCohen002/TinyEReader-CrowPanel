# Serves tools/web-flasher/ on http://localhost:8787 so the browser-based
# ESP Web Tools flasher can find firmware/tiny-ereader-merged.bin.
# Requires Python. Use Chrome or Edge to open the page (Web Serial only
# works in Chromium browsers).
#
# Run from anywhere with:
#   powershell -ExecutionPolicy Bypass -File .\tools\start-web-flasher.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$flasher = Join-Path $repoRoot "tools\web-flasher"

Set-Location $flasher
Write-Host "Starting flasher at http://localhost:8787"
Write-Host "Press Ctrl+C to stop it."
python -m http.server 8787
