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
  - Hold Top/Bottom while reading to auto-turn multiple pages instead of
    tapping once per page
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
  - Home menu (Resume Last Book / Choose Book / Connect to Wi-Fi), each
    screen showing free space remaining out of the ~6.3MB library partition
  - Light-sleeps the ESP32 and puts the e-paper panel to sleep after inactivity,
    wakes on any button press

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
#include "EPD.h"

// 1-bit icon/QR bitmaps in EPD_ShowPicture's packed format, generated with
// tools/image_to_epd.py from hand-drawn Piskel files and QR PNGs.
#include "generated/icon_book.h"
#include "generated/icon_bookshelf.h"
#include "generated/icon_wifi.h"
#include "generated/qr_wifi.h"
#include "generated/qr_webpage.h"

// ---------------- HARDWARE CONFIG ----------------
// Button/dial pins per Elecrow's own examples (2.13_key.ino) and product wiki.
//
// While reading a book: top = previous page (hold to keep going), bottom =
// next page (hold to keep going), dial up = previous chapter, dial down =
// next chapter, dial press = Home.
//
// Everywhere else (Home, Choose Book, Connect to Wi-Fi, the delete confirm
// dialog): dial up/down = move selection, dial press = select, top = jump
// straight to Home from anywhere (cancels the delete dialog without
// deleting), bottom on a highlighted book in Choose Book opens a Yes/No
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
constexpr uint32_t SLEEP_AFTER_MS = 60000;
constexpr uint8_t MAX_BOOKS = 5;  // library list isn't scrollable yet -- keep it to what fits on screen
                                   // (one row of the Choose Book screen is a free-space header)
constexpr unsigned long PAGE_REPEAT_DELAY_MS = 500;     // hold Top/Bottom this long to start auto-paging
constexpr unsigned long PAGE_REPEAT_INTERVAL_MS = 300;  // then advance one page this often while held

// Book text layout (8x16 font). 6 lines is the max that fits at all: the
// 12x24 font this used to be set to is exactly 24px tall, and 6 * 24 = 144
// is taller than the 122px screen -- there's no way to fit 6 lines of that
// size without rows overlapping. 16px is the next font size down.
constexpr uint16_t BOOK_LEFT_MARGIN = 4;
constexpr uint16_t BOOK_TOP_MARGIN = 1;
constexpr uint16_t BOOK_LINE_HEIGHT = 20;
constexpr uint8_t BOOK_FONT = 16;
constexpr uint8_t BOOK_CHARS_PER_LINE = 30;
constexpr uint8_t BOOK_MAX_LINES = 6;

// Menu screen layout (8x16 font -- smaller, so list items/labels fit comfortably).
constexpr uint16_t MENU_LEFT_MARGIN = 6;
constexpr uint16_t MENU_TOP_MARGIN = 4;
constexpr uint16_t MENU_LINE_HEIGHT = 20;
constexpr uint8_t MENU_FONT = 16;

WebServer server(80);

enum AppScreen : uint8_t { SCREEN_READING, SCREEN_HOME, SCREEN_CHOOSE_BOOK, SCREEN_WIFI_INFO, SCREEN_CONFIRM_DELETE };
AppScreen currentScreen = SCREEN_READING;

constexpr uint8_t HOME_ITEM_COUNT = 3;
uint8_t homeSelection = 0;

