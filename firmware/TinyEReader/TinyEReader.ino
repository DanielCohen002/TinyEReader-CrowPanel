/*
  Tiny ESP32-S3 E-Reader
  Elecrow CrowPanel ESP32 E-Paper HMI 2.13" (model DIE01021S, SSD1680Z/JD79661)

  Features:
  - Wi-Fi upload page at http://192.168.4.1, only powered on while the
    Connect to Wi-Fi screen is showing -- off the rest of the time to save
    battery, including at boot
  - Multi-book library: each upload adds a book instead of replacing one.
    Choose Book has a Yes/No confirm dialog before deleting one
  - Remembers reading position per book, boots straight into the last book read
  - True sequential "page up"/"page down": always the page whose content
    immediately precedes/follows what's on screen, computed by replaying
    forward pagination from the current chapter's start (cached so
    repeated/held presses don't redo that work) -- not an undo-last-jump
    stack, which would land on wherever you were before your last chapter
    skip instead
  - Hold Top while reading to save/overwrite a bookmark (up to 3 per book,
    separate from the automatic "current place" resume position -- see the
    Bookmarks screen and BOOKMARK_HOLD_MS below)
  - Chapter skip (dial rotate while reading) using form-feed markers a book
    may contain -- see tools/epub_to_txt.py
  - Common "smart" typographic punctuation (curly quotes, em/en dashes,
    ellipses) is transliterated to plain ASCII on the fly, since the font
    only has ASCII glyphs and would otherwise silently drop the rest of
    whatever line it appeared on
  - A single newline in the source text is treated as a word separator, not
    a forced line break, so hard-wrapped plain .txt ebooks (Project
    Gutenberg-style, one fixed-width source line per line) reflow to this
    screen's width instead of leaving a gap at every source line boundary
  - Home menu (Resume Last Book / Choose Book / Bookmarks / Connect to Wi-Fi /
    Settings) -- 5 items, shown 3 at a time as two pages (dial past the 3rd
    item to reach the other 2), see HOME_ITEM_COUNT below
  - Settings screen: auto-sleep timeout, deep-sleep timeout, auto page turn,
    invert display, book sort order, reading-progress indicator, text size,
    a controls reference, and factory reset -- all but the controls
    reference (see below) are persisted with the ESP32's own
    Preferences/NVS storage, not LittleFS
  - Controls reference (in Settings): a scrollable list of what every
    button/dial does, grouped into what they do while reading vs.
    everywhere else -- see HELP_LINES/renderControls()
  - Text size: 4 steps (Small/Medium/Large/X-Large, 12/16/24/32px), reflowing
    the whole book to the new width/line count from wherever you are --
    everything that depends on layout (page-fraction tracking, the "page up"
    replay cache) re-derives itself from the new size, see applyBookFont()
  - Light-sleeps the ESP32 and puts the e-paper panel to sleep after
    inactivity, wakes instantly on any button press. Optionally (off by
    default), if it's still untouched for a further stretch, drops into a
    second, deeper sleep tier for much lower current draw, after showing a
    "TinyEReader / Deep sleep" splash so it's obvious at a glance that it's
    really in that tier and not just light-sleeping -- wake from that one
    takes a few seconds (a real reboot) instead of being instant, but picks
    back up in the same book/page either way
  - Chapter skip repeats while dial up/down is held, instead of needing a
    fresh tap per chapter -- see CHAPTER_SKIP_REPEAT_DELAY_MS below
  - Bookmarks always shows both percent and page-fraction for each slot,
    regardless of the reading screen's own Settings -> Progress choice --
    see renderBookmarkSlots()

  Display driver: EPD.h / EPD_Init.h / spi.h (bundled in this sketch folder).
  These are Elecrow's own driver files for this exact panel, pulled from their
  reference repo (Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250).
  GxEPD2 was deliberately not used: this panel ships with either an SSD1680Z or
  JD79661 controller depending on batch, and only Elecrow's own driver is
  confirmed to handle both. It also owns the software SPI bit-banging and the
  GPIO7 "screen power" pin that the panel will not draw anything without.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "EPD.h"

// 1-bit icon/QR bitmaps in EPD_ShowPicture's packed format, generated with
// tools/image_to_epd.py from hand-drawn Piskel files and QR PNGs.
#include "generated/icon_book.h"
#include "generated/icon_bookshelf.h"
#include "generated/icon_bookmark.h"
#include "generated/icon_settings.h"
#include "generated/icon_wifi.h"
#include "generated/qr_wifi.h"
#include "generated/qr_webpage.h"

// ---------------- HARDWARE CONFIG ----------------
// Button/dial pins per Elecrow's own examples (2.13_key.ino) and product wiki.
//
// While reading a book: top tap = previous page, top hold = save/overwrite a
// bookmark (see BOOKMARK_HOLD_MS), bottom = next page, dial up = previous
// chapter, dial down = next chapter, dial press = Home.
//
// Everywhere else (Home, Choose Book, Bookmarks, Connect to Wi-Fi, Settings,
// the delete/reset confirm dialog): dial up/down = move selection, dial
// press = select, top = jump straight to Home from anywhere (cancels the
// confirm dialog without acting), bottom on a highlighted book in Choose
// Book or a highlighted bookmark in the Bookmarks screen opens a Yes/No
// delete confirmation.
#define BTN_MENU     2   // top button
#define BTN_BACK     1   // bottom button
#define DIAL_DOWN    4   // dial rotate down
#define DIAL_UP      6   // dial rotate up
#define DIAL_PRESS   5   // dial press

#define EPD_POWER_PIN 7  // Must be driven HIGH or the panel stays blank.

extern uint8_t ImageBW[ALLSCREEN_BYTES];  // Software framebuffer, defined in EPD.cpp.

// ---------------- APP CONFIG ----------------
const char* AP_NAME = "PocketReader";
const char* AP_PASS = "12345678";

// Per-book sanity cap. The real limit in practice is whatever free space
// handleUpload() finds at upload time (see uploadSizeLimit) -- this just
// stops one absurdly large file from being accepted. See partitions.csv:
// the LittleFS partition is sized at ~6.3MB of the board's 8MB flash.
constexpr size_t MAX_BOOK_SIZE = 3 * 1024 * 1024;
// Auto-sleep timeout is now a Settings-screen option (see cycleSleepTimeout()
// and SLEEP_PRESETS_MS below), persisted via Preferences -- sleepAfterMs is
// the live value, loaded from storage in setup(). 0 means "never sleep".
uint32_t sleepAfterMs = 60000;
const uint32_t SLEEP_PRESETS_MS[] = { 30000, 60000, 120000, 300000, 0 };
constexpr uint8_t SLEEP_PRESETS_COUNT = 5;
// Deep sleep is a second, optional tier past light sleep: once light-sleeping
// (see maybeSleep()), if there's STILL no button press after this much
// additional time, the ESP32 drops into a full deep sleep instead of staying
// light-asleep indefinitely -- much lower current draw, at the cost of a
// full reboot (a few seconds, vs. instant) to wake back up. 0 means "never
// escalate," i.e. today's behavior: light-sleep forever until a button is
// pressed. Off by default, same reasoning as autoTurnMs -- it changes
// device behavior, so it should be opt-in.
uint32_t deepSleepAfterMs = 0;
const uint32_t DEEP_SLEEP_PRESETS_MS[] = { 0, 300000, 600000, 1200000, 1800000 };
constexpr uint8_t DEEP_SLEEP_PRESETS_COUNT = 5;
// How often maybeSleep() briefly wakes from light sleep on its own (no
// button involved) to check whether the deepSleepAfterMs threshold has been
// crossed yet. Only used while deepSleepAfterMs != 0 -- coarse on purpose,
// this is just a polling interval, not user-facing.
constexpr uint64_t DEEP_SLEEP_CHECK_INTERVAL_US = 30ULL * 1000000ULL;
// Auto page turn: same idea as sleepAfterMs, 0 means "off". Reuses
// sleepPresetLabel() for its "N sec"/"N min" formatting (see renderSettings()).
uint32_t autoTurnMs = 0;
const uint32_t AUTO_TURN_PRESETS_MS[] = { 0, 5000, 30000, 60000, 120000 };
constexpr uint8_t AUTO_TURN_PRESETS_COUNT = 5;
unsigned long lastPageTurnTime = 0;  // see maybeAutoTurn()
bool invertDisplay = false;          // see toggleInvertDisplay(), applied in endFrame()
enum SortMode : uint8_t { SORT_AZ, SORT_ZA, SORT_SIZE };
SortMode sortMode = SORT_AZ;         // applied in listBooks() via sortBookList()
// Reading-screen progress indicator -- one of these four, never more than
// one at a time (see renderPageAtCore()). Declared in this order (None,
// Bar, Percent, Fraction) because cycleProgressMode() just steps the
// underlying enum value by one -- this IS the cycle order shown in
// Settings, not just a listing. Off by default: every mode past None costs
// either reading-line space (Percent/Fraction) or a lookup vs. the free
// path (Fraction's real cost, see below), so showing nothing is the
// zero-surprise starting point. PROGRESS_FRACTION shows "current page /
// total pages" -- see computeTotalPageCount()/pageNumberForOffset() for
// how, and totalPageCount/currentPageNumber below for the cost trade-off
// of keeping them accurate.
enum ProgressMode : uint8_t { PROGRESS_OFF, PROGRESS_BAR, PROGRESS_PERCENT, PROGRESS_FRACTION };
ProgressMode progressMode = PROGRESS_OFF;
// Page-fraction tracking for the currently-open-for-reading book (the
// Bookmarks screen keeps its own separate bookmarkTotalPages/
// bookmarkSlotPages instead of these, since it can be browsing a different
// book's bookmarks while a different one is actually open for reading).
// Both are only kept accurate while progressMode == PROGRESS_FRACTION --
// see computeTotalPageCount()/pageNumberForOffset() for why that guard
// exists (a real pagination replay, not free) and nextPage()/
// previousPage()/nextChapter()/previousChapter()/openBook()/
// openBookAtBookmark()/cycleProgressMode() for where they're updated.
uint32_t totalPageCount = 1;
uint32_t currentPageNumber = 1;
// Book count is bounded by flash space, not an arbitrary limit -- see
// uploadSizeLimit in handleUpload(). MAX_BOOKS is just the bookList array's
// capacity (RAM, not flash), sized well past what could realistically fit
// in the ~6.3MB library partition even with small books. The Choose Book
// screen only fits CHOOSE_VISIBLE_ROWS at a time and scrolls to show more.
constexpr uint8_t MAX_BOOKS = 64;
constexpr uint8_t CHOOSE_VISIBLE_ROWS = 5;  // one row is a free-space header, see renderChooseBook()
constexpr unsigned long BOOKMARK_HOLD_MS = 600;  // hold Top this long while reading to save a bookmark
// Holding dial up/down while reading keeps skipping chapters instead of
// needing a fresh tap per chapter -- the first skip still fires instantly
// on press same as always, then if it's still held this much longer it
// starts repeating, at this interval, until released. See handleButtons().
constexpr unsigned long CHAPTER_SKIP_REPEAT_DELAY_MS = 600;
constexpr unsigned long CHAPTER_SKIP_REPEAT_INTERVAL_MS = 350;

// Book text layout. Font size is a Settings-screen option (Text size, see
// cycleTextSize()/TEXT_SIZE_PRESETS_PX below) -- bookFont and everything
// derived from it (bookCharWidth/bookLineHeight/bookCharsPerLine/
// bookMaxLines) are runtime variables, not compile-time constants, kept in
// sync by applyBookFont() below. Only BOOK_LEFT_MARGIN/BOOK_TOP_MARGIN are
// unaffected by font size and stay as constexpr.
//
// All 5 baked-in bitmap fonts (EPD_ShowChar/EPDfont.h) are fixed-width with
// glyph width = height/2, so every derived value below is a pure function
// of bookFont -- the same formulas reproduce today's 16px numbers (8/20/
// 30/6) exactly, which is a good sanity check they're right. 48px isn't
// offered: at that size a "page" is only 2 lines of ~10 characters, too
// choppy to read continuously.
constexpr uint16_t BOOK_LEFT_MARGIN = 4;
constexpr uint16_t BOOK_TOP_MARGIN = 1;
// Upper bound on lines-per-page across every offered font size (7, at the
// smallest/12px preset) -- used only to size the `lines[]` stack array in
// renderPageAtCore(), since that needs a compile-time length. The actual
// per-frame line count is always the runtime bookMaxLines, which never
// exceeds this.
constexpr uint8_t BOOK_MAX_LINES_CAP = 8;
const uint32_t TEXT_SIZE_PRESETS_PX[] = { 12, 16, 24, 32 };
constexpr uint8_t TEXT_SIZE_PRESETS_COUNT = 4;
uint8_t bookFont = 16;
uint8_t bookCharWidth = 8;
uint16_t bookLineHeight = 20;
uint8_t bookCharsPerLine = 30;
uint8_t bookMaxLines = 6;

// Menu screen layout (8x16 font -- smaller, so list items/labels fit comfortably).
constexpr uint16_t MENU_LEFT_MARGIN = 6;
constexpr uint16_t MENU_TOP_MARGIN = 4;
constexpr uint16_t MENU_LINE_HEIGHT = 20;
constexpr uint8_t MENU_FONT = 16;

WebServer server(80);

enum AppScreen : uint8_t {
  SCREEN_READING, SCREEN_HOME, SCREEN_CHOOSE_BOOK, SCREEN_WIFI_INFO, SCREEN_CONFIRM_DELETE,
  SCREEN_BOOKMARK_BOOKS, SCREEN_BOOKMARK_SLOTS, SCREEN_SETTINGS, SCREEN_CONTROLS
};
AppScreen currentScreen = SCREEN_READING;

// renderConfirmDelete()/SCREEN_CONFIRM_DELETE is shared by all three
// destructive-or-impactful actions in the app (book deletion, bookmark
// deletion, factory reset) rather than having three near-identical Yes/No
// screens -- confirmAction picks which prompt/action applies.
enum ConfirmAction : uint8_t { CONFIRM_DELETE_BOOK, CONFIRM_DELETE_BOOKMARK, CONFIRM_FACTORY_RESET };
ConfirmAction confirmAction = CONFIRM_DELETE_BOOK;

constexpr uint8_t HOME_ITEM_COUNT = 5;
uint8_t homeSelection = 0;

String bookList[MAX_BOOKS];
uint8_t bookCount = 0;
uint8_t bookFileTotal = 0;  // like bookCount but not capped at MAX_BOOKS -- see listBooks()
uint8_t chooseSelection = 0;
uint8_t chooseWindowStart = 0;  // first bookList index shown on screen, see scrollChooseWindow()
bool confirmDeleteYes = false;  // which option is highlighted in the confirm dialog; defaults to No

// Bookmarks screen state: bookmarkBookSelection/bookmarkWindowStart pick
// which book's bookmarks to view (own scroll state, separate from Choose
// Book's, since either screen can be entered independently from Home).
// bookmarkOffsets is loaded fresh from disk (see loadBookmarks()) whenever
// SCREEN_BOOKMARK_SLOTS is entered, and written back on save/delete.
uint8_t bookmarkBookSelection = 0;
uint8_t bookmarkWindowStart = 0;
uint8_t bookmarkSlotSelection = 0;
constexpr uint8_t BOOKMARK_SLOT_COUNT = 3;
constexpr uint32_t BOOKMARK_EMPTY = 0xFFFFFFFF;
uint32_t bookmarkOffsets[BOOKMARK_SLOT_COUNT];
// Page-fraction cache for the Bookmarks screen -- shown there alongside
// percent regardless of the Settings -> Progress choice (see
// renderBookmarkSlots()). Computed once in enterBookmarkSlots(), not on
// every render, since moving the cursor between the 3 slots re-renders far
// more often than the book being browsed actually changes, and each of
// these numbers is a real pagination sweep (see computeTotalPageCount()/
// pageNumberForOffset()) through a book that isn't necessarily the one
// currently open for reading.
uint32_t bookmarkTotalPages = 0;
uint32_t bookmarkSlotPages[BOOKMARK_SLOT_COUNT];

// Settings screen state. Persisted settings live in NVS via Preferences, not
// LittleFS -- see setup() for the load and each cycle*()/toggle*() function
// for the save.
Preferences prefs;
// 0 = sleep timeout, 1 = deep sleep timeout, 2 = auto page turn,
// 3 = invert display, 4 = book sort, 5 = progress indicator, 6 = text size,
// 7 = controls reference, 8 = factory reset. More items than fit on screen
// at once now, so this scrolls the same way Choose Book does -- see
// SETTINGS_VISIBLE_ROWS/settingsWindowStart/scrollSettingsWindow().
constexpr uint8_t SETTINGS_ITEM_COUNT = 9;
constexpr uint8_t SETTINGS_VISIBLE_ROWS = 5;
uint8_t settingsSelection = 0;
uint8_t settingsWindowStart = 0;

// Controls screen state -- a pure scrollable reference (see HELP_LINES/
// renderControls() below), no per-row selection/action like Settings has,
// just a scroll position.
uint8_t helpScrollOffset = 0;

String currentBookName;  // filename only, e.g. "MyNovel.txt" -- lives under /books/

File book;
size_t fileSize = 0;
uint32_t pageStart = 0;
uint32_t pageEnd = 0;
unsigned long lastActivity = 0;

// Chapter boundaries are marked in the uploaded .txt with a form-feed byte
// (0x0C, '\f') -- see tools/epub_to_txt.py, which inserts one at each
// detected chapter heading when converting an EPUB. Plain hand-typed .txt
// files with no form feeds just get a single implicit "chapter" at offset 0.
constexpr uint16_t MAX_CHAPTERS = 200;
uint32_t chapterOffsets[MAX_CHAPTERS];
uint16_t chapterCount = 0;
// Page number (1-based) that each chapterOffsets[i] falls on -- filled in
// by computeTotalPageCount() as a side effect of its own full sweep (see
// there), so nextChapter()/previousChapter() can look a chapter's page
// number up for free in PROGRESS_FRACTION mode instead of each running
// their own separate O(book length) sweep via pageNumberForOffset() on
// every single skip. Only valid exactly when totalPageCount is (same
// PROGRESS_FRACTION-only guard, refreshed together by refreshPageTracking()).
uint32_t chapterPageNumbers[MAX_CHAPTERS];

// Page boundaries depend on word-wrapping, so there's no way to compute
// "the page before this byte offset" directly -- see findPreviousPageStart.
// This caches the most recent forward replay so repeated/held "page up"
// presses don't redo it from scratch each time.
constexpr uint16_t MAX_CACHED_PAGES = 300;
uint32_t cachedPageStarts[MAX_CACHED_PAGES];
uint16_t cachedPageCount = 0;
uint32_t cachedChapterStart = 0xFFFFFFFF;

const char uploadPage[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>PocketReader</title>
  <style>
    :root { color-scheme: light dark; }
    body {
      font-family: system-ui, sans-serif; line-height: 1.4; margin: 0;
      padding: 2rem 1rem; background: #f2efe9; color: #1c1a17;
      display: flex; justify-content: center;
    }
    .card {
      width: 100%; max-width: 26rem; background: #fffdf9;
      border-radius: 0.75rem; padding: 1.5rem 1.5rem 1.75rem;
      box-shadow: 0 1px 3px rgba(0,0,0,0.12);
    }
    h1 { font-size: 1.4rem; margin: 0 0 0.25rem; }
    .sub { color: #6b6357; font-size: 0.9rem; margin: 0 0 1.25rem; }
    .drop {
      display: block; box-sizing: border-box;
      border: 2px dashed #c9c0b0; border-radius: 0.6rem; padding: 1.5rem 1rem;
      text-align: center; cursor: pointer; transition: border-color 0.15s, background 0.15s;
    }
    .drop.drag { border-color: #8a7a5c; background: #f6f1e6; }
    .drop p { margin: 0.25rem 0; }
    .drop .hint { font-size: 0.85rem; color: #8a8072; }
    input[type=file] { display: none; }
    button {
      font: inherit; margin-top: 1rem; width: 100%; padding: 0.65rem;
      border: none; border-radius: 0.5rem; background: #3d3527; color: #fff;
      cursor: pointer;
    }
    button:disabled { background: #b6ac9a; cursor: not-allowed; }
    .files { margin-top: 1rem; font-size: 0.9rem; }
    .file-row { display: flex; justify-content: space-between; gap: 0.5rem; padding: 0.15rem 0; }
    .file-row .status { color: #8a8072; white-space: nowrap; }
    .file-row.done .status { color: #3c7a3c; }
    .file-row.error .status { color: #b03a2e; }
    h2 { font-size: 1rem; margin: 1.75rem 0 0.5rem; }
    .lib-meta { font-size: 0.85rem; color: #6b6357; margin: 0 0 0.5rem; }
    ul.lib { list-style: none; margin: 0; padding: 0; font-size: 0.9rem; }
    ul.lib li { display: flex; align-items: center; gap: 0.5rem; padding: 0.35rem 0; border-top: 1px solid #ece7dc; }
    ul.lib li .name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    ul.lib li .size { color: #8a8072; white-space: nowrap; }
    ul.lib li .del {
      flex: none; width: auto; margin-top: 0; padding: 0.2rem 0.55rem; font-size: 0.8rem;
      border: 1px solid #c9c0b0; border-radius: 0.35rem; background: transparent; color: #8a6b5c;
    }
    ul.lib li .del:hover { border-color: #b03a2e; color: #b03a2e; }
    .empty { color: #8a8072; font-size: 0.9rem; }
    @media (prefers-color-scheme: dark) {
      body { background: #171512; color: #ece7dc; }
      .card { background: #201d19; box-shadow: none; }
      .sub, .lib-meta, .empty, .drop .hint, .file-row .status { color: #a49b8a; }
      .drop { border-color: #4a4438; }
      .drop.drag { border-color: #8a7a5c; background: #2a251e; }
      button { background: #ece7dc; color: #201d19; }
      button:disabled { background: #4a4438; color: #a49b8a; }
      ul.lib li { border-top-color: #332e27; }
      ul.lib li .del { border-color: #4a4438; color: #c9b8a8; background: transparent; }
      ul.lib li .del:hover { border-color: #d47a6a; color: #d47a6a; }
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>PocketReader</h1>
    <p class="sub">Upload one or more .txt or .epub books (EPUBs are converted automatically). Open a book from the device's Choose Book screen.</p>

    <form id="form">
      <label class="drop" id="drop" for="file">
        <p id="dropLabel">Tap to choose books</p>
        <p class="hint">or drag &amp; drop .txt/.epub files here</p>
      </label>
      <input type="file" id="file" name="file" accept=".txt,.epub,text/plain,application/epub+zip" multiple>
      <button type="submit" id="go" disabled>Upload</button>
    </form>
    <div class="files" id="files"></div>

    <h2>Library</h2>
    <p class="lib-meta" id="libMeta">Loading...</p>
    <ul class="lib" id="libList"></ul>
  </div>

  <script src="epub-to-txt.js"></script>
  <script>
    var fileInput = document.getElementById('file');
    var drop = document.getElementById('drop');
    var dropLabel = document.getElementById('dropLabel');
    var go = document.getElementById('go');
    var filesBox = document.getElementById('files');
    var form = document.getElementById('form');
    var picked = [];

    function fmtSize(n) {
      if (n > 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + 'MB';
      if (n > 1024) return Math.round(n / 1024) + 'KB';
      return n + 'B';
    }

    function refreshLibrary() {
      fetch('/books').then(function (r) { return r.json(); }).then(function (data) {
        var meta = data.count + ' book' + (data.count === 1 ? '' : 's') + ' - ' + data.free;
        if (data.hidden > 0) {
          meta += ' (+' + data.hidden + ' more on the device you can\'t see or delete here - ' +
            'delete a visible book to make room, which will reveal one of them)';
        }
        document.getElementById('libMeta').textContent = meta;
        var list = document.getElementById('libList');
        list.innerHTML = '';
        if (data.books.length === 0) {
          var li = document.createElement('li');
          li.className = 'empty';
          li.textContent = 'No books yet.';
          list.appendChild(li);
        }
        data.books.forEach(function (b) {
          var li = document.createElement('li');
          var name = document.createElement('span');
          name.className = 'name';
          name.textContent = b.name;
          var size = document.createElement('span');
          size.className = 'size';
          size.textContent = fmtSize(b.size);
          var del = document.createElement('button');
          del.type = 'button';
          del.className = 'del';
          del.textContent = 'Delete';
          del.addEventListener('click', function () { deleteBook(b.name); });
          li.appendChild(name);
          li.appendChild(size);
          li.appendChild(del);
          list.appendChild(li);
        });
      }).catch(function () {
        document.getElementById('libMeta').textContent = '';
      });
    }

    function deleteBook(name) {
      if (!confirm('Delete "' + name + '"? This can\'t be undone.')) return;
      fetch('/delete?name=' + encodeURIComponent(name), { method: 'POST' })
        .then(refreshLibrary)
        .catch(refreshLibrary);
    }

    function isEpub(file) { return /\.epub$/i.test(file.name); }
    function isTxt(file) { return /\.txt$/i.test(file.name); }

    function setPicked(fileList) {
      picked = Array.prototype.filter.call(fileList, function (f) {
        return isTxt(f) || isEpub(f);
      });
      dropLabel.textContent = picked.length === 0
        ? 'Tap to choose books'
        : picked.length + ' book' + (picked.length > 1 ? 's' : '') + ' selected';
      go.disabled = picked.length === 0;
      filesBox.innerHTML = '';
    }

    fileInput.addEventListener('change', function () { setPicked(fileInput.files); });

    ['dragenter', 'dragover'].forEach(function (evt) {
      drop.addEventListener(evt, function (e) { e.preventDefault(); drop.classList.add('drag'); });
    });
    ['dragleave', 'drop'].forEach(function (evt) {
      drop.addEventListener(evt, function (e) { e.preventDefault(); drop.classList.remove('drag'); });
    });
    drop.addEventListener('drop', function (e) {
      setPicked(e.dataTransfer.files);
    });

    function addFileRow(displayName) {
      var row = document.createElement('div');
      row.className = 'file-row';
      var name = document.createElement('span');
      name.textContent = displayName;
      var status = document.createElement('span');
      status.className = 'status';
      row.appendChild(name);
      row.appendChild(status);
      filesBox.appendChild(row);
      return { row: row, status: status };
    }

    function bookTitleFromFilename(name) {
      return name.replace(/\.(txt|epub)$/i, '');
    }

    // Runs epubToTxt() (from epub-to-txt.js) on an .epub File and returns a
    // plain-text File with the same base name, reusing the same row (r) the
    // caller already created so the status label just updates in place
    // rather than the row disappearing and a new one appearing.
    function convertIfNeeded(file, r) {
      if (!isEpub(file)) return Promise.resolve(file);
      r.status.textContent = 'Converting...';
      return file.arrayBuffer().then(function (buf) {
        return window.epubToTxt(buf);
      }).then(function (text) {
        return new File([text], bookTitleFromFilename(file.name) + '.txt', { type: 'text/plain' });
      }).catch(function (err) {
        console.error('EPUB conversion failed for', file.name, err);
        r.row.classList.add('error');
        r.status.textContent = 'Conversion failed';
        return null;
      });
    }

    function uploadOne(file, r) {
      return new Promise(function (resolve) {
        r.status.textContent = '0%';

        var xhr = new XMLHttpRequest();
        xhr.open('POST', '/upload');
        xhr.upload.onprogress = function (e) {
          if (e.lengthComputable) r.status.textContent = Math.round((e.loaded / e.total) * 100) + '%';
        };
        xhr.onload = function () {
          if (xhr.status === 200) {
            r.row.classList.add('done');
            r.status.textContent = 'Done';
          } else {
            r.row.classList.add('error');
            r.status.textContent = xhr.status === 413 ? 'Too large' : 'Failed';
          }
          resolve();
        };
        xhr.onerror = function () {
          r.row.classList.add('error');
          r.status.textContent = 'Failed';
          resolve();
        };
        var data = new FormData();
        data.append('file', file);
        xhr.send(data);
      });
    }

    // Mirrors handleUpload()'s own acceptance check (min(maxSize, freeBytes -
    // 8192), freeBytes shrinking as earlier books in the queue are counted)
    // so books that can't possibly fit are rejected up front instead of
    // failing partway through a batch after using up real upload time.
    function planQueue(files, freeBytes, maxSize) {
      var available = freeBytes;
      return files.map(function (file) {
        var allowed = available > 8192 ? Math.min(maxSize, available - 8192) : 0;
        var fits = file.size <= allowed;
        if (fits) available -= file.size;
        return fits;
      });
    }

    form.addEventListener('submit', function (e) {
      e.preventDefault();
      if (picked.length === 0) return;
      go.disabled = true;
      filesBox.innerHTML = '';

      // One row per originally-picked file, created up front so its status
      // label can just update in place through convert -> upload rather than
      // rows disappearing/reappearing as files get transformed.
      var rows = picked.map(function (file) { return addFileRow(file.name); });

      var converted = picked.reduce(function (chain, file, i) {
        return chain.then(function (acc) {
          return convertIfNeeded(file, rows[i]).then(function (result) {
            if (result) acc.push({ file: result, row: rows[i] });
            return acc;
          });
        });
      }, Promise.resolve([]));

      converted.then(function (ready) {
        if (ready.length === 0) return Promise.resolve();
        return fetch('/books').then(function (r) { return r.json(); }).then(function (data) {
          var files = ready.map(function (item) { return item.file; });
          var fits = planQueue(files, data.freeBytes, data.maxSize);
          var chain = Promise.resolve();
          ready.forEach(function (item, i) {
            if (fits[i]) {
              chain = chain.then(function () { return uploadOne(item.file, item.row); });
            } else {
              item.row.row.classList.add('error');
              item.row.status.textContent = "Won't fit - skipped";
            }
          });
          return chain;
        });
      }).then(function () {
        picked = [];
        fileInput.value = '';
        dropLabel.textContent = 'Tap to choose books';
        refreshLibrary();
      });
    });

    refreshLibrary();
  </script>
</body>
</html>
)rawliteral";

// Client-side EPUB-to-.txt converter, served at /epub-to-txt.js and loaded
// by uploadPage above. Runs entirely in the browser (this device has no
// internet access to fetch a library from, and parsing a whole EPUB on the
// ESP32 itself isn't worth the flash/RAM/complexity) -- unzips the EPUB with
// a small hand-rolled zip reader (using the browser's own DecompressionStream
// for the actual inflate), then walks each spine document's HTML with
// DOMParser to extract plain text, inserting a form-feed chapter marker at
// each real <h1>/<h2>/<h3> heading (or one marker per spine file if the book
// has fewer than 2 headings total) -- the same logic as tools/epub_to_txt.py,
// kept in sync with it by hand since there's no build step to share the two.
const char epubToTxtScript[] PROGMEM = R"rawliteral(
(function () {
  'use strict';

  function normalizePath(path) {
    var parts = path.split('/');
    var out = [];
    for (var i = 0; i < parts.length; i++) {
      var part = parts[i];
      if (part === '' || part === '.') continue;
      if (part === '..') out.pop();
      else out.push(part);
    }
    return out.join('/');
  }

  function readZip(buf) {
    var view = new DataView(buf);
    var bytes = new Uint8Array(buf);
    var eocdSig = 0x06054b50;
    var pos = -1;
    var minPos = Math.max(0, bytes.length - 65557);
    for (var i = bytes.length - 22; i >= minPos; i--) {
      if (view.getUint32(i, true) === eocdSig) { pos = i; break; }
    }
    if (pos === -1) throw new Error("Not a valid EPUB (zip end-of-directory record not found).");

    var cdEntries = view.getUint16(pos + 10, true);
    var cdOffset = view.getUint32(pos + 16, true);

    var entries = {};
    var offset = cdOffset;
    for (var e = 0; e < cdEntries; e++) {
      var sig = view.getUint32(offset, true);
      if (sig !== 0x02014b50) throw new Error("Corrupt EPUB (bad zip central directory entry).");
      var method = view.getUint16(offset + 10, true);
      var compSize = view.getUint32(offset + 20, true);
      var nameLen = view.getUint16(offset + 28, true);
      var extraLen = view.getUint16(offset + 30, true);
      var commentLen = view.getUint16(offset + 32, true);
      var localOffset = view.getUint32(offset + 42, true);
      var name = new TextDecoder('utf-8').decode(bytes.subarray(offset + 46, offset + 46 + nameLen));
      entries[name] = { method: method, compSize: compSize, localOffset: localOffset };
      offset += 46 + nameLen + extraLen + commentLen;
    }

    async function readEntry(name) {
      var meta = entries[name];
      if (!meta) throw new Error('Missing "' + name + '" in EPUB.');
      var lo = meta.localOffset;
      var lNameLen = view.getUint16(lo + 26, true);
      var lExtraLen = view.getUint16(lo + 28, true);
      var dataStart = lo + 30 + lNameLen + lExtraLen;
      var compData = bytes.subarray(dataStart, dataStart + meta.compSize);
      if (meta.method === 0) return compData;
      if (meta.method === 8) {
        if (typeof DecompressionStream === 'undefined') {
          throw new Error("This browser can't unzip EPUBs (no DecompressionStream support) -- try a newer version of Chrome, Edge, Firefox, or Safari, or use tools/epub_to_txt.py instead.");
        }
        var stream = new Blob([compData]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
        var out = await new Response(stream).arrayBuffer();
        return new Uint8Array(out);
      }
      throw new Error('Unsupported zip compression method ' + meta.method + ' for "' + name + '".');
    }

    return { readEntry: readEntry };
  }

  function getByLocalName(doc, localName) {
    var all = doc.getElementsByTagName('*');
    var out = [];
    for (var i = 0; i < all.length; i++) {
      if (all[i].localName === localName) out.push(all[i]);
    }
    return out;
  }

  var HEADING_TAGS = { H1: true, H2: true, H3: true };
  var SKIP_TAGS = { SCRIPT: true, STYLE: true, HEAD: true };
  var BLOCK_TAGS = { P: true, DIV: true, LI: true, TR: true, BLOCKQUOTE: true, BR: true };

  function extractChapterAwareText(html) {
    var doc = new DOMParser().parseFromString(html, 'text/html');
    var chunks = [];
    var headingOffsets = [];
    var length = 0;
    var atLineStart = true;

    function emit(text) {
      if (!text) return;
      chunks.push(text);
      length += text.length;
      atLineStart = text.charAt(text.length - 1) === '\n';
    }
    function breakParagraph() {
      if (!atLineStart) emit('\n\n');
    }
    function walk(node) {
      if (node.nodeType === 3) {  // TEXT_NODE
        var text = node.nodeValue.replace(/[ \t\r\f\v]+/g, ' ');
        if (text.trim() === '') return;
        if (atLineStart) text = text.replace(/^ +/, '');
        emit(text);
        return;
      }
      if (node.nodeType !== 1) return;  // ELEMENT_NODE
      var tag = node.tagName;
      if (SKIP_TAGS[tag]) return;
      var isHeading = HEADING_TAGS[tag];
      var isBlock = BLOCK_TAGS[tag];
      if (isHeading) {
        breakParagraph();
        headingOffsets.push(length);
      } else if (isBlock) {
        breakParagraph();
      }
      for (var c = 0; c < node.childNodes.length; c++) walk(node.childNodes[c]);
      if (isHeading || isBlock) breakParagraph();
    }
    walk(doc.body || doc.documentElement);
    return { text: chunks.join('').trim(), headingOffsets: headingOffsets };
  }

  async function epubToTxt(arrayBuffer) {
    var zip = readZip(arrayBuffer);

    var containerBytes = await zip.readEntry('META-INF/container.xml');
    var containerXml = new DOMParser().parseFromString(new TextDecoder('utf-8').decode(containerBytes), 'application/xml');
    var rootfiles = getByLocalName(containerXml, 'rootfile');
    if (rootfiles.length === 0) throw new Error("Could not find the EPUB's content file (missing container.xml rootfile).");
    var opfPath = rootfiles[0].getAttribute('full-path');
    var opfDir = opfPath.indexOf('/') === -1 ? '' : opfPath.substring(0, opfPath.lastIndexOf('/'));

    var opfBytes = await zip.readEntry(opfPath);
    var opfXml = new DOMParser().parseFromString(new TextDecoder('utf-8').decode(opfBytes), 'application/xml');

    var manifest = {};
    getByLocalName(opfXml, 'item').forEach(function (item) {
      var href = item.getAttribute('href');
      try { href = decodeURIComponent(href); } catch (e) {}
      manifest[item.getAttribute('id')] = href;
    });

    var spinePaths = [];
    getByLocalName(opfXml, 'itemref').forEach(function (itemref) {
      var href = manifest[itemref.getAttribute('idref')];
      if (!href) return;
      var path = opfDir ? opfDir + '/' + href : href;
      spinePaths.push(normalizePath(path));
    });
    if (spinePaths.length === 0) throw new Error('Could not find any readable content in this EPUB.');

    var fileTexts = [];
    var fileHeadingOffsets = [];
    for (var i = 0; i < spinePaths.length; i++) {
      var bytes;
      try {
        bytes = await zip.readEntry(spinePaths[i]);
      } catch (err) {
        continue;
      }
      var extracted = extractChapterAwareText(new TextDecoder('utf-8').decode(bytes));
      if (extracted.text) {
        fileTexts.push(extracted.text);
        fileHeadingOffsets.push(extracted.headingOffsets);
      }
    }
    if (fileTexts.length === 0) throw new Error('Could not extract any text from this EPUB.');

    var totalHeadings = fileHeadingOffsets.reduce(function (sum, offs) { return sum + offs.length; }, 0);
    var pieces = [];
    if (totalHeadings >= 2) {
      var isFirstHeadingOverall = true;
      for (var f = 0; f < fileTexts.length; f++) {
        var text = fileTexts[f];
        var offsets = fileHeadingOffsets[f];
        var cursor = 0;
        for (var h = 0; h < offsets.length; h++) {
          var off = offsets[h];
          if (isFirstHeadingOverall) { isFirstHeadingOverall = false; continue; }
          pieces.push(text.substring(cursor, off));
          pieces.push('\f');
          cursor = off;
        }
        pieces.push(text.substring(cursor));
        pieces.push('\n\n');
      }
    } else {
      for (var j = 0; j < fileTexts.length; j++) {
        if (j > 0) pieces.push('\f');
        pieces.push(fileTexts[j]);
        pieces.push('\n\n');
      }
    }
    return pieces.join('').trim() + '\n';
  }

  window.epubToTxt = epubToTxt;
})();
)rawliteral";

// ---------------- UTIL ----------------
bool pressed(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

void touchActivity() {
  lastActivity = millis();
}

String bookPath(const String& name) {
  return "/books/" + name;
}

String posPath(const String& name) {
  return "/books/" + name + ".pos";
}

String bookmarkPath(const String& name) {
  return "/books/" + name + ".bm";
}

// Bookmarks file is BOOKMARK_SLOT_COUNT lines, each either a decimal byte
// offset or blank for an unused slot -- deliberately separate from posPath()
// ("current place"), which is the automatic resume position and must never
// be touched by bookmark save/jump/delete (see saveBookmark(),
// openBookAtBookmark()).
void loadBookmarks(const String& name, uint32_t offsets[BOOKMARK_SLOT_COUNT]) {
  for (uint8_t i = 0; i < BOOKMARK_SLOT_COUNT; i++) offsets[i] = BOOKMARK_EMPTY;
  File f = LittleFS.open(bookmarkPath(name), "r");
  if (!f) return;
  for (uint8_t i = 0; i < BOOKMARK_SLOT_COUNT && f.available(); i++) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) offsets[i] = (uint32_t)line.toInt();
  }
  f.close();
}

void saveBookmarks(const String& name, const uint32_t offsets[BOOKMARK_SLOT_COUNT]) {
  File f = LittleFS.open(bookmarkPath(name), "w");
  if (!f) return;
  for (uint8_t i = 0; i < BOOKMARK_SLOT_COUNT; i++) {
    if (offsets[i] != BOOKMARK_EMPTY) f.println(offsets[i]);
    else f.println();
  }
  f.close();
}

// Filenames always end in ".txt" (see sanitizeFilename()) -- strip it for
// display purposes so messages read as a title, not a filename.
String bookTitle(const String& name) {
  return name.endsWith(".txt") ? name.substring(0, name.length() - 4) : name;
}

// 26 characters is what actually fits on one MENU_FONT-width row at
// MENU_LEFT_MARGIN on this screen -- used for book names, titles, and other
// one-line labels shown across the menu screens (Choose Book, Bookmarks,
// the confirm dialog, and the "Opening..." message).
String truncateForRow(String text) {
  if (text.length() > 26) text = text.substring(0, 23) + "...";
  return text;
}

String freeSpaceLabel() {
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  char buf[24];
  snprintf(buf, sizeof(buf), "%uKB free", (unsigned)(freeBytes / 1024));
  return String(buf);
}

// A full EPD_Init/ALL_Fill/Update/Clear_R26H cycle flashes the whole panel
// and is only actually needed once after power-on, or after the panel has
// been put to sleep (it needs a hardware reset to wake back up). Running
// that full cycle before every single page turn -- which the first version
// of this fix did, to work around ghosting -- made every page flash several
// times and feel sluggish. The real fix for ghosting is EPD_SyncOldData:
// keeping the "previous frame" register (R26H) in sync with what's actually
// on screen so each partial update diffs against the right reference.
bool epdNeedsFullInit = true;

void beginFrame() {
  if (epdNeedsFullInit) {
    EPD_Init();
    EPD_ALL_Fill(WHITE);
    EPD_Update();
    EPD_Clear_R26H();
    epdNeedsFullInit = false;
  }

  // In the ImageBW software framebuffer (as opposed to the raw WHITE/BLACK
  // register values above), a set bit means BLACK and a clear bit means
  // WHITE -- see EPD_DrawPoint. So "clear to white" means filling with 0x00,
  // not the WHITE constant.
  memset(ImageBW, 0x00, ALLSCREEN_BYTES);
}

void endFrame() {
  // Invert display: EPD_DisplayImage() already sends ~ImageBW[i] to the
  // panel (see EPD_Init.cpp) -- flipping the whole software framebuffer
  // here first, before that, is the single cheap place to support a
  // global invert without touching any of the individual draw calls
  // scattered through this file. ALLSCREEN_BYTES is tiny (~3.8KB for this
  // panel), so a full pass over it every frame is negligible.
  if (invertDisplay) {
    for (uint32_t i = 0; i < ALLSCREEN_BYTES; i++) ImageBW[i] = ~ImageBW[i];
  }
  EPD_DisplayImage(ImageBW);
  EPD_PartUpdate();
  EPD_SyncOldData(ImageBW);
}

void showMessage(const String& message) {
  beginFrame();

  uint16_t y = MENU_TOP_MARGIN;
  int start = 0;
  String msg = message;
  while (start < (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    String line = (nl == -1) ? msg.substring(start) : msg.substring(start, nl);
    EPD_ShowString(MENU_LEFT_MARGIN, y, line.c_str(), BLACK, MENU_FONT);
    y += MENU_LINE_HEIGHT;
    start = (nl == -1) ? msg.length() : nl + 1;
  }

  endFrame();
}

// Drawn right before the device actually drops into deep sleep (see
// maybeSleep()) -- deliberately unmistakable for the ordinary reading
// screen, since the whole point is being able to glance at it and know
// it's really in the deep-sleep tier, not just light-sleeping (which
// leaves whatever page was already showing untouched). The panel holds
// this image with zero power for as long as it's down, same as it holds a
// book page.
//
// By the time this is called, EPD_Sleep() has already put the panel itself
// into its own low-power hold state (see maybeSleep()) -- forcing
// epdNeedsFullInit here makes beginFrame() run a real EPD_Init(), which is
// what actually wakes it back up to accept new image data, rather than
// attempting a partial update against a now-stale reference frame. EPD_Sleep()
// is called again afterward, once this frame is drawn, to put it right back
// into that same low-power state for the actual duration of the sleep --
// otherwise it'd sit at full power the whole time the ESP32 itself is in
// deep sleep, which defeats a good chunk of the point.
constexpr uint8_t DEEP_SLEEP_TITLE_FONT = 32;
void renderDeepSleepSplash() {
  epdNeedsFullInit = true;
  beginFrame();

  String title = "TinyEReader";
  uint16_t titleWidth = title.length() * (DEEP_SLEEP_TITLE_FONT / 2);
  uint16_t titleX = (EPD_W - titleWidth) / 2;
  EPD_ShowString(titleX, 30, title.c_str(), BLACK, DEEP_SLEEP_TITLE_FONT);

  String subtitle = "Deep sleep - press any button";
  uint16_t subtitleWidth = subtitle.length() * (MENU_FONT / 2);
  uint16_t subtitleX = (EPD_W - subtitleWidth) / 2;
  EPD_ShowString(subtitleX, 76, subtitle.c_str(), BLACK, MENU_FONT);

  endFrame();

  EPD_Sleep();
  epdNeedsFullInit = true;
}

// ---------------- LIBRARY ----------------
void saveCurrentBookName() {
  File f = LittleFS.open("/current.txt", "w");
  if (!f) return;
  f.print(currentBookName);
  f.close();
}

String loadCurrentBookName() {
  File f = LittleFS.open("/current.txt", "r");
  if (!f) return "";
  String name = f.readString();
  f.close();
  name.trim();
  return name;
}

void savePosition() {
  if (currentBookName.length() == 0) return;
  File pos = LittleFS.open(posPath(currentBookName), "w");
  if (!pos) return;
  pos.print(pageStart);
  pos.close();
}

uint32_t loadSavedPosition(const String& name) {
  File pos = LittleFS.open(posPath(name), "r");
  if (!pos) return 0;
  uint32_t saved = pos.parseInt();
  pos.close();
  return saved;
}

void reopenBookAt(uint32_t offset) {
  if (book) book.close();
  if (currentBookName.length() == 0) {
    fileSize = 0;
    pageStart = 0;
    return;
  }
  book = LittleFS.open(bookPath(currentBookName), "r");
  if (!book) return;
  fileSize = book.size();
  if (offset > fileSize) offset = 0;
  pageStart = offset;
  book.seek(pageStart);
}

// Simple insertion sort -- bookCount is capped at MAX_BOOKS (64), so even
// the O(n^2) worst case is negligible, and SORT_SIZE re-opening each file
// just to read its size is cheap at that count too. Applies to bookList in
// place; called from listBooks() so every screen that lists books (Choose
// Book, Bookmarks, and the web upload page's library list) sees the same
// order without each needing its own sort call.
void sortBookList() {
  size_t sizes[MAX_BOOKS];
  if (sortMode == SORT_SIZE) {
    for (uint8_t i = 0; i < bookCount; i++) {
      File f = LittleFS.open(bookPath(bookList[i]), "r");
      sizes[i] = f ? f.size() : 0;
      if (f) f.close();
    }
  }

  for (uint8_t i = 1; i < bookCount; i++) {
    String name = bookList[i];
    size_t size = (sortMode == SORT_SIZE) ? sizes[i] : 0;
    int16_t j = (int16_t)i - 1;
    while (j >= 0) {
      bool shouldMoveRight;
      if (sortMode == SORT_ZA) shouldMoveRight = bookList[j] < name;
      else if (sortMode == SORT_SIZE) shouldMoveRight = sizes[j] < size;  // largest first
      else shouldMoveRight = bookList[j] > name;                          // SORT_AZ
      if (!shouldMoveRight) break;
      bookList[j + 1] = bookList[j];
      if (sortMode == SORT_SIZE) sizes[j + 1] = sizes[j];
      j--;
    }
    bookList[j + 1] = name;
    if (sortMode == SORT_SIZE) sizes[j + 1] = size;
  }
}

void listBooks() {
  bookCount = 0;
  bookFileTotal = 0;
  File dir = LittleFS.open("/books");
  if (!dir || !dir.isDirectory()) return;

  // Keeps counting past MAX_BOOKS (into bookFileTotal) even once bookList is
  // full, so leftover books from before upload enforced the MAX_BOOKS cap --
  // invisible here and in the web library list, but still eating flash
  // space -- can be surfaced instead of silently vanishing.
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());
    int slash = name.lastIndexOf('/');
    if (slash != -1) name = name.substring(slash + 1);
    if (name.length() > 0 && !name.endsWith(".pos") && !name.endsWith(".bm")) {
      if (bookCount < MAX_BOOKS) bookList[bookCount++] = name;
      bookFileTotal++;
    }
    f = dir.openNextFile();
  }

  sortBookList();
}

void renderChooseBook();
void scrollChooseWindow();

// Shared by the on-device delete flow and the web /delete endpoint. Returns
// false if the book didn't exist (nothing to delete).
bool deleteBookFile(const String& name) {
  if (!LittleFS.exists(bookPath(name))) return false;

  if (name == currentBookName) {
    if (book) book.close();
    currentBookName = "";
    LittleFS.remove("/current.txt");
  }

  LittleFS.remove(bookPath(name));
  LittleFS.remove(posPath(name));
  LittleFS.remove(bookmarkPath(name));
  return true;
}

void deleteSelectedBook() {
  if (bookCount == 0) return;
  deleteBookFile(bookList[chooseSelection]);

  listBooks();
  if (bookCount == 0) {
    chooseSelection = 0;
  } else if (chooseSelection >= bookCount) {
    chooseSelection = bookCount - 1;
  }
  scrollChooseWindow();
  renderChooseBook();
}

void renderPageAt(uint32_t offset);
void renderPageAtCore(uint32_t offset);
uint32_t computeTotalPageCount(bool trackChapterPages = false);
uint32_t pageNumberForOffset(uint32_t offset);
void refreshPageTracking(uint32_t offset);

// Scans the whole book once for form-feed chapter markers and records their
// byte offsets. Called once per book-open, not per page -- a full linear
// scan on every page turn would be needlessly slow.
void indexChapters() {
  chapterCount = 0;
  chapterOffsets[chapterCount++] = 0;  // implicit chapter 0 at the very start

  if (!book) return;

  book.seek(0);
  uint32_t offset = 0;
  while (book.available() && chapterCount < MAX_CHAPTERS) {
    int c = book.read();
    offset++;
    if (c == '\f') {
      chapterOffsets[chapterCount++] = offset;
    }
  }
  // No need to restore the read position here -- every caller of
  // indexChapters() immediately follows it with renderPageAt(), which
  // re-seeks the book itself.
}

void nextChapter() {
  for (uint16_t i = 0; i < chapterCount; i++) {
    if (chapterOffsets[i] > pageStart) {
      // A chapter skip is a jump, not a sequential step, so currentPageNumber
      // can't reuse nextPage()'s cheap +1 -- but unlike a genuinely new
      // offset, a chapter start's page number is already known for free
      // from chapterPageNumbers[] (see computeTotalPageCount()), so this
      // doesn't need pageNumberForOffset()'s own O(book length) sweep.
      if (progressMode == PROGRESS_FRACTION) currentPageNumber = chapterPageNumbers[i];
      renderPageAt(chapterOffsets[i]);
      return;
    }
  }
}

void previousChapter() {
  for (int16_t i = (int16_t)chapterCount - 1; i >= 0; i--) {
    if (chapterOffsets[i] < pageStart) {
      if (progressMode == PROGRESS_FRACTION) currentPageNumber = chapterPageNumbers[i];
      renderPageAt(chapterOffsets[i]);
      return;
    }
  }
  if (chapterCount > 0) {
    if (progressMode == PROGRESS_FRACTION) currentPageNumber = chapterPageNumbers[0];
    renderPageAt(chapterOffsets[0]);
  }
}

void openBook(const String& name) {
  // indexChapters() does a byte-by-byte scan of the whole file, which can take
  // a couple of seconds on a large book -- show a message first so this reads
  // as "working" rather than a frozen screen. Title gets its own line (rather
  // than sharing one with "Opening") so long titles have more room before
  // truncating.
  String title = truncateForRow(bookTitle(name));
  showMessage("Opening\n" + title);

  currentBookName = name;
  saveCurrentBookName();
  uint32_t saved = loadSavedPosition(name);
  reopenBookAt(saved);
  indexChapters();
  refreshPageTracking(saved);
  currentScreen = SCREEN_READING;
  renderPageAt(saved);
}

// Like openBook(), but opens at an explicit bookmark offset instead of the
// saved "current place" -- and deliberately uses renderPageAtCore() instead
// of renderPageAt() so jumping to a bookmark does NOT overwrite "current
// place". If you keep reading forward/back from here, normal page turns
// call renderPageAt() as usual and current place starts tracking again.
// slot is 0-based; shown to the user as 1-based.
void openBookAtBookmark(const String& name, uint32_t offset, uint8_t slot) {
  String title = truncateForRow(bookTitle(name));
  showMessage("Opening bookmark " + String(slot + 1) + "\n" + title);

  currentBookName = name;
  saveCurrentBookName();
  reopenBookAt(offset);
  indexChapters();
  refreshPageTracking(offset);
  currentScreen = SCREEN_READING;
  renderPageAtCore(offset);
}

// Saves/overwrites a bookmark at the current reading position: fills the
// first empty slot, or overwrites slot 1 if all three are already used.
// Triggered by holding Top for BOOKMARK_HOLD_MS while reading (see
// pollButtons()). Flashes a confirmation message, then redraws the page
// that was already on screen -- a blocking pause here is consistent with
// how showMessage() is already used elsewhere (e.g. "Opening <title>").
void saveBookmark() {
  if (currentBookName.length() == 0) return;

  uint32_t offsets[BOOKMARK_SLOT_COUNT];
  loadBookmarks(currentBookName, offsets);

  uint8_t slot = 0;  // falls back to overwriting slot 1 if none are empty
  for (uint8_t i = 0; i < BOOKMARK_SLOT_COUNT; i++) {
    if (offsets[i] == BOOKMARK_EMPTY) {
      slot = i;
      break;
    }
  }
  offsets[slot] = pageStart;
  saveBookmarks(currentBookName, offsets);

  showMessage("Bookmark " + String(slot + 1) + " saved");
  delay(900);
  renderPageAtCore(pageStart);
}

// ---------------- WIFI + WEB ----------------
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME, AP_PASS);
}

void handleRoot() {
  touchActivity();
  server.send(200, "text/html", uploadPage);
}

void handleEpubToTxtScript() {
  server.send(200, "application/javascript", epubToTxtScript);
}

// Hand-built JSON (no ArduinoJson dependency) so the upload page can show
// the current library without a page reload. bookCount is capped at
// MAX_BOOKS by listBooks(), so this stays small and fast even though it
// opens each file just to read its size.
void handleBooksList() {
  touchActivity();
  listBooks();

  String json = "{\"books\":[";
  for (uint8_t i = 0; i < bookCount; i++) {
    if (i > 0) json += ",";
    File f = LittleFS.open(bookPath(bookList[i]), "r");
    size_t size = f ? f.size() : 0;
    if (f) f.close();
    json += "{\"name\":\"" + bookList[i] + "\",\"size\":" + String(size) + "}";
  }
  // freeBytes/maxSize let the upload page simulate handleUpload()'s own
  // acceptance math (min(MAX_BOOK_SIZE, freeSpace - 8192)) client-side, so
  // it can reject queued books that won't fit before even starting the
  // upload instead of finding out partway through a batch.
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  json += "],\"count\":" + String(bookCount) +
          ",\"hidden\":" + String(bookFileTotal > bookCount ? bookFileTotal - bookCount : 0) +
          ",\"free\":\"" + freeSpaceLabel() + "\"" +
          ",\"freeBytes\":" + String(freeBytes) +
          ",\"maxSize\":" + String(MAX_BOOK_SIZE) + "}";

  server.send(200, "application/json", json);
}

String sanitizeFilename(String name) {
  name.trim();
  String out;
  for (size_t i = 0; i < name.length() && out.length() < 40; i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ' ';
    if (ok) out += c;
  }
  if (out.length() == 0) out = "book";
  if (!out.endsWith(".txt")) out += ".txt";
  return out;
}

bool tooLarge = false;
String uploadName;
size_t uploadSizeLimit = 0;

void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File incoming;
  touchActivity();  // keep a long upload from being interrupted by sleep

  if (upload.status == UPLOAD_FILE_START) {
    tooLarge = false;
    uploadName = sanitizeFilename(upload.filename);

    if (!LittleFS.exists("/books")) LittleFS.mkdir("/books");

    size_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
    uploadSizeLimit = min((size_t)MAX_BOOK_SIZE, freeSpace > 8192 ? freeSpace - 8192 : 0);
    if (uploadSizeLimit == 0) {
      tooLarge = true;
      return;
    }

    if (LittleFS.exists(posPath(uploadName))) LittleFS.remove(posPath(uploadName));
    if (LittleFS.exists(bookmarkPath(uploadName))) LittleFS.remove(bookmarkPath(uploadName));
    incoming = LittleFS.open(bookPath(uploadName), "w");
  }

  if (upload.status == UPLOAD_FILE_WRITE && incoming && !tooLarge) {
    if (incoming.size() + upload.currentSize > uploadSizeLimit) {
      tooLarge = true;
      incoming.close();
      LittleFS.remove(bookPath(uploadName));
      return;
    }
    incoming.write(upload.buf, upload.currentSize);
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (incoming) incoming.close();
  }
}

// The WebServer library only finalizes the HTTP response once this request
// handler runs (after handleUpload has fully drained the multipart body).
// Calling server.send() from inside handleUpload instead sends the response
// mid-parse, which makes the library emit a second, conflicting set of
// headers (ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_LENGTH in Chrome).
void handleUploadComplete() {
  if (tooLarge) {
    server.send(413, "text/plain", "File too large (limit 3MB per book, or not enough free space)");
    return;
  }

  // Deliberately doesn't open the book -- uploading just adds it to the
  // library so the page stays put for uploading more. Open it from the
  // device's Choose Book screen.
  server.send(200, "text/plain", "Upload complete: " + uploadName);
}

void handleDeleteBook() {
  touchActivity();
  // Reuses the same sanitizer as uploads so this can't be pointed outside
  // /books via the name query param.
  String name = sanitizeFilename(server.arg("name"));
  if (!deleteBookFile(name)) {
    server.send(404, "text/plain", "Book not found: " + name);
    return;
  }
  server.send(200, "text/plain", "Deleted: " + name);
}

// Routes are registered once, at boot, regardless of Wi-Fi state --
// registering them doesn't require Wi-Fi to be up. setupWebServer() itself
// gets called every time the Wi-Fi screen is entered (and after waking from
// sleep while on it), so it only starts/stops listening; re-registering the
// same routes on every visit would leak a RequestHandler node each time.
void registerWebRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/epub-to-txt.js", HTTP_GET, handleEpubToTxtScript);
  server.on("/books", HTTP_GET, handleBooksList);
  server.on("/delete", HTTP_POST, handleDeleteBook);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUpload);
}

void setupWebServer() {
  server.begin();
}

// ---------------- READER ----------------
// The font only has glyphs for printable ASCII, and EPD_ShowString stops
// rendering a line dead at the first byte outside that range (see EPD.cpp:
// `(*s <= '~') && (*s >= ' ')` -- char is signed here, so bytes >= 0x80 read
// as negative and fail that check immediately). Real book text is full of
// multi-byte UTF-8 "smart" punctuation once entities are decoded -- curly
// quotes, em/en dashes, ellipses -- which without this would silently kill
// the rest of whatever line they're on, showing as missing words followed
// by a blank gap to the end of that line. This decodes the lead byte's
// UTF-8 sequence length, consumes the continuation bytes, and maps the
// handful of typographic characters that show up constantly in real prose
// to plain ASCII lookalikes. Anything without a mapping is dropped rather
// than left to corrupt rendering -- there's no glyph for it regardless.
String utf8ToAsciiReplacement(uint8_t leadByte) {
  uint8_t extraBytes;
  uint32_t codepoint;
  if ((leadByte & 0xE0) == 0xC0) {
    extraBytes = 1;
    codepoint = leadByte & 0x1F;
  } else if ((leadByte & 0xF0) == 0xE0) {
    extraBytes = 2;
    codepoint = leadByte & 0x0F;
  } else if ((leadByte & 0xF8) == 0xF0) {
    extraBytes = 3;
    codepoint = leadByte & 0x07;
  } else {
    return "";  // not a valid UTF-8 lead byte (or a stray continuation byte)
  }

  for (uint8_t i = 0; i < extraBytes && book.available(); i++) {
    uint8_t cont = (uint8_t)book.peek();
    if ((cont & 0xC0) != 0x80) break;  // malformed sequence, bail out early
    book.read();
    codepoint = (codepoint << 6) | (cont & 0x3F);
  }

  switch (codepoint) {
    case 0x2018: case 0x2019: return "'";    // curly single quotes / apostrophe
    case 0x201C: case 0x201D: return "\"";   // curly double quotes
    case 0x2013: return "-";                 // en dash
    case 0x2014: return "--";                // em dash
    case 0x2026: return "...";               // ellipsis
    case 0x00A0: return " ";                 // non-breaking space
    default: return "";
  }
}

// Greedy word-wrap: build the line word by word and stop before a word that
// would overflow bookCharsPerLine, instead of cutting mid-word. If a word
// won't fit, seek back to its start so it becomes the start of the next line.
//
// Newline handling: a lone '\n' is treated as just another word separator,
// not a forced line break. Plenty of real-world plain .txt ebooks (Project
// Gutenberg-style) hard-wrap every source line at a fixed width like 70-80
// characters with a single '\n' between them, unrelated to paragraph
// structure -- treating every '\n' as a real break ends a rendered line at
// each of those arbitrary source boundaries, leaving the rest of this
// screen's much narrower line blank. Two or more consecutive newlines (a
// blank line in the source, which is how tools/epub_to_txt.py separates
// paragraphs) are what actually mark a paragraph break: the whole blank-line
// run is consumed and the current line ends there, without inserting an
// extra fully-blank rendered line for it (wastes a line out of the 6-line
// page budget on a screen this small for no visible benefit).
//
// '\r' is ignored wherever it appears -- just part of a \r\n pair or a
// stray leftover either way. '\f' is a chapter marker inserted by
// tools/epub_to_txt.py and is always a hard break, same as before -- and
// also signals the *page* it's on should end there too (see
// hitChapterBreak/computePageEnd): otherwise a chapter's last page keeps
// pulling in lines from the next chapter to fill out a full page, silently
// mixing the two chapters together with no visual break, and throwing off
// exactly how far back that page's start is by however many extra lines
// of the next chapter it absorbed.
String readWrappedLine(bool* hitChapterBreak = nullptr) {
  String line;
  if (hitChapterBreak) *hitChapterBreak = false;

  while (true) {
    while (book.available() && (book.peek() == ' ' || book.peek() == '\r')) book.read();

    if (!book.available()) break;

    if (book.peek() == '\f') {
      book.read();
      if (hitChapterBreak) *hitChapterBreak = true;
      break;
    }

    if (book.peek() == '\n') {
      book.read();
      while (book.available() && book.peek() == '\r') book.read();
      if (book.available() && book.peek() == '\n') {
        while (book.available() && (book.peek() == '\n' || book.peek() == '\r')) book.read();
        break;
      }
      continue;  // lone newline: treat like a space, keep filling this line
    }

    uint32_t wordStart = book.position();
    String word;
    while (book.available()) {
      uint8_t c = (uint8_t)book.peek();
      if (c == ' ' || c == '\n' || c == '\r' || c == '\f') break;
      book.read();

      if (c >= ' ' && c <= '~') {
        word += (char)c;
      } else if (c >= 0x80) {
        word += utf8ToAsciiReplacement(c);
      }
      // Anything else (a stray control byte below 0x20 that isn't one of
      // our delimiters) is dropped silently.
    }

    if (word.length() > bookCharsPerLine) {
      // Single word longer than a whole line (long URL, etc): hard-break it
      // rather than looping forever. Flush whatever we already have first so
      // the long word starts cleanly on its own line.
      if (line.length() > 0) {
        book.seek(wordStart);
        break;
      }
      book.seek(wordStart + bookCharsPerLine);
      return word.substring(0, bookCharsPerLine);
    }

    uint16_t candidateLen = line.length() + (line.length() > 0 ? 1 : 0) + word.length();
    if (candidateLen > bookCharsPerLine) {
      book.seek(wordStart);
      break;
    }

    if (line.length() > 0) line += ' ';
    line += word;
  }

  return line;
}

// Wraps text starting at `offset` (assumes `book` is already open on the
// current book) into up to bookMaxLines lines and returns where the
// resulting page ends. Shared by the real render path and by
// findPreviousPageStart()'s forward-replay simulation below -- both need to
// agree exactly on where pages break, or "page up" could land somewhere
// that doesn't match what was actually shown.
uint32_t computePageEnd(uint32_t offset, String* outLines, uint8_t* outLineCount) {
  if (!book) return offset;
  if (offset > fileSize) offset = 0;
  book.seek(offset);

  uint8_t lineCount = 0;
  while (book.available() && lineCount < bookMaxLines) {
    bool hitChapterBreak = false;
    String line = readWrappedLine(&hitChapterBreak);
    if (outLines) outLines[lineCount] = line;
    if (line.length() > 0 || book.available()) lineCount++;
    if (hitChapterBreak) break;  // start the next chapter on a fresh page
  }

  if (outLineCount) *outLineCount = lineCount;
  return book.position();
}

// Finds the byte offset where the page immediately before `currentStart`
// begins. Page breaks depend on word-wrapping, so there's no way to compute
// "one page back" from a byte offset alone -- this replays forward
// pagination from the start of the chapter containing currentStart (or the
// previous chapter, if currentStart is itself a chapter's first page) up to
// currentStart, and returns the start of the last simulated page before it.
// The replay is cached (see cachedPageStarts) so repeated or held "page up"
// presses within the same chapter don't redo it from scratch every time.
uint32_t findPreviousPageStart(uint32_t currentStart) {
  if (currentStart == 0) return 0;

  int16_t chapterIndex = 0;
  for (uint16_t i = 0; i < chapterCount; i++) {
    if (chapterOffsets[i] <= currentStart) chapterIndex = i;
    else break;
  }

  uint32_t replayFrom = chapterOffsets[chapterIndex];
  uint32_t replayTo = currentStart;
  if (replayFrom == currentStart) {
    if (chapterIndex == 0) return 0;  // already at the first page of the first chapter
    replayFrom = chapterOffsets[chapterIndex - 1];
    replayTo = chapterOffsets[chapterIndex];
  }

  bool cacheCoversTarget = cachedChapterStart == replayFrom && cachedPageCount > 0 &&
                            cachedPageStarts[cachedPageCount - 1] >= replayTo;
  if (!cacheCoversTarget) {
    cachedChapterStart = replayFrom;
    cachedPageCount = 0;
    cachedPageStarts[cachedPageCount++] = replayFrom;

    uint32_t simulated = replayFrom;
    while (simulated < replayTo && cachedPageCount < MAX_CACHED_PAGES) {
      uint32_t nextSimulated = computePageEnd(simulated, nullptr, nullptr);
      if (nextSimulated <= simulated) break;  // malformed input safety net
      simulated = nextSimulated;
      cachedPageStarts[cachedPageCount++] = simulated;
    }
  }

  uint32_t previousStart = replayFrom;
  for (uint16_t i = 0; i < cachedPageCount; i++) {
    if (cachedPageStarts[i] >= replayTo) break;
    previousStart = cachedPageStarts[i];
  }
  return previousStart;
}

// Full sweep -- how many pages does the currently-open book (whatever
// `book`/fileSize point to) paginate into? Expensive on a large book (same
// order of cost as indexChapters(), but per-line word-wrap work instead of
// a byte scan) -- only ever called while PROGRESS_FRACTION is actually
// selected, not unconditionally on every book-open.
//
// trackChapterPages: also records, in the same pass, which page number each
// entry in chapterOffsets[] lands on (chapterPageNumbers[]) -- letting
// nextChapter()/previousChapter() look those up for free afterward instead
// of each running their own separate O(book length) sweep. ONLY pass true
// when `book`/fileSize definitely point at the same book chapterOffsets[]
// was indexed from (i.e. from refreshPageTracking(), which always reopens
// currentBookName first) -- enterBookmarkSlots() also calls this function,
// but temporarily repoints book/fileSize at whichever book is being
// browsed for bookmarks, which frequently isn't the current reading book,
// so it must NOT opt into this (leaves the default, false).
uint32_t computeTotalPageCount(bool trackChapterPages) {
  if (!book || fileSize == 0) return 1;
  uint32_t count = 0;
  uint32_t pos = 0;
  uint16_t chapterIdx = 0;
  while (true) {
    count++;
    if (trackChapterPages) {
      while (chapterIdx < chapterCount && chapterOffsets[chapterIdx] <= pos) {
        chapterPageNumbers[chapterIdx] = count;
        chapterIdx++;
      }
    }
    if (pos >= fileSize) break;
    uint32_t next = computePageEnd(pos, nullptr, nullptr);
    if (next <= pos) break;  // malformed-input safety net
    pos = next;
  }
  if (trackChapterPages) {
    while (chapterIdx < chapterCount) {
      chapterPageNumbers[chapterIdx] = count;  // trailing chapter(s) starting exactly at EOF
      chapterIdx++;
    }
  }
  return count;
}

// Partial sweep -- which 1-based page number does `offset` fall on, in the
// currently-open book? Cheaper than computeTotalPageCount() since it can
// stop as soon as it passes `offset` instead of covering the whole book,
// but still O(offset) -- used for jumps (chapter skip, book/bookmark open),
// not every page turn (see currentPageNumber's own comment).
uint32_t pageNumberForOffset(uint32_t offset) {
  if (!book) return 1;
  uint32_t count = 0;
  uint32_t pos = 0;
  while (true) {
    count++;
    if (pos >= offset) break;
    uint32_t next = computePageEnd(pos, nullptr, nullptr);
    if (next <= pos) break;
    pos = next;
  }
  return count;
}

// Refreshes totalPageCount/currentPageNumber for currentBookName at
// `offset` -- a full sweep, so this only actually does anything while
// PROGRESS_FRACTION is selected (see those globals' own comment). Called
// whenever a book is (re)opened (openBook(), openBookAtBookmark(), the
// initial boot in setup()) and when switching Settings -- Progress to
// Fraction mode.
//
// Always reopens currentBookName itself first, rather than trusting `book`
// to already be pointing at it -- the Bookmarks screen temporarily repoints
// the shared `book`/fileSize at whatever book it's browsing (see
// enterBookmarkSlots()), so without this, cycling into Fraction mode via
// Settings right after browsing a different book's bookmarks would sweep
// the wrong file's content against the real active book's `pageStart`. The
// extra reopen is cheap (a file open + seek) next to the sweep itself, so
// this costs nothing extra when called right after openBook()'s own
// reopenBookAt() -- just redundant, not wrong.
void refreshPageTracking(uint32_t offset) {
  if (progressMode != PROGRESS_FRACTION) return;
  if (currentBookName.length() == 0) {
    totalPageCount = 1;
    currentPageNumber = 1;
    return;
  }
  reopenBookAt(offset);
  totalPageCount = computeTotalPageCount(/* trackChapterPages= */ true);
  currentPageNumber = pageNumberForOffset(offset);
}

