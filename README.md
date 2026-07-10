# TinyEReader

A tiny Wi-Fi e-reader firmware for the [Elecrow CrowPanel ESP32 E-Paper HMI 2.13"](https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250) (model DIE01021S).

Upload a plain `.txt` file over Wi-Fi, read it on the e-paper screen, and it remembers where you left off.

## Features

- Wi-Fi access point (`PocketReader` / `12345678`) with a one-page upload form at `http://192.168.4.1`
- Stores the book in LittleFS, survives reboots
- Remembers your reading position and real page-turn history (Back goes to the actual previous page, not just backward by N bytes)
- Dial/menu/back buttons for navigation
- Light-sleeps the ESP32 and puts the e-paper panel to sleep after a minute of inactivity, wakes on any button press

## Hardware

| Board feature | GPIO |
| --- | --- |
| Menu button | 2 |
| Back / Exit button | 1 |
| Dial down (next) | 4 |
| Dial up (prev) | 6 |
| Dial press (OK) | 5 |
| Display power enable | 7 |
| E-paper SCK | 12 |
| E-paper MOSI | 11 |
| E-paper RST | 10 |
| E-paper DC | 13 |
| E-paper CS | 14 |
| E-paper BUSY | 9 |

The BOOT button (GPIO0) and REST/RESET button are hardware-level (bootloader / reset) and aren't read by the sketch.

## Display driver

The sketch does **not** use GxEPD2. This panel ships with either an SSD1680Z or JD79661 controller depending on manufacturing batch, and only Elecrow's own driver is confirmed to handle both correctly. `firmware/TinyEReader/EPD*.{h,cpp}` and `spi.{h,cpp}` are Elecrow's own driver files, pulled from their [reference repo](https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250) and bundled directly in the sketch folder (Arduino compiles any `.cpp`/`.h` sitting next to the `.ino`, so no separate library install is needed). No license file is published in Elecrow's repo; it's vendor example code distributed for use with their hardware.

GPIO7 must be driven high before the panel will draw anything — it's the screen's power-enable pin. Easy to miss; it's not called out in the product's onboarding docs, only in the example sketches.

## Building

Requires the Arduino IDE (for its bundled `arduino-cli`) — no manual library installs needed, since the only dependency (the EPD driver) lives in the sketch folder.

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build-firmware.ps1
```

This installs the ESP32 board core on first run, compiles `firmware/TinyEReader`, and copies the merged binary to `tools/web-flasher/firmware/tiny-ereader-merged.bin`.

(If double-clicking `.ps1` files or running them directly gives a "running scripts is disabled" error, that's Windows' default PowerShell execution policy — the `-ExecutionPolicy Bypass` flag above sidesteps it for just this one run, without changing your system policy.)

### Board settings (if building from the Arduino IDE instead)

- Board: **ESP32S3 Dev Module**
- Flash Size: **8MB**
- Partition Scheme: **8M with spiffs (3MB APP/1.5MB SPIFFS)**
- PSRAM: **OPI PSRAM**
- USB CDC On Boot: **Enabled**

## Flashing

**Option 1 — direct USB upload (fastest):**

```powershell
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
& $cli upload -p COMx --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB,CDCOnBoot=cdc" --input-dir firmware\TinyEReader\build firmware\TinyEReader
```

Replace `COMx` with the board's port (check Device Manager). If it doesn't connect, hold **BOOT**, tap **RESET**, then release **BOOT** and retry.

**Option 2 — browser flasher (no Arduino IDE needed on the flashing machine):**

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start-web-flasher.ps1
```

Then open `http://localhost:8787` in Chrome or Edge (Web Serial requires a Chromium browser), click **Connect and Flash**, and pick the board's serial port.

## Using it

1. Flash the firmware.
2. On your phone/laptop, connect to Wi-Fi network `PocketReader`, password `12345678`.
3. Open `http://192.168.4.1` and upload a `.txt` file.
4. Use the dial to turn pages, Back to go to the previous page, Menu to see the Wi-Fi info screen.