String bookList[MAX_BOOKS];
uint8_t bookCount = 0;
uint8_t chooseSelection = 0;
bool confirmDeleteYes = false;  // which option is highlighted in the delete dialog; defaults to No

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
    ul.lib li { display: flex; justify-content: space-between; gap: 0.5rem; padding: 0.35rem 0; border-top: 1px solid #ece7dc; }
    ul.lib li .size { color: #8a8072; white-space: nowrap; }
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
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>PocketReader</h1>
    <p class="sub">Upload one or more .txt books (up to 3MB each). The last one uploaded becomes the active book.</p>

    <form id="form">
      <label class="drop" id="drop" for="file">
        <p id="dropLabel">Tap to choose books</p>
        <p class="hint">or drag &amp; drop .txt files here</p>
      </label>
      <input type="file" id="file" name="file" accept=".txt,text/plain" multiple>
      <button type="submit" id="go" disabled>Upload</button>
    </form>
    <div class="files" id="files"></div>

    <h2>Library</h2>
    <p class="lib-meta" id="libMeta">Loading...</p>
    <ul class="lib" id="libList"></ul>
  </div>

  <script>
    var fileInput = document.getElementById('file');
    var drop = document.getElementById('drop');
    var dropLabel = document.getElementById('dropLabel');
    var go = document.getElementById('go');
    var filesBox = document.getElementById('files');
    var form = document.getElementById('form');
    var picked = [];
    var libraryFull = false;

    function fmtSize(n) {
      if (n > 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + 'MB';
      if (n > 1024) return Math.round(n / 1024) + 'KB';
      return n + 'B';
    }

    function refreshLibrary() {
      fetch('/books').then(function (r) { return r.json(); }).then(function (data) {
        document.getElementById('libMeta').textContent =
          data.count + ' of ' + data.max + ' books - ' + data.free;
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
          name.textContent = b.name;
          var size = document.createElement('span');
          size.className = 'size';
          size.textContent = fmtSize(b.size);
          li.appendChild(name);
          li.appendChild(size);
          list.appendChild(li);
        });
        libraryFull = data.count >= data.max;
        go.disabled = picked.length === 0 || libraryFull;
        if (libraryFull && picked.length === 0) {
          dropLabel.textContent = 'Library full - delete a book on the device first';
        }
      }).catch(function () {
        document.getElementById('libMeta').textContent = '';
      });
    }

    function setPicked(fileList) {
      picked = Array.prototype.filter.call(fileList, function (f) {
        return f.name.toLowerCase().endsWith('.txt');
      });
      if (picked.length === 0) {
        dropLabel.textContent = libraryFull ? 'Library full - delete a book on the device first' : 'Tap to choose books';
      } else {
        dropLabel.textContent = picked.length + ' book' + (picked.length > 1 ? 's' : '') + ' selected';
      }
      go.disabled = picked.length === 0 || libraryFull;
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

    function uploadOne(file, isLast) {
      return new Promise(function (resolve) {
        var row = document.createElement('div');
        row.className = 'file-row';
        var name = document.createElement('span');
        name.textContent = file.name;
        var status = document.createElement('span');
        status.className = 'status';
        status.textContent = '0%';
        row.appendChild(name);
        row.appendChild(status);
        filesBox.appendChild(row);

        var xhr = new XMLHttpRequest();
        xhr.open('POST', '/upload?open=' + (isLast ? '1' : '0'));
        xhr.upload.onprogress = function (e) {
          if (e.lengthComputable) status.textContent = Math.round((e.loaded / e.total) * 100) + '%';
        };
        xhr.onload = function () {
          if (xhr.status === 200) {
            row.classList.add('done');
            status.textContent = 'Done';
          } else {
            row.classList.add('error');
            status.textContent = xhr.status === 413 ? 'Too large' : 'Failed';
          }
          resolve();
        };
        xhr.onerror = function () {
          row.classList.add('error');
          status.textContent = 'Failed';
          resolve();
        };
        var data = new FormData();
        data.append('file', file);
        xhr.send(data);
      });
    }

    form.addEventListener('submit', function (e) {
      e.preventDefault();
      if (picked.length === 0) return;
      go.disabled = true;
      filesBox.innerHTML = '';
      var chain = Promise.resolve();
      picked.forEach(function (file, i) {
        chain = chain.then(function () { return uploadOne(file, i === picked.length - 1); });
      });
      chain.then(function () {
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

void listBooks() {
  bookCount = 0;
  File dir = LittleFS.open("/books");
  if (!dir || !dir.isDirectory()) return;

  File f = dir.openNextFile();
  while (f && bookCount < MAX_BOOKS) {
    String name = String(f.name());
    int slash = name.lastIndexOf('/');
    if (slash != -1) name = name.substring(slash + 1);
    if (name.length() > 0 && !name.endsWith(".pos")) {
      bookList[bookCount++] = name;
    }
    f = dir.openNextFile();
  }
}

void renderChooseBook();

void deleteSelectedBook() {
  if (bookCount == 0) return;
  String name = bookList[chooseSelection];

  if (name == currentBookName) {
    if (book) book.close();
    currentBookName = "";
    LittleFS.remove("/current.txt");
  }

  LittleFS.remove(bookPath(name));
  LittleFS.remove(posPath(name));

  listBooks();
  if (bookCount == 0) {
    chooseSelection = 0;
  } else if (chooseSelection >= bookCount) {
    chooseSelection = bookCount - 1;
  }
  renderChooseBook();
}

void renderPageAt(uint32_t offset);

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
      renderPageAt(chapterOffsets[i]);
      return;
    }
  }
}

void previousChapter() {
  for (int16_t i = (int16_t)chapterCount - 1; i >= 0; i--) {
    if (chapterOffsets[i] < pageStart) {
      renderPageAt(chapterOffsets[i]);
      return;
    }
  }
  if (chapterCount > 0) renderPageAt(chapterOffsets[0]);
}

void openBook(const String& name) {
  // indexChapters() does a byte-by-byte scan of the whole file, which can take
  // a couple of seconds on a large book -- show a message first so this reads
  // as "working" rather than a frozen screen.
  showMessage("Opening book...");

  currentBookName = name;
  saveCurrentBookName();
  uint32_t saved = loadSavedPosition(name);
  reopenBookAt(saved);
  indexChapters();
  currentScreen = SCREEN_READING;
  renderPageAt(saved);
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
  json += "],\"count\":" + String(bookCount) +
          ",\"max\":" + String(MAX_BOOKS) +
          ",\"free\":\"" + freeSpaceLabel() + "\"}";

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
    server.send(413, "text/plain", "File too large or library full (limit 3MB per book)");
    return;
  }

  // The upload page batches multi-file selections into one request per file
  // and only asks to open the last one, so picking several books doesn't
  // flash the e-paper panel open on every single file in the batch.
  if (server.arg("open") != "0") openBook(uploadName);
  server.send(200, "text/plain", "Upload complete: " + uploadName);
}

// Routes are registered once, at boot, regardless of Wi-Fi state --
// registering them doesn't require Wi-Fi to be up. setupWebServer() itself
// gets called every time the Wi-Fi screen is entered (and after waking from
// sleep while on it), so it only starts/stops listening; re-registering the
// same routes on every visit would leak a RequestHandler node each time.
void registerWebRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/books", HTTP_GET, handleBooksList);
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
// would overflow BOOK_CHARS_PER_LINE, instead of cutting mid-word. If a word
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

    if (word.length() > BOOK_CHARS_PER_LINE) {
      // Single word longer than a whole line (long URL, etc): hard-break it
      // rather than looping forever. Flush whatever we already have first so
      // the long word starts cleanly on its own line.
      if (line.length() > 0) {
        book.seek(wordStart);
        break;
      }
      book.seek(wordStart + BOOK_CHARS_PER_LINE);
      return word.substring(0, BOOK_CHARS_PER_LINE);
    }

    uint16_t candidateLen = line.length() + (line.length() > 0 ? 1 : 0) + word.length();
    if (candidateLen > BOOK_CHARS_PER_LINE) {
      book.seek(wordStart);
      break;
    }

    if (line.length() > 0) line += ' ';
    line += word;
  }

  return line;
}