// Text for the reading-screen progress indicator, per progressMode -- ""
// for PROGRESS_OFF and PROGRESS_BAR (the bar is drawn separately, see
// drawProgressBar() and its call site in renderPageAtCore(), and is
// mutually exclusive with the corner text). Fraction mode reads
// currentPageNumber/totalPageCount rather than recomputing them here --
// see those globals' own comment for who's responsible for keeping them
// in sync with `offset`.
String progressIndicatorText(uint32_t offset) {
  if (progressMode == PROGRESS_OFF || progressMode == PROGRESS_BAR) return "";
  if (progressMode == PROGRESS_FRACTION) {
    return String(currentPageNumber) + "/" + String(totalPageCount);
  }
  if (fileSize == 0) return "";
  return String((uint8_t)(((uint64_t)offset * 100) / fileSize)) + "%";
}

// 1-2px progress bar hugging the very bottom edge of the screen, filled
// proportionally to how far `offset` is through the file -- independent of
// how many lines this particular page has, so it stays in a fixed spot.
// bookMaxLines already uses the screen down to its last pixel of slack (see
// applyBookFont()), but the glyphs themselves (bookFont px tall, against a
// taller bookLineHeight) always end a few px above the bottom edge --
// plenty of room for this without eating a line, at any offered font size.
// Only drawn for progressMode == PROGRESS_BAR (see call site in
// renderPageAtCore()) -- mutually exclusive with the corner text modes, not
// layered with them.
constexpr uint16_t PROGRESS_BAR_THICKNESS = 2;
void drawProgressBar(uint32_t offset, size_t size) {
  if (size == 0) return;
  uint16_t barLeft = BOOK_LEFT_MARGIN;
  uint16_t barWidth = EPD_W - 2 * BOOK_LEFT_MARGIN;
  uint16_t filled = (uint16_t)(((uint64_t)offset * barWidth) / size);
  uint16_t y = EPD_H - PROGRESS_BAR_THICKNESS;
  for (uint16_t t = 0; t < PROGRESS_BAR_THICKNESS; t++) {
    EPD_DrawLine(barLeft, y + t, barLeft + filled, y + t, BLACK);
  }
}

