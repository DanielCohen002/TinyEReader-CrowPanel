# TinyEReader

A tiny Wi-Fi e-reader firmware for the [Elecrow CrowPanel ESP32 E-Paper HMI 2.13"](https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250) (model DIE01021S).

Upload `.txt` books over Wi-Fi, read them on the e-paper screen, jump between chapters, and it remembers your place in each book.

## Features

- Multi-book library: each upload adds a book rather than replacing one, up to 3MB per book
- Remembers your reading position per book, boots straight back into whichever book you read last
- True sequential page-up/page-down (always the actual neighboring page, computed by replaying pagination rather than an undo-last-jump stack) — see [Display driver](#display-driver)
- Chapter skip, using a marker character a book can contain — see [Chapters](#chapters-and-the-epub-converter) below
- Home menu: Resume Last Book / Choose Book / Connect to Wi-Fi, each with its own icon, each screen showing free space left in the library
- Delete a book from Choose Book via a Yes/No confirmation dialog
- Two QR codes on the Connect to Wi-Fi screen — one auto-joins the `PocketReader` network, one links out to the project — generated from PNGs with `tools/image_to_epd.py`
- Wi-Fi only powers on while the Connect to Wi-Fi screen is open — off the rest of the time (including at boot) to save battery
- Light-sleeps the ESP32 and puts the e-paper panel to sleep after a minute of inactivity, wakes on any button press

## Hardware

| Board feature | GPIO |
| --- | --- |
| Menu button (top) | 2 |
| Back / Exit button (bottom) | 1 |
| Dial rotate down | 4 |
| Dial rotate up | 6 |
| Dial press | 5 |
| Display power enable | 7 |
| E-paper SCK | 12 |
| E-paper MOSI | 11 |
| E-paper RST | 10 |
| E-paper DC | 13 |
| E-paper CS | 14 |
| E-paper BUSY | 9 |

The BOOT button (GPIO0) and REST/RESET button are hardware-level (bootloader / reset) and aren't read by the sketch.

Button meaning depends on which screen is showing:

| | Top | Bottom | Dial rotate | Dial press |
| --- | --- | --- | --- | --- |
| Reading a book | Previous page (hold to repeat) | Next page (hold to repeat) | Up = previous chapter, down = next chapter | Open Home menu |
| Home / Choose Book / delete dialog | Jump to Home (cancels the delete dialog without deleting) | On a book in Choose Book, opens the delete dialog | Move selection up/down | Select highlighted item |
| Connect to Wi-Fi | Back to Home | — | — | Back to Home |

## Display driver

The sketch does **not** use GxEPD2. This panel ships with either an SSD1680Z or JD79661 controller depending on manufacturing batch, and only Elecrow's own driver is confirmed to handle both correctly. `firmware/TinyEReader/EPD*.{h,cpp}` and `spi.{h,cpp}` are Elecrow's own driver files, pulled from their [reference repo](https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250) and bundled directly in the sketch folder (Arduino compiles any `.cpp`/`.h` sitting next to the `.ino`, so no separate library install is needed). No license file is published in Elecrow's repo; it's vendor example code distributed for use with their hardware.

GPIO7 must be driven high before the panel will draw anything — it's the screen's power-enable pin. Easy to miss; it's not called out in the product's onboarding docs, only in the example sketches.

Page turns run a full white-fill-and-clear cycle only once at boot (or after waking from sleep); every other page draw is a partial update kept ghost-free by re-syncing the panel's "previous frame" register (`EPD_SyncOldData`) after each draw, instead of re-flashing the whole screen on every turn.

## Storage

The board has 8MB of flash, but Arduino's stock "8M with spiffs" partition scheme only gives LittleFS 1.5MB of it (reserving 3MB for the app, when this firmware compiles to about 1MB). `firmware/TinyEReader/partitions.csv` is a custom table that gives the app 1.5MB (still comfortable headroom) and LittleFS the remaining ~6.3MB. Building/flashing with `PartitionScheme=custom` in the FQBN picks this file up automatically — that's what `tools/build-firmware.ps1` and the commands below use.

Changing the partition table changes where LittleFS's data physically lives on the flash chip, so **flashing this firmware onto a board that was previously running a different partition scheme will wipe any books already uploaded** — LittleFS won't recognize the old data at the new offsets and reformats automatically. That only happens once, the first time you flash the new table.

## Chapters and the EPUB converter

Plain `.txt` has no concept of chapters, so the firmware looks for a form-feed byte (`0x0C`, `'\f'`) in the book to mark chapter boundaries — invisible in the rendered text, just a jump point for the dial while reading. A hand-typed `.txt` with no form feeds just has one implicit chapter.

`tools/epub_to_txt.py` converts an EPUB to a `.txt` with these markers already inserted, using the book's real chapter headings (`<h1>`/`<h2>`/`<h3>`) when present, or one marker per internal chapter file as a fallback:

```powershell
py tools\epub_to_txt.py yourbook.epub
```

Writes `yourbook.txt` next to it — upload that the normal way. Real-world EPUBs vary a lot in how publishers structure their markup, so double-check the first book you convert; if the chapters land in odd places, note which book and it's worth revisiting the detection heuristic.

## Icons and QR codes

`EPD_ShowPicture()` (the vendor driver's bitmap function) takes a 1-bit image packed MSB-first, row-major, with each row padded to a whole byte — not a format any normal image tool exports. `tools/image_to_epd.py` converts a PNG (or the base64 PNG embedded in a `.piskel` file, so hand-drawn icons from [Piskel](https://www.piskelapp.com/) can be used directly with no separate export step) into that format as a ready-to-paste C array:

```powershell
py tools\image_to_epd.py icon.piskel --name iconName --out firmware\TinyEReader\generated\icon_name.h
py tools\image_to_epd.py qr.png --name qrName --size 80 --out firmware\TinyEReader\generated\qr_name.h
```

`--size` resizes to an NxN square using PIL's BOX filter (averages each target pixel over its source region — held up in testing better than nearest-neighbor for QR codes, where a single bad sample can flip a module and break the scan). For a new QR code, verify it still decodes at your target size *from the actual packed bytes*, not just the source image, before wiring it in — a corrupted pack step or a size that's too small can silently produce an unscannable code that still "looks right" in a raw preview.

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
- Partition Scheme: **Custom** (picks up `firmware/TinyEReader/partitions.csv`)
- PSRAM: **OPI PSRAM**
- USB CDC On Boot: **Enabled**

## Flashing

**Option 1 — direct USB upload (fastest):**

```powershell
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
& $cli upload -p COMx --fqbn "esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc" --input-dir firmware\TinyEReader\build firmware\TinyEReader
```

Replace `COMx` with the board's port (check Device Manager). If it doesn't connect, hold **BOOT**, tap **RESET**, then release **BOOT** and retry.

**Option 2 — browser flasher (no Arduino IDE needed on the flashing machine):**

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start-web-flasher.ps1
```

Then open `http://localhost:8787` in Chrome or Edge (Web Serial requires a Chromium browser), click **Connect and Flash**, and pick the board's serial port.

## Using it

1. Flash the firmware.
2. On the device, press the dial to open the Home menu, move to **Connect to Wi-Fi** with the dial, and press the dial to select it.
3. Scan the Wi-Fi QR code with your phone (or connect manually to `PocketReader`, password `12345678`).
4. Open `http://192.168.4.1` and upload a `.txt` file (or a converted EPUB — see [Chapters](#chapters-and-the-epub-converter)). It becomes the active book immediately.
5. Press the dial (or tap Top) to back out of the Wi-Fi screen when done — Wi-Fi turns off automatically.
6. Use Top/Bottom to turn pages (hold to keep going), the dial to skip chapters, dial press for the Home menu.