// Wraps text starting at `offset` (assumes `book` is already open on the
// current book) into up to BOOK_MAX_LINES lines and returns where the
// resulting page ends. Shared by the real render path and by
// findPreviousPageStart()'s forward-replay simulation below -- both need to
// agree exactly on where pages break, or "page up" could land somewhere
// that doesn't match what was actually shown.
uint32_t computePageEnd(uint32_t offset, String* outLines, uint8_t* outLineCount) {
  if (!book) return offset;
  if (offset > fileSize) offset = 0;
  book.seek(offset);

  uint8_t lineCount = 0;
  while (book.available() && lineCount < BOOK_MAX_LINES) {
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

void renderPageAt(uint32_t offset) {
  if (currentBookName.length() == 0 || !LittleFS.exists(bookPath(currentBookName))) {
    showMessage("Upload a TXT at\n192.168.4.1\nor press Menu\nfor the Home screen");
    return;
  }

  reopenBookAt(offset);
  if (!book) {
    showMessage("Book open failed");
    return;
  }

  String lines[BOOK_MAX_LINES];
  uint8_t lineCount = 0;
  pageEnd = computePageEnd(offset, lines, &lineCount);

  beginFrame();

  for (uint8_t i = 0; i < lineCount; i++) {
    EPD_ShowString(BOOK_LEFT_MARGIN, BOOK_TOP_MARGIN + (i * BOOK_LINE_HEIGHT), lines[i].c_str(), BLACK, BOOK_FONT);
  }

  endFrame();

  savePosition();
  touchActivity();
}

void nextPage() {
  if (!book || pageEnd >= fileSize) return;
  renderPageAt(pageEnd);
}

void previousPage() {
  if (!book || pageStart == 0) return;
  renderPageAt(findPreviousPageStart(pageStart));
}

// ---------------- MENUS ----------------
const uint8_t* HOME_ICONS[] = { iconBook, iconBookshelf, iconWifi };
const char* HOME_SHORT_LABELS[] = { "Read", "Books", "Wi-Fi" };

// Icon-first layout: a row of three 32x32 icons with a small label under
// each, centered as a block in the 250x122 screen, selection shown as a
// border box rather than a "> " prefix (there's no list to prefix here).
constexpr uint16_t HOME_ICON_SIZE = 48;
constexpr uint16_t HOME_ICON_Y = 30;
constexpr uint8_t HOME_LABEL_FONT = 12;  // 6x12 -- deliberately small/secondary to the icon
constexpr uint16_t HOME_LABEL_Y = HOME_ICON_Y + HOME_ICON_SIZE + 4;
const uint16_t HOME_ICON_X[HOME_ITEM_COUNT] = { 26, 100, 174 };  // evenly spaced, symmetric margins

void renderHome() {
  beginFrame();
  for (uint8_t i = 0; i < HOME_ITEM_COUNT; i++) {
    uint16_t x = HOME_ICON_X[i];
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

// Row 0 is a free-space header; the book list starts one row down. Dial
// press opens the highlighted book; dial down opens a Yes/No delete
// confirmation for it instead (see renderConfirmDelete/enterConfirmDelete).
void renderChooseBook() {
  beginFrame();
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, freeSpaceLabel().c_str(), BLACK, MENU_FONT);

  if (bookCount == 0) {
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + MENU_LINE_HEIGHT, "No books yet.", BLACK, MENU_FONT);
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 2 * MENU_LINE_HEIGHT, "Upload one first.", BLACK, MENU_FONT);
  } else {
    for (uint8_t i = 0; i < bookCount; i++) {
      String label = (i == chooseSelection) ? "> " : "  ";
      String name = bookList[i];
      if (name.length() > 26) name = name.substring(0, 23) + "...";
      label += name;
      EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + (i + 1) * MENU_LINE_HEIGHT, label.c_str(), BLACK, MENU_FONT);
    }
  }
  endFrame();
}