// Split out from renderPageAt() so opening a book at a bookmark can render
// without touching "current place" (see openBookAtBookmark()) -- everything
// except the savePosition() call at the end.
void renderPageAtCore(uint32_t offset) {
  reopenBookAt(offset);
  if (!book) {
    showMessage("Book open failed");
    return;
  }

  String lines[BOOK_MAX_LINES_CAP];
  uint8_t lineCount = 0;
  pageEnd = computePageEnd(offset, lines, &lineCount);

  // Progress indicator: actually make room for it on the last line rather
  // than drawing on top of whatever's already there (which used to block
  // real words) -- trim whole words off the end of that line until there's
  // space, or leave it untouched if it was already short enough not to need
  // any.
  String indicator = (lineCount > 0) ? progressIndicatorText(offset) : "";
  if (indicator.length() > 0) {
    uint16_t INDICATOR_GAP = bookCharWidth;
    uint16_t indicatorWidth = indicator.length() * bookCharWidth;
    uint16_t usableWidth = EPD_W - 2 * BOOK_LEFT_MARGIN;
    String& lastLine = lines[lineCount - 1];
    while (lastLine.length() * bookCharWidth + INDICATOR_GAP + indicatorWidth > usableWidth) {
      int lastSpace = lastLine.lastIndexOf(' ');
      if (lastSpace < 0) {
        lastLine = "";
        break;
      }
      lastLine = lastLine.substring(0, lastSpace);
    }
  }

  beginFrame();

  for (uint8_t i = 0; i < lineCount; i++) {
    EPD_ShowString(BOOK_LEFT_MARGIN, BOOK_TOP_MARGIN + (i * bookLineHeight), lines[i].c_str(), BLACK, bookFont);
  }

  if (indicator.length() > 0) {
    uint16_t indicatorX = EPD_W - BOOK_LEFT_MARGIN - (uint16_t)(indicator.length() * bookCharWidth);
    uint16_t indicatorY = BOOK_TOP_MARGIN + (lineCount - 1) * bookLineHeight;
    EPD_ShowString(indicatorX, indicatorY, indicator.c_str(), BLACK, bookFont);
  }
  if (progressMode == PROGRESS_BAR) drawProgressBar(offset, fileSize);

  endFrame();

  touchActivity();
  lastPageTurnTime = millis();  // see maybeAutoTurn()
}

