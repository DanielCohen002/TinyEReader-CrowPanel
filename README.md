# TinyEReader

A tiny Wi-Fi e-reader firmware for the [Elecrow CrowPanel ESP32 E-Paper HMI 2.13"](https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250) (model DIE01021S).

Upload `.txt` books over Wi-Fi, read them on the e-paper screen, jump between chapters, and it remembers your place in each book.

**[Flash it from your browser](https://danielcohen002.github.io/TinyEReader-CrowPanel/)** — no Arduino IDE needed, just Chrome or Edge and a USB cable.

## Features

- Multi-book library: each upload adds a book rather than replacing one, up to 3MB per book
- Remembers your reading position per book, boots straight back into whichever book you read last
- Up to 3 bookmarks per book, separate from that automatic "current place" — hold Top while reading to save/overwrite one, browse and jump to them from the Bookmarks screen, which always shows both percent and page fraction for each slot regardless of the reading screen's own progress-indicator setting (see [Bookmarks](#bookmarks) below)
- True sequential page-up/page-down (always the actual neighboring page, computed by replaying pagination rather than an undo-last-jump stack) — see [Display driver](#display-driver)
- Chapter skip, using a marker character a book can contain — see [Chapters](#chapters-and-the-epub-converter) below. Hold the dial up/down instead of tapping repeatedly to keep skipping
- Drop an `.epub` straight onto the upload page and it's converted to `.txt` automatically, right in the browser
- Reading progress indicator, one of four: percentage, page fraction, a thin bottom bar, or off. Percent/Fraction are tacked onto the end of the last line of text rather than their own row — the line trims back a whole word at a time to make genuine room when needed, rather than drawing over whatever was already there — so none of the four costs a line of reading space
- Optional auto page turn (off by default) — reading becomes hands-free at a configurable interval
- Home menu: 5 icons (Resume Last Book / Choose Book / Bookmarks / Connect to Wi-Fi / Settings) shown 3 at a time across two pages, selection shown as a border box. Free space left in the library shows on Choose Book only (Home and Choose Book could report a hair apart due to LittleFS's own block-level accounting, so it's shown in one place, the more conservative of the two, instead of two possibly-inconsistent ones)
- Settings screen: auto-sleep timeout, deep-sleep timeout, auto page turn interval, invert display, book sort order, the progress indicator's format, text size, a controls reference, and factory reset — see [Settings](#settings) below
- Controls reference (in Settings): a scrollable list of what every button/dial does, grouped by what they do while reading vs. everywhere else
- Choose Book and the Bookmarks screen's book picker can sort A-Z, Z-A, or by file size
- Delete a book from Choose Book, or a bookmark from the Bookmarks screen, via the same Yes/No confirmation dialog
- Two QR codes on the Connect to Wi-Fi screen — one auto-joins the `PocketReader` network, one links out to the project — generated from PNGs with `tools/image_to_epd.py`
- Wi-Fi only powers on while the Connect to Wi-Fi screen is open — off the rest of the time (including at boot) to save battery
- Light-sleeps the ESP32 and puts the e-paper panel to sleep after inactivity (adjustable in Settings), wakes on any button press. Optionally escalates to a deeper, lower-power sleep tier after a further period of inactivity — see [Settings](#settings)

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
| Reading a book | Tap = previous page, hold = save/overwrite a bookmark (see [Bookmarks](#bookmarks)) | Next page | Up = previous chapter, down = next chapter — hold either to keep skipping instead of tapping repeatedly | Open Home menu |
| Home / Choose Book / Bookmarks / Settings / Controls / confirm dialog | Jump to Home (cancels the confirm dialog without acting) | On a book in Choose Book, or a bookmark in the Bookmarks screen, opens the delete confirmation | Move selection up/down (scrolls the Controls screen instead, which has nothing to select) | Select highlighted item (also just returns Home from Controls) |
| Connect to Wi-Fi | Back to Home | — | — | Back to Home |

This same table, plus which screens count as "reading" vs. "everywhere else," is also on the device itself — **Settings -> Controls**.

Home no longer fits all 5 items in one row at the icon size used, so it's paged: dial past the 3rd item (Bookmarks) to reach the other 2 (Connect to Wi-Fi / Settings), same dial up/down as everywhere else.

## Display driver

The sketch does **not** use GxEPD2. This panel ships with either an SSD1680Z or JD79661 controller depending on manufacturing batch, and only Elecrow's own driver is confirmed to handle both correctly. `firmware/TinyEReader/EPD*.{h,cpp}` and `spi.{h,cpp}` are Elecrow's own driver files, pulled from their [reference repo](https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250) and bundled directly in the sketch folder (Arduino compiles any `.cpp`/`.h` sitting next to the `.ino`, so no separate library install is needed). No license file is published in Elecrow's repo; it's vendor example code distributed for use with their hardware.

GPIO7 must be driven high before the panel will draw anything — it's the screen's power-enable pin. Easy to miss; it's not called out in the product's onboarding docs, only in the example sketches.

Page turns run a full white-fill-and-clear cycle only once at boot (or after waking from sleep); every other page draw is a partial update kept ghost-free by re-syncing the panel's "previous frame" register (`EPD_SyncOldData`) after each draw, instead of re-flashing the whole screen on every turn.

## Storage

The board has 8MB of flash, but Arduino's stock "8M with spiffs" partition scheme only gives LittleFS 1.5MB of it (reserving 3MB for the app, when this firmware compiles to about 1MB). `firmware/TinyEReader/partitions.csv` is a custom table that gives the app 1.5MB (still comfortable headroom) and LittleFS the remaining ~6.3MB. Building/flashing with `PartitionScheme=custom` in the FQBN picks this file up automatically — that's what `tools/build-firmware.ps1` and the commands below use.

Changing the partition table changes where LittleFS's data physically lives on the flash chip, so **flashing this firmware onto a board that was previously running a different partition scheme will wipe any books already uploaded** — LittleFS won't recognize the old data at the new offsets and reformats automatically. That only happens once, the first time you flash the new table.

## Chapters and the EPUB converter

Plain `.txt` has no concept of chapters, so the firmware looks for a form-feed byte (`0x0C`, `'\f'`) in the book to mark chapter boundaries — invisible in the rendered text, just a jump point for the dial while reading. A hand-typed `.txt` with no form feeds just has one implicit chapter.

**Just drop an `.epub` straight onto the upload page** (`http://192.168.4.1`) and it converts automatically, right in the browser, before uploading — no separate tool, no Python. It uses the book's real chapter headings (`<h1>`/`<h2>`/`<h3>`) when present, or one marker per internal chapter file as a fallback, then uploads the resulting `.txt`. This needs a browser with `DecompressionStream` support (recent Chrome, Edge, Firefox, or Safari); the conversion code is `firmware/TinyEReader/TinyEReader.ino`'s `epubToTxtScript`, served at `/epub-to-txt.js`.

There's also a standalone version of the same converter at **[the flash page](https://danielcohen002.github.io/TinyEReader-CrowPanel/epub-to-txt.html)** if you'd rather convert a batch of books ahead of time and keep the `.txt` files around, or two command-line options if you prefer scripting it: `tools/epub_to_txt.py` (Python, no dependencies) --

```powershell
py tools\epub_to_txt.py yourbook.epub
```

-- writes `yourbook.txt` next to it; upload that the normal way. Real-world EPUBs vary a lot in how publishers structure their markup, so double-check the first book you convert with any of these; if the chapters land in odd places, note which book and it's worth revisiting the detection heuristic (shared between the browser and Python versions, since they use the same logic).

## Bookmarks

Separate from the automatic "current place" (which always tracks wherever you last turned a page), each book gets up to 3 explicit bookmarks. Hold **Top** while reading to save one at the current page -- it fills the first empty slot, or overwrites slot 1 once all three are used, and flashes "Bookmark N saved" briefly so you know which slot changed. Tapping Top normally (not holding) still turns to the previous page as usual; the two are told apart by how long Top stays down (`BOOKMARK_HOLD_MS`, 600ms).

To use one, open the **Bookmarks** icon from Home, pick a book, then pick a slot -- each shows how far into the book it is as both a percentage and a page fraction together (e.g. `128/512 (25%)`), regardless of the reading screen's own Settings -- Progress choice (see [Reading progress](#reading-progress) below) -- or "(empty)" if unused. Computing both is a real pagination sweep through the book, so opening this screen can take a moment on a large book (a "Loading..." message covers it). Opening a bookmark does **not** touch "current place"; if you keep reading forward from there, current place starts tracking again normally. Bottom on a highlighted slot deletes it, with the same Yes/No confirmation as deleting a book.

## Settings

Open the **Settings** icon from Home (it's on the second Home page -- see [Buttons](#hardware)). Dial up/down moves between options, dial press acts on the highlighted one. Everything here persists with the ESP32's own Preferences (NVS) library, not LittleFS, since it's device configuration rather than book data.

- **Sleep timeout** -- cycles through 30 sec / 1 min / 2 min / 5 min / Never. Light sleep: near-instant to wake, on any button press.
- **Deep sleep timeout** -- cycles through Off / 5 min / 10 min / 20 min / 30 min, off by default. If the device is *still* untouched this much longer after light sleep has already kicked in, it drops into a deeper sleep tier with much lower current draw -- and shows a "TinyEReader / Deep sleep" splash screen right before it does, so a glance at the panel tells you it's actually in the deep tier rather than just light-sleeping with the last page still on screen (the panel holds that splash with zero power the whole time it's down, same as it holds a book page). Waking from it takes a few seconds (it's a real reboot) instead of being instant, but it comes back up in the same book at the same page either way -- it just can't restore whatever other screen (Settings, Choose Book, etc.) happened to be open when it went down, since that only lives in RAM. If that's ever more annoying than useful, set it back to Off, or just raise the plain Sleep timeout instead so light sleep alone covers you.
- **Auto-turn** -- cycles through Off / 15 sec / 30 sec / 1 min / 2 min. When enabled, the current page auto-advances once that long has passed since the last page change of any kind (a real page turn, a chapter jump, opening a book or bookmark) -- so it doesn't fire right after you've already moved. Stops naturally at the end of the book (`nextPage()` is already a no-op there).
- **Invert** -- On/Off, black-on-white vs. white-on-black. Forces a full white-fill-and-clear refresh on the next redraw so the polarity flip doesn't ghost.
- **Sort** -- cycles A-Z / Z-A / Size (largest first). Applies everywhere the library is listed: Choose Book, the Bookmarks screen's book picker, and the web upload page's library view, since they all go through the same `listBooks()`.
- **Progress** -- cycles Percent / Fraction / Bar / Off. One at a time, not layered -- Bar shows the thin bottom bar instead of corner text, Off shows neither. Fraction shows the current chapter out of the book's total chapter count, not a page count -- see [Reading progress](#reading-progress) below for why.
- **Text size** -- cycles Small / Medium / Large / X-Large (12/16/24/32px). Medium is the original size and still the default. Changing it re-paginates the book you're currently reading from wherever you are, so the same byte position just reflows into more or fewer, longer or shorter lines -- no lost place, but page numbers and the "page up" history for the current chapter both recompute against the new layout, so don't expect the *page count* to mean the same thing across a size change, only the reading position itself.
- **Controls** -- not a setting to change, just opens a scrollable reference screen listing what every button/dial does, split into "in reader" vs. "in menus" (see the [button table](#hardware) above). Dial up/down scrolls it; Top or dial press both just return to Home, since there's nothing on this screen to select.
- **Factory reset** -- wipes every book, reading position, and bookmark on the device, plus every setting on this screen, then reboots. Same Yes/No confirmation as deleting a book; there's no undo.

That's 6 options plus the header -- one more than fits in a single screen at this font size, so this screen scrolls now (same up/down-past-the-edge behavior as Choose Book), rather than trying to cram everything in or drop something else to make room.

Font size is deliberately not adjustable: `BOOK_MAX_LINES`/`BOOK_CHARS_PER_LINE` are derived from a fixed font, and changing it at runtime would need re-paginating the currently-open book and invalidating the page cache -- more risk than the other settings here for a first pass. Battery warning was also considered and skipped: this board has no battery-sense ADC pin wired up in the current hardware, so there's nothing for firmware to read without a hardware modification first.

## Reading progress

The reading screen can show how far you are into the current book one of four ways (Settings -- Progress, see [Settings](#settings) above), all driven by the current page's start offset:

- **Percent** -- corner text, cheap (just `pageStart / fileSize`, no scanning).
- **Fraction** -- corner text, "current page / total pages" in the book. Unlike Percent, this needs a real pagination sweep -- see below.
- **Bar** -- a thin (2px) bar hugging the very bottom edge of the screen, filled proportionally, instead of any corner text.
- **Off** -- neither.

Percent and Fraction share the same corner placement: the end of the last visible line rather than a reserved row of its own, so neither costs any reading space -- but that means the line needs to actually make room for it. If the line's own text would otherwise collide with it, the line trims back a whole word at a time (never mid-word) until there's genuine space; short lines that already had room keep every word untouched.

**Fraction's cost.** There's no way to know "page 42 of 300" without actually replaying word-wrap pagination -- unlike Percent (a cheap byte-offset ratio) or the old chapter-based version this used to show, a real page count means calling the same pagination logic used for rendering itself, repeatedly, either across the whole book (for the total) or up to the current position (to know which page that is). To keep this from slowing down every single page turn, only jumps (opening a book, jumping to a bookmark, or switching Settings -- Progress to Fraction) trigger a fresh sweep; turning pages normally with Top/Bottom just steps the page number by one instead, and chapter skip looks its target page number up for free in a table built during that same sweep rather than replaying pagination all over again for every skip. Both sweeps only run while Fraction is actually selected -- picking Percent, Bar, or Off skips them entirely, so there's no cost unless you're using this specific mode. On a large book, expect a book-open in Fraction mode to take noticeably longer than in the other modes (same order of cost as `indexChapters()`, but per-line word-wrap work instead of a byte scan, so slower) -- chapter skips within that same book, once open, stay fast.

The Bookmarks screen's slot list (see [Bookmarks](#bookmarks) above) always shows both Percent and Fraction together for each slot, regardless of this setting -- unlike the reading screen (which re-renders on every page turn, so picking a cheap format matters), Bookmarks is a one-shot render, so paying for both there is a small, bounded cost rather than a per-frame one. Since it can be browsing a different book's bookmarks than whatever's actually open for reading, it keeps its own separate page-count sweep (computed once per visit to that screen, not on every cursor move between the 3 slots) rather than reusing the reading screen's numbers.

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
4. Open `http://192.168.4.1` and upload a `.txt` or `.epub` file (EPUBs convert automatically — see [Chapters](#chapters-and-the-epub-converter)). It becomes the active book immediately.
5. Press the dial (or tap Top) to back out of the Wi-Fi screen when done — Wi-Fi turns off automatically.
6. Use Top/Bottom to turn pages, the dial to skip chapters, dial press for the Home menu, hold Top to save a bookmark (see [Bookmarks](#bookmarks)).