// Top/Bottom and dial up/down all move the Yes/No highlight here (same
// up/down convention as every other screen); dial press confirms whichever
// is highlighted. Defaults to No so nothing destructive happens unless the
// user deliberately moves to Yes and confirms.
void renderConfirmDelete() {
  beginFrame();
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, "Delete this book?", BLACK, MENU_FONT);

  String name = (chooseSelection < bookCount) ? bookList[chooseSelection] : "";
  if (name.length() > 26) name = name.substring(0, 23) + "...";
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + MENU_LINE_HEIGHT, name.c_str(), BLACK, MENU_FONT);

  String yesLabel = confirmDeleteYes ? "> Yes" : "  Yes";
  String noLabel = !confirmDeleteYes ? "> No" : "  No";
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 3 * MENU_LINE_HEIGHT, yesLabel.c_str(), BLACK, MENU_FONT);
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 4 * MENU_LINE_HEIGHT, noLabel.c_str(), BLACK, MENU_FONT);

  endFrame();
}

void enterConfirmDelete() {
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
      currentScreen = SCREEN_CHOOSE_BOOK;
      renderChooseBook();
      break;
    case 2:  // Connect to Wi-Fi
      enterWifiInfo();
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
  static unsigned long menuLastRepeat = 0;
  static unsigned long backHoldStart = 0;
  static unsigned long backLastRepeat = 0;

  bool nowMenu = pressed(BTN_MENU);
  bool nowBack = pressed(BTN_BACK);
  bool nowDialDown = pressed(DIAL_DOWN);
  bool nowDialUp = pressed(DIAL_UP);
  bool nowDialPress = pressed(DIAL_PRESS);

  bool menuTapped = nowMenu && !lastMenu;
  bool backTapped = nowBack && !lastBack;
  bool dialDownTapped = nowDialDown && !lastDialDown;
  bool dialUpTapped = nowDialUp && !lastDialUp;
  bool dialPressTapped = nowDialPress && !lastDialPress;

  // Holding Top or Bottom past PAGE_REPEAT_DELAY_MS starts auto-turning
  // pages every PAGE_REPEAT_INTERVAL_MS until released, so you can hold to
  // skip several pages instead of tapping once per page. Only meaningful
  // in SCREEN_READING (see below), but harmless to track unconditionally.
  if (menuTapped) {
    menuHoldStart = millis();
    menuLastRepeat = millis();
  }
  bool menuRepeat = nowMenu && !menuTapped &&
                     millis() - menuHoldStart >= PAGE_REPEAT_DELAY_MS &&
                     millis() - menuLastRepeat >= PAGE_REPEAT_INTERVAL_MS;
  if (menuRepeat) menuLastRepeat = millis();

  if (backTapped) {
    backHoldStart = millis();
    backLastRepeat = millis();
  }
  bool backRepeat = nowBack && !backTapped &&
                     millis() - backHoldStart >= PAGE_REPEAT_DELAY_MS &&
                     millis() - backLastRepeat >= PAGE_REPEAT_INTERVAL_MS;
  if (backRepeat) backLastRepeat = millis();

  if (menuTapped || backTapped || dialDownTapped || dialUpTapped || dialPressTapped ||
      menuRepeat || backRepeat) {
    touchActivity();
  }

  switch (currentScreen) {
    case SCREEN_READING:
      if (menuTapped || menuRepeat) previousPage();
      if (backTapped || backRepeat) nextPage();
      if (dialUpTapped) previousChapter();
      if (dialDownTapped) nextChapter();
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
          renderChooseBook();
        }
        if (dialDownTapped) {
          chooseSelection = (chooseSelection + 1) % bookCount;
          renderChooseBook();
        }
        if (dialPressTapped) openBook(bookList[chooseSelection]);
        if (backTapped) enterConfirmDelete();
      }
      if (menuTapped) enterHome();
      break;

    case SCREEN_WIFI_INFO:
      if (dialPressTapped || menuTapped) {
        exitWifiInfo();
        enterHome();
      }
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
        currentScreen = SCREEN_CHOOSE_BOOK;
        if (confirmDeleteYes) {
          deleteSelectedBook();  // re-renders Choose Book itself
        } else {
          renderChooseBook();
        }
      }
      if (menuTapped) enterHome();  // cancels without deleting
      break;
  }

  lastMenu = nowMenu;
  lastBack = nowBack;
  lastDialDown = nowDialDown;
  lastDialUp = nowDialUp;
  lastDialPress = nowDialPress;
}

void maybeSleep() {
  if (millis() - lastActivity < SLEEP_AFTER_MS) return;

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
  esp_light_sleep_start();

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

  uint32_t saved = currentBookName.length() > 0 ? loadSavedPosition(currentBookName) : 0;
  reopenBookAt(saved);
  indexChapters();
  currentScreen = SCREEN_READING;
  renderPageAt(saved);
}

void loop() {
  server.handleClient();
  handleButtons();
  maybeSleep();
  delay(20);
}