void renderPageAt(uint32_t offset) {
  renderPageAtCore(offset);
  savePosition();
}

void nextPage() {
  if (!book || pageEnd >= fileSize) return;
  // Sequential move, so currentPageNumber just steps by one instead of a
  // fresh pageNumberForOffset() sweep -- see its own comment for why that
  // matters.
  if (progressMode == PROGRESS_FRACTION) currentPageNumber++;
  renderPageAt(pageEnd);
}

void previousPage() {
  if (!book || pageStart == 0) return;
  if (progressMode == PROGRESS_FRACTION) currentPageNumber--;
  renderPageAt(findPreviousPageStart(pageStart));
}

// ---------------- MENUS ----------------
const uint8_t* HOME_ICONS[HOME_ITEM_COUNT] = { iconBook, iconBookshelf, iconBookmark, iconWifi, iconSettings };
const char* HOME_SHORT_LABELS[HOME_ITEM_COUNT] = { "Read", "Books", "Marks", "Wi-Fi", "Setup" };

// Icon-first layout: a row of three 48x48 icons with a small label under
// each, centered as a block in the 250x122 screen, selection shown as a
// border box rather than a "> " prefix (there's no list to prefix here).
// 5 items don't fit in one row at this icon size, so they're split into two
// pages of up to HOME_ICONS_PER_PAGE icons each -- dialing past the last
// item of page 0 moves into page 1 and vice versa (see the modular
// arithmetic in SCREEN_HOME's dial handling in pollButtons(), which is
// already generic over HOME_ITEM_COUNT and needed no changes for this).
constexpr uint16_t HOME_ICON_SIZE = 48;
constexpr uint16_t HOME_ICON_Y = 30;
constexpr uint8_t HOME_LABEL_FONT = 12;  // 6x12 -- deliberately small/secondary to the icon
constexpr uint16_t HOME_LABEL_Y = HOME_ICON_Y + HOME_ICON_SIZE + 4;
constexpr uint8_t HOME_ICONS_PER_PAGE = 3;
const uint16_t HOME_PAGE0_X[HOME_ICONS_PER_PAGE] = { 26, 100, 174 };  // evenly spaced, symmetric margins
const uint16_t HOME_PAGE1_X[HOME_ITEM_COUNT - HOME_ICONS_PER_PAGE] = { 64, 138 };  // centered pair, same spacing rhythm as page 0

void renderHome() {
  beginFrame();
  bool page1 = homeSelection >= HOME_ICONS_PER_PAGE;
  uint8_t startIdx = page1 ? HOME_ICONS_PER_PAGE : 0;
  uint8_t count = page1 ? (HOME_ITEM_COUNT - HOME_ICONS_PER_PAGE) : HOME_ICONS_PER_PAGE;
  const uint16_t* xs = page1 ? HOME_PAGE1_X : HOME_PAGE0_X;

  for (uint8_t slot = 0; slot < count; slot++) {
    uint8_t i = startIdx + slot;
    uint16_t x = xs[slot];
    EPD_ShowPicture(x, HOME_ICON_Y, HOME_ICON_SIZE, HOME_ICON_SIZE, HOME_ICONS[i], BLACK);

    const char* label = HOME_SHORT_LABELS[i];
    uint16_t labelWidth = strlen(label) * (HOME_LABEL_FONT / 2);
    uint16_t labelX = x + (HOME_ICON_SIZE - labelWidth) / 2;
    EPD_ShowString(labelX, HOME_LABEL_Y, label, BLACK, HOME_LABEL_FONT);

    if (i == homeSelection) {
      EPD_DrawRectangle(x - 4, HOME_ICON_Y - 4, x + HOME_ICON_SIZE + 4,
                         HOME_LABEL_Y + HOME_LABEL_FONT + 2, BLACK);
    }
  }
  endFrame();
}

// Shared scrolling-window logic for any screen with a selectable list that
// might be longer than fits on screen (Choose Book, the Bookmarks screen's
// book picker, Settings) -- keeps `selection` inside [windowStart,
// windowStart + visibleRows) by scrolling the window the minimum amount
// needed. Each screen keeps its own thin named wrapper (scrollChooseWindow()
// etc.) below so call sites read as what they're scrolling, and each
// screen's window position stays independent (e.g. Choose Book and the
// Bookmarks book picker can be scrolled to different places at once).
void scrollWindow(uint8_t selection, uint8_t& windowStart, uint8_t itemCount, uint8_t visibleRows) {
  if (selection < windowStart) {
    windowStart = selection;
  } else if (selection >= windowStart + visibleRows) {
    windowStart = selection - visibleRows + 1;
  }
  uint8_t maxStart = (itemCount > visibleRows) ? itemCount - visibleRows : 0;
  if (windowStart > maxStart) windowStart = maxStart;
}

void scrollChooseWindow() {
  scrollWindow(chooseSelection, chooseWindowStart, bookCount, CHOOSE_VISIBLE_ROWS);
}

void scrollBookmarkWindow() {
  scrollWindow(bookmarkBookSelection, bookmarkWindowStart, bookCount, CHOOSE_VISIBLE_ROWS);
}

// Shared rendering for a scrollable, selectable book list -- Choose Book and
// the Bookmarks screen's book picker share this layout exactly (header with
// ^/v scroll hints once the list is longer than fits, "> "/"  " prefix per
// row, truncated names), differing only in the header text and what to show
// when the library is empty (emptyLine2 is optional, pass nullptr to skip
// it).
void renderBookListScreen(const String& header, uint8_t selection, uint8_t windowStart,
                           const char* emptyLine1, const char* emptyLine2) {
  beginFrame();

  String headerWithHints = header;
  if (bookCount > CHOOSE_VISIBLE_ROWS) {
    headerWithHints += "  ";
    headerWithHints += (windowStart > 0) ? "^" : " ";
    headerWithHints += (windowStart + CHOOSE_VISIBLE_ROWS < bookCount) ? "v" : " ";
  }
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, headerWithHints.c_str(), BLACK, MENU_FONT);

  if (bookCount == 0) {
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + MENU_LINE_HEIGHT, emptyLine1, BLACK, MENU_FONT);
    if (emptyLine2) EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 2 * MENU_LINE_HEIGHT, emptyLine2, BLACK, MENU_FONT);
  } else {
    uint8_t windowEnd = min((uint8_t)(windowStart + CHOOSE_VISIBLE_ROWS), bookCount);
    for (uint8_t i = windowStart; i < windowEnd; i++) {
      uint8_t row = i - windowStart;
      String label = (i == selection) ? "> " : "  ";
      label += truncateForRow(bookList[i]);
      EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + (row + 1) * MENU_LINE_HEIGHT, label.c_str(), BLACK, MENU_FONT);
    }
  }
  endFrame();
}

// Row 0 is a free-space header (plus ^/v hints when the list scrolls); the
// book list starts one row down and shows at most CHOOSE_VISIBLE_ROWS at a
// time. Dial press opens the highlighted book; dial down opens a Yes/No
// delete confirmation for it instead (see renderConfirmDelete/enterConfirmDelete).
void renderChooseBook() {
  renderBookListScreen(freeSpaceLabel(), chooseSelection, chooseWindowStart, "No books yet.", "Upload one first.");
}

// Book picker for the Bookmarks screen -- same layout as renderChooseBook()
// but dial press moves into SCREEN_BOOKMARK_SLOTS for the highlighted book
// instead of opening it directly (see enterBookmarkSlots()).
void renderBookmarkBooks() {
  renderBookListScreen("Bookmarks", bookmarkBookSelection, bookmarkWindowStart, "No books yet.", nullptr);
}

void enterBookmarkBooks() {
  listBooks();
  bookmarkBookSelection = 0;
  bookmarkWindowStart = 0;
  currentScreen = SCREEN_BOOKMARK_BOOKS;
  renderBookmarkBooks();
}

// Unlike the reading screen (which shows whichever single format Settings
// -> Progress picked, re-rendering every page turn so the cost of picking
// one has to stay cheap), this is a one-shot render, and a bookmark's whole
// point is deciding at a glance whether it's the one you want -- so it
// always shows both percent and page fraction together, regardless of that
// setting. Both are precomputed in enterBookmarkSlots().
void renderBookmarkSlots() {
  beginFrame();
  String title = truncateForRow(bookTitle(bookList[bookmarkBookSelection]));
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, title.c_str(), BLACK, MENU_FONT);

  for (uint8_t i = 0; i < BOOKMARK_SLOT_COUNT; i++) {
    String label = (i == bookmarkSlotSelection) ? "> " : "  ";
    label += String(i + 1) + ": ";
    if (bookmarkOffsets[i] == BOOKMARK_EMPTY) {
      label += "(empty)";
    } else {
      uint8_t pct = (fileSize > 0) ? (uint8_t)(((uint64_t)bookmarkOffsets[i] * 100) / fileSize) : 0;
      label += String(bookmarkSlotPages[i]) + "/" + String(bookmarkTotalPages) + " (" + String(pct) + "%)";
    }
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + (i + 1) * MENU_LINE_HEIGHT, label.c_str(), BLACK, MENU_FONT);
  }
  endFrame();
}

void enterBookmarkSlots() {
  loadBookmarks(bookList[bookmarkBookSelection], bookmarkOffsets);
  bookmarkSlotSelection = 0;

  // Always computed now (used to be gated behind PROGRESS_FRACTION, back
  // when this screen only showed one format) -- see renderBookmarkSlots().
  // A real pagination sweep, same cost class as indexChapters() opening a
  // book, so this can take a couple of seconds on a large book; the
  // "Loading..." message is here for the same reason openBook() shows one.
  //
  // Temporarily points the shared `book`/fileSize at this bookmark's own
  // book to reuse the same pagination sweep the reading screen uses --
  // safe outside SCREEN_READING, since reopenBookAt() always freshly
  // reopens the correct file before anything reads `book`/fileSize again
  // (see its call at the top of renderPageAtCore()), regardless of what
  // they were left pointing to here. renderBookmarkSlots() also reads this
  // same fileSize for the percent figure, rather than reopening the file
  // again just to ask its size a second time.
  if (book) book.close();
  book = LittleFS.open(bookPath(bookList[bookmarkBookSelection]), "r");
  fileSize = book ? book.size() : 0;

  showMessage("Loading...");
  bookmarkTotalPages = computeTotalPageCount();
  for (uint8_t i = 0; i < BOOKMARK_SLOT_COUNT; i++) {
    bookmarkSlotPages[i] = (bookmarkOffsets[i] != BOOKMARK_EMPTY) ? pageNumberForOffset(bookmarkOffsets[i]) : 0;
  }

  currentScreen = SCREEN_BOOKMARK_SLOTS;
  renderBookmarkSlots();
}

void deleteSelectedBookmark() {
  bookmarkOffsets[bookmarkSlotSelection] = BOOKMARK_EMPTY;
  saveBookmarks(bookList[bookmarkBookSelection], bookmarkOffsets);
  renderBookmarkSlots();
}

// Shared by book deletion, bookmark deletion, and factory reset -- see
// ConfirmAction. Top/Bottom and dial up/down all move the Yes/No highlight
// (same up/down convention as every other screen); dial press confirms
// whichever is highlighted. Defaults to No so nothing destructive happens
// unless the user deliberately moves to Yes and confirms.
void renderConfirmDelete() {
  beginFrame();

  String prompt, detail;
  switch (confirmAction) {
    case CONFIRM_DELETE_BOOK:
      prompt = "Delete this book?";
      detail = (chooseSelection < bookCount) ? bookList[chooseSelection] : "";
      break;
    case CONFIRM_DELETE_BOOKMARK:
      prompt = "Delete this bookmark?";
      detail = bookTitle(bookList[bookmarkBookSelection]) + " #" + String(bookmarkSlotSelection + 1);
      break;
    case CONFIRM_FACTORY_RESET:
      prompt = "Factory reset?";
      detail = "Erases ALL books & settings";
      break;
  }
  detail = truncateForRow(detail);

  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, prompt.c_str(), BLACK, MENU_FONT);
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + MENU_LINE_HEIGHT, detail.c_str(), BLACK, MENU_FONT);

  String yesLabel = confirmDeleteYes ? "> Yes" : "  Yes";
  String noLabel = !confirmDeleteYes ? "> No" : "  No";
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 3 * MENU_LINE_HEIGHT, yesLabel.c_str(), BLACK, MENU_FONT);
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 4 * MENU_LINE_HEIGHT, noLabel.c_str(), BLACK, MENU_FONT);

  endFrame();
}

void enterConfirm(ConfirmAction action) {
  confirmAction = action;
  confirmDeleteYes = false;
  currentScreen = SCREEN_CONFIRM_DELETE;
  renderConfirmDelete();
}

void renderWifiInfo() {
  beginFrame();

  constexpr uint16_t leftX = 4;
  constexpr uint16_t rightX = leftX + QRWIFI_W + 8;
  constexpr uint16_t qrY = 20;

  EPD_ShowString(leftX, 2, "Wi-Fi", BLACK, MENU_FONT);
  EPD_ShowPicture(leftX, qrY, QRWIFI_W, QRWIFI_H, qrWifi, BLACK);

  EPD_ShowString(rightX, 2, "Web", BLACK, MENU_FONT);
  EPD_ShowPicture(rightX, qrY, QRWEBPAGE_W, QRWEBPAGE_H, qrWebpage, BLACK);

  EPD_ShowString(leftX, qrY + QRWIFI_H + 4, "192.168.4.1", BLACK, MENU_FONT);

  endFrame();
}

// Wi-Fi only runs while this screen is showing, to save battery -- it's off
// the rest of the time, including at boot.
void enterWifiInfo() {
  currentScreen = SCREEN_WIFI_INFO;
  setupAP();
  setupWebServer();
  touchActivity();
  renderWifiInfo();
}

void exitWifiInfo() {
  server.stop();
  WiFi.mode(WIFI_OFF);
}

// Returns a short human label for a SLEEP_PRESETS_MS entry, e.g. "1 min" or
// "Never" for the 0 sentinel.
String sleepPresetLabel(uint32_t ms) {
  if (ms == 0) return "Never";
  if (ms < 60000) return String(ms / 1000) + " sec";
  return String(ms / 60000) + " min";
}

String sortModeLabel(SortMode mode) {
  switch (mode) {
    case SORT_ZA: return "Z-A";
    case SORT_SIZE: return "Size";
    default: return "A-Z";
  }
}

String progressModeLabel(ProgressMode mode) {
  switch (mode) {
    case PROGRESS_FRACTION: return "Fraction";
    case PROGRESS_BAR: return "Bar";
    case PROGRESS_OFF: return "None";
    default: return "Percent";
  }
}

// Named labels for TEXT_SIZE_PRESETS_PX rather than just printing the raw
// pixel height, same reasoning as sortModeLabel()/progressModeLabel().
String textSizeLabel(uint8_t font) {
  switch (font) {
    case 12: return "Small";
    case 24: return "Large";
    case 32: return "X-Large";
    default: return "Medium";
  }
}

// Label for settings row `index` (0..SETTINGS_ITEM_COUNT-1), without the
// "> "/"  " selection prefix -- see renderSettings().
String settingsItemLabel(uint8_t index) {
  switch (index) {
    case 0: return "Sleep: " + sleepPresetLabel(sleepAfterMs);
    case 1: return "Deep sleep: " + ((deepSleepAfterMs == 0) ? String("Off") : sleepPresetLabel(deepSleepAfterMs));
    case 2: return "Auto-turn: " + ((autoTurnMs == 0) ? String("Off") : sleepPresetLabel(autoTurnMs));
    case 3: return "Invert: " + String(invertDisplay ? "On" : "Off");
    case 4: return "Sort: " + sortModeLabel(sortMode);
    case 5: return "Progress: " + progressModeLabel(progressMode);
    case 6: return "Text size: " + textSizeLabel(bookFont);
    case 7: return "Controls";
    default: return "Factory reset";
  }
}

// Keeps settingsSelection inside [settingsWindowStart, settingsWindowStart +
// SETTINGS_VISIBLE_ROWS) -- same logic as scrollChooseWindow().
void scrollSettingsWindow() {
  if (settingsSelection < settingsWindowStart) {
    settingsWindowStart = settingsSelection;
  } else if (settingsSelection >= settingsWindowStart + SETTINGS_VISIBLE_ROWS) {
    settingsWindowStart = settingsSelection - SETTINGS_VISIBLE_ROWS + 1;
  }
  constexpr uint8_t maxStart = SETTINGS_ITEM_COUNT - SETTINGS_VISIBLE_ROWS;
  if (settingsWindowStart > maxStart) settingsWindowStart = maxStart;
}

// 6 rows is the max that fits this screen (MENU_TOP_MARGIN=4, MENU_LINE_HEIGHT=20,
// 122px tall -- the menu screens use a fixed font/line height, unlike the
// reading screen's bookLineHeight, which is why this number doesn't move
// when Text size changes): header + up to SETTINGS_VISIBLE_ROWS items,
// scrolling for the rest (same ^/v hint convention as renderChooseBook())
// now that there are more items than fit
// at once.
void renderSettings() {
  beginFrame();

  String header = "Settings";
  if (SETTINGS_ITEM_COUNT > SETTINGS_VISIBLE_ROWS) {
    header += "  ";
    header += (settingsWindowStart > 0) ? "^" : " ";
    header += (settingsWindowStart + SETTINGS_VISIBLE_ROWS < SETTINGS_ITEM_COUNT) ? "v" : " ";
  }
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, header.c_str(), BLACK, MENU_FONT);

  uint8_t windowEnd = min((uint8_t)(settingsWindowStart + SETTINGS_VISIBLE_ROWS), SETTINGS_ITEM_COUNT);
  for (uint8_t i = settingsWindowStart; i < windowEnd; i++) {
    uint8_t row = i - settingsWindowStart;
    String label = (i == settingsSelection) ? "> " : "  ";
    label += settingsItemLabel(i);
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + (row + 1) * MENU_LINE_HEIGHT, label.c_str(), BLACK, MENU_FONT);
  }

  endFrame();
}

void enterSettings() {
  settingsSelection = 0;
  settingsWindowStart = 0;
  currentScreen = SCREEN_SETTINGS;
  renderSettings();
}

// Shared by cycleSleepTimeout()/cycleAutoTurn() -- finds `current` in
// `presets` and returns the next entry, wrapping. Falls back to presets[0]
// if `current` isn't found (shouldn't happen, since these are only ever set
// from this same array, but that's a safer default than reading past the
// array).
uint32_t nextPreset(const uint32_t presets[], uint8_t count, uint32_t current) {
  for (uint8_t i = 0; i < count; i++) {
    if (presets[i] == current) return presets[(i + 1) % count];
  }
  return presets[0];
}

// Steps sleepAfterMs to the next SLEEP_PRESETS_MS entry (wrapping) and
// persists it -- Preferences/NVS, not LittleFS, since it's device
// configuration rather than book data (see the global `prefs`/setup()).
void cycleSleepTimeout() {
  sleepAfterMs = nextPreset(SLEEP_PRESETS_MS, SLEEP_PRESETS_COUNT, sleepAfterMs);
  prefs.putUInt("sleepMs", sleepAfterMs);
  renderSettings();
}

// Steps deepSleepAfterMs the same way -- see maybeSleep() for how this and
// sleepAfterMs combine (deep sleep triggers at sleepAfterMs + deepSleepAfterMs
// of total inactivity).
void cycleDeepSleepTimeout() {
  deepSleepAfterMs = nextPreset(DEEP_SLEEP_PRESETS_MS, DEEP_SLEEP_PRESETS_COUNT, deepSleepAfterMs);
  prefs.putUInt("deepSleepMs", deepSleepAfterMs);
  renderSettings();
}

// lastPageTurnTime is reset here so turning this on doesn't immediately
// fire using a stale elapsed time -- see maybeAutoTurn().
void cycleAutoTurn() {
  autoTurnMs = nextPreset(AUTO_TURN_PRESETS_MS, AUTO_TURN_PRESETS_COUNT, autoTurnMs);
  prefs.putUInt("autoTurnMs", autoTurnMs);
  lastPageTurnTime = millis();
  renderSettings();
}

// Forces a full re-init on the next frame so the panel does a clean
// white-fill-and-clear cycle instead of partial-updating straight into the
// new polarity, which would otherwise ghost.
void toggleInvertDisplay() {
  invertDisplay = !invertDisplay;
  prefs.putBool("invert", invertDisplay);
  epdNeedsFullInit = true;
  renderSettings();
}

void cycleSortMode() {
  sortMode = (SortMode)((sortMode + 1) % 3);
  prefs.putUChar("sortMode", sortMode);
  renderSettings();
}

void cycleProgressMode() {
  progressMode = (ProgressMode)((progressMode + 1) % 4);
  prefs.putUChar("progressMode", progressMode);
  // Only actually sweeps if the new mode is Fraction (see
  // refreshPageTracking()) -- otherwise switching away from Fraction and
  // back later would show whatever currentPageNumber/totalPageCount were
  // last left at, possibly for a page you've since turned away from. That
  // sweep is a real pagination replay (same cost class as indexChapters()
  // opening a book) -- show a "Loading..." message first when it's about
  // to run, or switching into Fraction on anything but a tiny book looks
  // like the device has frozen.
  if (progressMode == PROGRESS_FRACTION) showMessage("Loading...");
  refreshPageTracking(pageStart);
  renderSettings();
}

// Derives bookCharWidth/bookLineHeight/bookCharsPerLine/bookMaxLines from
// `font` -- every offered font is fixed-width with glyph width = height/2
// (see EPD_ShowChar), so these are pure formulas, not a lookup table; at
// font=16 they reproduce today's original numbers (8/20/30/6) exactly.
// +4px of line spacing above the glyph height, matching the original
// 16-on-20 ratio. -2px of width slack is intentional, same as the original
// 30*8=240-vs-242-usable gap -- flooring division already gets us there.
//
// Also invalidates the "page up" replay cache (cachedChapterStart/
// cachedPageCount): those cache old page *boundaries*, which depend on the
// layout that produced them. Left alone, a font change wouldn't reliably
// bust the cache on its own -- cacheCoversTarget in findPreviousPageStart()
// only compares chapter start offsets, which don't change with font size,
// so a stale cache from the old layout could otherwise get reused and
// "page up" would land somewhere that doesn't match what's now on screen.
void applyBookFont(uint8_t font) {
  bookFont = font;
  bookCharWidth = font / 2;
  bookLineHeight = font + 4;
  bookCharsPerLine = (EPD_W - 2 * BOOK_LEFT_MARGIN) / bookCharWidth;
  bookMaxLines = EPD_H / bookLineHeight;
  cachedChapterStart = 0xFFFFFFFF;
  cachedPageCount = 0;
}

void cycleTextSize() {
  uint32_t next = nextPreset(TEXT_SIZE_PRESETS_PX, TEXT_SIZE_PRESETS_COUNT, bookFont);
  applyBookFont((uint8_t)next);
  prefs.putUChar("bookFont", bookFont);
  // Page boundaries just shifted, so re-sweep for Fraction mode the same
  // way cycleProgressMode() does -- a no-op unless that's the active mode,
  // same "Loading..." reasoning as there too.
  if (progressMode == PROGRESS_FRACTION) showMessage("Loading...");
  refreshPageTracking(pageStart);
  renderSettings();
}

// Wipes the entire LittleFS partition (all books, positions, bookmarks) and
// the Preferences/NVS namespace (settings), then reboots -- there's no
// "undo" screen for this, only the Yes/No confirm in renderConfirmDelete().
void factoryReset() {
  showMessage("Resetting...");
  LittleFS.format();
  prefs.clear();
  delay(300);
  ESP.restart();
}

// ---------------- CONTROLS (button/dial reference) ----------------
// Grouped the same way the README's own button table is: what each control
// does while actually reading a book vs. everywhere else (Home, Choose
// Book, Bookmarks, Settings, and the Yes/No confirm screens all share the
// same "Menus" behavior), plus the one further exception (the Wi-Fi
// screen). Kept to plain fixed strings rather than trying to generate this
// from the same logic handleButtons() runs, since that logic is spread
// across a big screen-by-screen switch and isn't naturally describable as
// a list of lines -- this can drift from it if that switch changes without
// a matching update here, same risk any hand-written doc/code pair has.
const char* const HELP_LINES[] = {
  "IN READER",
  "Top tap: previous page",
  "Top hold: save bookmark",
  "Bottom: next page",
  "Dial up/down: prev/next",
  "  chapter (hold to repeat)",
  "Dial press: Home menu",
  "",
  "IN MENUS",
  "(Home, Choose Book,",
  " Bookmarks, Settings,",
  " Yes/No confirms)",
  "Top: back to Home",
  "  (or cancels a confirm)",
  "Bottom: delete, where shown",
  "Dial up/down: move selection",
  "Dial press: select",
  "",
  "WI-FI SCREEN",
  "Top or dial press: Home",
};
constexpr uint8_t HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);
// 5 content rows fit below the header, same math as SETTINGS_VISIBLE_ROWS.
constexpr uint8_t HELP_VISIBLE_ROWS = 5;

void renderControls() {
  beginFrame();

  String header = "Controls";
  if (HELP_LINE_COUNT > HELP_VISIBLE_ROWS) {
    header += "  ";
    header += (helpScrollOffset > 0) ? "^" : " ";
    header += (helpScrollOffset + HELP_VISIBLE_ROWS < HELP_LINE_COUNT) ? "v" : " ";
  }
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, header.c_str(), BLACK, MENU_FONT);

  uint8_t windowEnd = min((uint8_t)(helpScrollOffset + HELP_VISIBLE_ROWS), HELP_LINE_COUNT);
  for (uint8_t i = helpScrollOffset; i < windowEnd; i++) {
    uint8_t row = i - helpScrollOffset;
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + (row + 1) * MENU_LINE_HEIGHT, HELP_LINES[i], BLACK, MENU_FONT);
  }

  endFrame();
}

void enterControls() {
  helpScrollOffset = 0;
  currentScreen = SCREEN_CONTROLS;
  renderControls();
}

// delta is +1/-1 -- one line per dial tick, clamped to the line count
// rather than wrapping, since there's no "selected row" to carry the
// wraparound feel Settings/Choose Book have.
void scrollControls(int8_t delta) {
  int16_t next = (int16_t)helpScrollOffset + delta;
  if (next < 0) next = 0;
  uint8_t maxStart = (HELP_LINE_COUNT > HELP_VISIBLE_ROWS) ? HELP_LINE_COUNT - HELP_VISIBLE_ROWS : 0;
  if (next > maxStart) next = maxStart;
  helpScrollOffset = (uint8_t)next;
  renderControls();
}

void enterHome() {
  currentScreen = SCREEN_HOME;
  homeSelection = 0;
  renderHome();
}

void selectHomeItem() {
  switch (homeSelection) {
    case 0:  // Resume Last Book
      if (currentBookName.length() > 0) {
        currentScreen = SCREEN_READING;
        renderPageAt(pageStart);
      }
      break;
    case 1:  // Choose Book
      listBooks();
      chooseSelection = 0;
      chooseWindowStart = 0;
      currentScreen = SCREEN_CHOOSE_BOOK;
      renderChooseBook();
      break;
    case 2:  // Bookmarks
      enterBookmarkBooks();
      break;
    case 3:  // Connect to Wi-Fi
      enterWifiInfo();
      break;
    case 4:  // Settings
      enterSettings();
      break;
  }
}

// ---------------- BUTTONS + SLEEP ----------------
void handleButtons() {
  static bool lastMenu = false;
  static bool lastBack = false;
  static bool lastDialDown = false;
  static bool lastDialUp = false;
  static bool lastDialPress = false;
  static unsigned long menuHoldStart = 0;
  static bool menuHoldFired = false;
  static unsigned long dialUpHoldStart = 0;
  static unsigned long dialDownHoldStart = 0;
  static unsigned long lastChapterRepeatTime = 0;

  bool nowMenu = pressed(BTN_MENU);
  bool nowBack = pressed(BTN_BACK);
  bool nowDialDown = pressed(DIAL_DOWN);
  bool nowDialUp = pressed(DIAL_UP);
  bool nowDialPress = pressed(DIAL_PRESS);

  bool menuTapped = nowMenu && !lastMenu;
  bool menuReleased = !nowMenu && lastMenu;
  bool backTapped = nowBack && !lastBack;
  bool dialDownTapped = nowDialDown && !lastDialDown;
  bool dialUpTapped = nowDialUp && !lastDialUp;
  bool dialPressTapped = nowDialPress && !lastDialPress;

  // Top's tap action (previousPage in SCREEN_READING, Home everywhere else)
  // can't fire on the press edge like every other button here, since a tap
  // and the start of a hold look identical until BOOKMARK_HOLD_MS has
  // passed -- so SCREEN_READING alone waits for either the hold threshold
  // (menuHoldTriggered, fires once, still held) or a release before that
  // threshold (menuShortRelease). Every other screen keeps using menuTapped
  // instantly on press, unaffected by any of this.
  if (menuTapped) {
    menuHoldStart = millis();
    menuHoldFired = false;
  }
  bool menuHoldTriggered = nowMenu && !menuHoldFired && millis() - menuHoldStart >= BOOKMARK_HOLD_MS;
  if (menuHoldTriggered) menuHoldFired = true;
  bool menuShortRelease = menuReleased && !menuHoldFired;

  // Dial up/down hold-to-repeat (SCREEN_READING's chapter skip only -- see
  // its use below). The tap itself (dialUpTapped/dialDownTapped) always
  // fires the first skip instantly, unaffected by any of this; holding
  // past CHAPTER_SKIP_REPEAT_DELAY_MS starts additional skips every
  // CHAPTER_SKIP_REPEAT_INTERVAL_MS on top of that, until released.
  if (dialUpTapped) dialUpHoldStart = millis();
  if (dialDownTapped) dialDownHoldStart = millis();
  bool dialUpRepeat = nowDialUp && millis() - dialUpHoldStart >= CHAPTER_SKIP_REPEAT_DELAY_MS &&
                       millis() - lastChapterRepeatTime >= CHAPTER_SKIP_REPEAT_INTERVAL_MS;
  bool dialDownRepeat = nowDialDown && millis() - dialDownHoldStart >= CHAPTER_SKIP_REPEAT_DELAY_MS &&
                         millis() - lastChapterRepeatTime >= CHAPTER_SKIP_REPEAT_INTERVAL_MS;
  if (dialUpRepeat || dialDownRepeat) lastChapterRepeatTime = millis();

  if (menuTapped || backTapped || dialDownTapped || dialUpTapped || dialPressTapped || menuHoldTriggered ||
      dialUpRepeat || dialDownRepeat) {
    touchActivity();
  }

  switch (currentScreen) {
    case SCREEN_READING:
      if (menuShortRelease) previousPage();
      if (menuHoldTriggered) saveBookmark();
      if (backTapped) nextPage();
      if (dialUpTapped) previousChapter();
      if (dialDownTapped) nextChapter();
      if (dialUpRepeat) previousChapter();
      if (dialDownRepeat) nextChapter();
      if (dialPressTapped) enterHome();
      break;

    case SCREEN_HOME:
      if (dialUpTapped) {
        homeSelection = (homeSelection + HOME_ITEM_COUNT - 1) % HOME_ITEM_COUNT;
        renderHome();
      }
      if (dialDownTapped) {
        homeSelection = (homeSelection + 1) % HOME_ITEM_COUNT;
        renderHome();
      }
      if (dialPressTapped) selectHomeItem();
      break;

    case SCREEN_CHOOSE_BOOK:
      if (bookCount > 0) {
        if (dialUpTapped) {
          chooseSelection = (chooseSelection + bookCount - 1) % bookCount;
          scrollChooseWindow();
          renderChooseBook();
        }
        if (dialDownTapped) {
          chooseSelection = (chooseSelection + 1) % bookCount;
          scrollChooseWindow();
          renderChooseBook();
        }
        if (dialPressTapped) openBook(bookList[chooseSelection]);
        if (backTapped) enterConfirm(CONFIRM_DELETE_BOOK);
      }
      if (menuTapped) enterHome();
      break;

    case SCREEN_BOOKMARK_BOOKS:
      if (bookCount > 0) {
        if (dialUpTapped) {
          bookmarkBookSelection = (bookmarkBookSelection + bookCount - 1) % bookCount;
          scrollBookmarkWindow();
          renderBookmarkBooks();
        }
        if (dialDownTapped) {
          bookmarkBookSelection = (bookmarkBookSelection + 1) % bookCount;
          scrollBookmarkWindow();
          renderBookmarkBooks();
        }
        if (dialPressTapped) enterBookmarkSlots();
      }
      if (menuTapped) enterHome();
      break;

    case SCREEN_BOOKMARK_SLOTS:
      if (dialUpTapped) {
        bookmarkSlotSelection = (bookmarkSlotSelection + BOOKMARK_SLOT_COUNT - 1) % BOOKMARK_SLOT_COUNT;
        renderBookmarkSlots();
      }
      if (dialDownTapped) {
        bookmarkSlotSelection = (bookmarkSlotSelection + 1) % BOOKMARK_SLOT_COUNT;
        renderBookmarkSlots();
      }
      if (dialPressTapped && bookmarkOffsets[bookmarkSlotSelection] != BOOKMARK_EMPTY) {
        openBookAtBookmark(bookList[bookmarkBookSelection], bookmarkOffsets[bookmarkSlotSelection], bookmarkSlotSelection);
      }
      if (backTapped && bookmarkOffsets[bookmarkSlotSelection] != BOOKMARK_EMPTY) {
        enterConfirm(CONFIRM_DELETE_BOOKMARK);
      }
      if (menuTapped) enterHome();
      break;

    case SCREEN_WIFI_INFO:
      if (dialPressTapped || menuTapped) {
        exitWifiInfo();
        enterHome();
      }
      break;

    case SCREEN_SETTINGS:
      if (dialUpTapped) {
        settingsSelection = (settingsSelection + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
        scrollSettingsWindow();
        renderSettings();
      }
      if (dialDownTapped) {
        settingsSelection = (settingsSelection + 1) % SETTINGS_ITEM_COUNT;
        scrollSettingsWindow();
        renderSettings();
      }
      if (dialPressTapped) {
        switch (settingsSelection) {
          case 0: cycleSleepTimeout(); break;
          case 1: cycleDeepSleepTimeout(); break;
          case 2: cycleAutoTurn(); break;
          case 3: toggleInvertDisplay(); break;
          case 4: cycleSortMode(); break;
          case 5: cycleProgressMode(); break;
          case 6: cycleTextSize(); break;
          case 7: enterControls(); break;
          default: enterConfirm(CONFIRM_FACTORY_RESET); break;
        }
      }
      if (menuTapped) enterHome();
      break;

    case SCREEN_CONTROLS:
      if (dialUpTapped) scrollControls(-1);
      if (dialDownTapped) scrollControls(1);
      if (menuTapped || dialPressTapped) enterHome();
      break;

    case SCREEN_CONFIRM_DELETE:
      if (dialUpTapped) {
        confirmDeleteYes = true;
        renderConfirmDelete();
      }
      if (dialDownTapped) {
        confirmDeleteYes = false;
        renderConfirmDelete();
      }
      if (dialPressTapped) {
        if (confirmAction == CONFIRM_DELETE_BOOK) {
          currentScreen = SCREEN_CHOOSE_BOOK;
          if (confirmDeleteYes) deleteSelectedBook();  // re-renders Choose Book itself
          else renderChooseBook();
        } else if (confirmAction == CONFIRM_DELETE_BOOKMARK) {
          currentScreen = SCREEN_BOOKMARK_SLOTS;
          if (confirmDeleteYes) deleteSelectedBookmark();  // re-renders itself
          else renderBookmarkSlots();
        } else {  // CONFIRM_FACTORY_RESET
          if (confirmDeleteYes) {
            factoryReset();  // reboots -- does not return
          } else {
            currentScreen = SCREEN_SETTINGS;
            renderSettings();
          }
        }
      }
      if (menuTapped) enterHome();  // cancels without acting
      break;
  }

  lastMenu = nowMenu;
  lastBack = nowBack;
  lastDialDown = nowDialDown;
  lastDialUp = nowDialUp;
  lastDialPress = nowDialPress;
}

// While enabled and reading, advances to the next page once autoTurnMs has
// passed since the last page change of any kind (renderPageAtCore() resets
// lastPageTurnTime on every render, whether from a real page turn, a
// chapter jump, or opening a book/bookmark). nextPage() is already a no-op
// at the end of the book, so this doesn't need its own end-of-book check.
void maybeAutoTurn() {
  if (autoTurnMs == 0) return;
  if (currentScreen != SCREEN_READING) return;
  if (millis() - lastPageTurnTime < autoTurnMs) return;
  nextPage();
}

void maybeSleep() {
  if (sleepAfterMs == 0) return;  // "Never" preset, see cycleSleepTimeout()
  if (millis() - lastActivity < sleepAfterMs) return;

  // Wi-Fi is normally already off (it only runs while SCREEN_WIFI_INFO is
  // showing) -- only tear it down/restore it here if that's where we are.
  bool wifiActive = (currentScreen == SCREEN_WIFI_INFO);

  EPD_Sleep();
  epdNeedsFullInit = true;  // the panel needs a hardware reset to wake back up
  if (wifiActive) exitWifiInfo();
  btStop();

  uint64_t wakeMask = (1ULL << BTN_MENU) | (1ULL << BTN_BACK) | (1ULL << DIAL_DOWN) |
                      (1ULL << DIAL_UP) | (1ULL << DIAL_PRESS);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);

  if (deepSleepAfterMs == 0) {
    // Deep sleep escalation is off -- exactly today's behavior: block here
    // (near-zero power draw while waiting) until a real button press wakes
    // it, how ever long that takes.
    esp_light_sleep_start();
  } else {
    // Also wake on a timer, purely to re-check the clock -- light sleep
    // otherwise only wakes on a real button press, so without this there'd
    // be no way to notice that deepSleepAfterMs has elapsed with nothing
    // pressed. Loops back into light sleep each time the timer (not a
    // button) is what woke it, until either a button is pressed (real
    // activity -- fall through below like normal) or the combined
    // sleepAfterMs + deepSleepAfterMs threshold is crossed, at which point
    // it reboots into deep sleep instead (see setup() for the wake side of
    // that -- deep sleep can't just "return" here like light sleep does).
    uint32_t deepThresholdMs = sleepAfterMs + deepSleepAfterMs;
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_CHECK_INTERVAL_US);
    while (true) {
      esp_light_sleep_start();
      if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) break;
      if (millis() - lastActivity >= deepThresholdMs) {
        renderDeepSleepSplash();
        esp_deep_sleep_start();  // does not return
      }
    }
  }

  touchActivity();
  if (wifiActive) {
    setupAP();
    setupWebServer();
  }
  // The e-paper stays asleep until the next render call wakes it; the panel
  // keeps showing the last image in the meantime since it is bistable.
}

// ---------------- ARDUINO ----------------
void setup() {
  Serial.begin(115200);

  pinMode(BTN_MENU, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(DIAL_DOWN, INPUT_PULLUP);
  pinMode(DIAL_UP, INPUT_PULLUP);
  pinMode(DIAL_PRESS, INPUT_PULLUP);

  pinMode(EPD_POWER_PIN, OUTPUT);
  digitalWrite(EPD_POWER_PIN, HIGH);

  prefs.begin("settings", false);
  sleepAfterMs = prefs.getUInt("sleepMs", 60000);
  deepSleepAfterMs = prefs.getUInt("deepSleepMs", 0);
  autoTurnMs = prefs.getUInt("autoTurnMs", 0);
  invertDisplay = prefs.getBool("invert", false);
  sortMode = (SortMode)prefs.getUChar("sortMode", SORT_AZ);
  progressMode = (ProgressMode)prefs.getUChar("progressMode", PROGRESS_OFF);
  applyBookFont(prefs.getUChar("bookFont", 16));

  if (!LittleFS.begin(true)) {
    showMessage("LittleFS failed");
    while (true) delay(1000);
  }
  if (!LittleFS.exists("/books")) LittleFS.mkdir("/books");

  registerWebRoutes();

  // Wi-Fi stays off until the user opens the Connect to Wi-Fi screen -- see
  // enterWifiInfo()/exitWifiInfo() -- to save battery the rest of the time.
  WiFi.mode(WIFI_OFF);

  currentBookName = loadCurrentBookName();
  if (currentBookName.length() == 0 || !LittleFS.exists(bookPath(currentBookName))) {
    listBooks();
    currentBookName = (bookCount > 0) ? bookList[0] : "";
  }

  // Loads/indexes the last book (if any) so Resume Last Book is instant.
  uint32_t saved = currentBookName.length() > 0 ? loadSavedPosition(currentBookName) : 0;
  reopenBookAt(saved);
  indexChapters();
  refreshPageTracking(saved);
  touchActivity();

  // A real cold boot (power-on, reset, freshly flashed) lands on Home, same
  // as always. But this reboot might instead be deep sleep waking back up
  // (see maybeSleep()/cycleDeepSleepTimeout()) -- that's a real button
  // press too (ESP_SLEEP_WAKEUP_EXT1, same wake source light sleep uses),
  // just one that happens to require a reboot to act on. Landing on Home in
  // that case would be a jarring change from how waking up always looked
  // before deep sleep existed (light sleep never reboots, so it just
  // resumes whatever screen was already showing) -- go straight back to
  // reading instead, the same place Resume Last Book goes.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 && currentBookName.length() > 0) {
    currentScreen = SCREEN_READING;
    renderPageAt(pageStart);
  } else {
    currentScreen = SCREEN_HOME;
    renderHome();
  }
}

void loop() {
  server.handleClient();
  handleButtons();
  maybeAutoTurn();
  maybeSleep();
  delay(20);
}
