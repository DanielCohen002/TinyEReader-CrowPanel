/*
  Tiny ESP32-S3 E-Reader
  Elecrow CrowPanel ESP32 E-Paper HMI 2.13" (model DIE01021S, SSD1680Z/JD79661)

  Features:
  - Creates a Wi-Fi upload page at http://192.168.4.1
  - Multi-book library: each upload adds a book instead of replacing one
  - Remembers reading position per book, boots straight into the last book read
  - Real page-start history so Back goes to the actual previous page
  - Home menu (Resume Last Book / Choose Book / Connect to Wi-Fi)
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

// ---------------- HARDWARE CONFIG ----------------
// Button/dial pins per Elecrow's own examples (2.13_key.ino) and product wiki.
// While reading a book: top = previous chapter, bottom = next chapter,
// dial rotate = page turn, dial press = Home. In menus: top/bottom/dial
// press mean Back/Select as documented on each screen's handler below.
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

constexpr size_t MAX_BOOK_SIZE = 600 * 1024;  // per book; LittleFS partition is 1.5MB total
constexpr uint16_t PAGE_HISTORY_LIMIT = 128;
constexpr uint32_t SLEEP_AFTER_MS = 60000;
constexpr uint8_t MAX_BOOKS = 6;  // library list isn't scrollable yet -- keep it to what fits on screen

// Book text layout (12x24 font -- big enough to use nearly the full 122px height).
constexpr uint16_t BOOK_LEFT_MARGIN = 4;
constexpr uint16_t BOOK_TOP_MARGIN = 1;
constexpr uint16_t BOOK_LINE_HEIGHT = 24;
constexpr uint8_t BOOK_FONT = 24;
constexpr uint8_t BOOK_CHARS_PER_LINE = 20;
constexpr uint8_t BOOK_MAX_LINES = 5;

// Menu screen layout (8x16 font -- smaller, so list items/labels fit comfortably).
constexpr uint16_t MENU_LEFT_MARGIN = 6;
constexpr uint16_t MENU_TOP_MARGIN = 4;
constexpr uint16_t MENU_LINE_HEIGHT = 20;
constexpr uint8_t MENU_FONT = 16;

WebServer server(80);

enum AppScreen : uint8_t { SCREEN_READING, SCREEN_HOME, SCREEN_CHOOSE_BOOK, SCREEN_WIFI_INFO };
AppScreen currentScreen = SCREEN_READING;

const char* HOME_ITEMS[] = { "Resume Last Book", "Choose Book", "Connect to Wi-Fi" };
constexpr uint8_t HOME_ITEM_COUNT = 3;
uint8_t homeSelection = 0;

String bookList[MAX_BOOKS];
uint8_t bookCount = 0;
uint8_t chooseSelection = 0;

String currentBookName;  // filename only, e.g. "MyNovel.txt" -- lives under /books/

File book;
size_t fileSize = 0;
uint32_t pageStart = 0;
uint32_t pageEnd = 0;
uint32_t pageHistory[PAGE_HISTORY_LIMIT];
uint16_t historyCount = 0;
unsigned long lastActivity = 0;

// Chapter boundaries are marked in the uploaded .txt with a form-feed byte
// (0x0C, '\f') -- see tools/epub_to_txt.py, which inserts one at each
// detected chapter heading when converting an EPUB. Plain hand-typed .txt
// files with no form feeds just get a single implicit "chapter" at offset 0.
constexpr uint16_t MAX_CHAPTERS = 200;
uint32_t chapterOffsets[MAX_CHAPTERS];
uint16_t chapterCount = 0;

const char uploadPage[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>PocketReader</title>
  <style>
    body { font-family: system-ui, sans-serif; margin: 2rem; line-height: 1.35; }
    input, button { font: inherit; margin-top: 1rem; }
  </style>
</head>
<body>
  <h1>PocketReader</h1>
  <p>Uploading adds a new book to your library (up to 600KB per book). It becomes the active book right away.</p>
  <form method="POST" action="/upload" enctype="multipart/form-data">
    <input type="file" name="file" accept=".txt,text/plain">
    <button type="submit">Upload TXT</button>
  </form>
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

void pushHistory(uint32_t offset) {
  if (historyCount > 0 && pageHistory[historyCount - 1] == offset) return;

  if (historyCount == PAGE_HISTORY_LIMIT) {
    memmove(pageHistory, pageHistory + 1, sizeof(pageHistory[0]) * (PAGE_HISTORY_LIMIT - 1));
    historyCount--;
  }

  pageHistory[historyCount++] = offset;
}

uint32_t popHistory() {
  if (historyCount == 0) return 0;
  return pageHistory[--historyCount];
}

void renderPageAt(uint32_t offset, bool rememberPrevious);

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
  // reopens and re-seeks the book itself.
}

void nextChapter() {
  for (uint16_t i = 0; i < chapterCount; i++) {
    if (chapterOffsets[i] > pageStart) {
      renderPageAt(chapterOffsets[i], true);
      return;
    }
  }
}

void previousChapter() {
  for (int16_t i = (int16_t)chapterCount - 1; i >= 0; i--) {
    if (chapterOffsets[i] < pageStart) {
      renderPageAt(chapterOffsets[i], true);
      return;
    }
  }
  if (chapterCount > 0) renderPageAt(chapterOffsets[0], true);
}

void openBook(const String& name) {
  currentBookName = name;
  saveCurrentBookName();
  historyCount = 0;
  uint32_t saved = loadSavedPosition(name);
  reopenBookAt(saved);
  indexChapters();
  currentScreen = SCREEN_READING;
  renderPageAt(saved, false);
}

// ---------------- WIFI + WEB ----------------
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME, AP_PASS);
}

void handleRoot() {
  server.send(200, "text/html", uploadPage);
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
    server.send(413, "text/plain", "File too large or library full (limit 600KB per book)");
    return;
  }

  openBook(uploadName);
  server.send(200, "text/plain", "Upload complete: " + uploadName);
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUpload);
  server.begin();
}

// ---------------- READER ----------------
// Greedy word-wrap: build the line word by word and stop before a word that
// would overflow BOOK_CHARS_PER_LINE, instead of cutting mid-word. If a word
// won't fit, seek back to its start so it becomes the start of the next line.
// '\f' (form feed) is a chapter marker inserted by tools/epub_to_txt.py --
// it's treated like a hard line break here but never printed, since
// EPD_ShowString would otherwise just silently stop rendering at it (it's
// below the printable ASCII range the font covers).
String readWrappedLine() {
  String line;

  while (true) {
    while (book.available() && book.peek() == ' ') book.read();

    if (!book.available()) break;
    if (book.peek() == '\n' || book.peek() == '\f') {
      book.read();
      break;
    }

    uint32_t wordStart = book.position();
    String word;
    while (book.available()) {
      char c = book.peek();
      if (c == ' ' || c == '\n' || c == '\r' || c == '\f') break;
      word += (char)book.read();
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

    if (book.available() && book.peek() == '\r') book.read();
    if (book.available() && (book.peek() == '\n' || book.peek() == '\f')) {
      book.read();
      break;
    }
  }

  return line;
}

void renderPageAt(uint32_t offset, bool rememberPrevious) {
  if (currentBookName.length() == 0 || !LittleFS.exists(bookPath(currentBookName))) {
    showMessage("Upload a TXT at\n192.168.4.1\nor press Menu\nfor the Home screen");
    return;
  }

  if (rememberPrevious && pageStart != offset) pushHistory(pageStart);

  reopenBookAt(offset);
  if (!book) {
    showMessage("Book open failed");
    return;
  }

  String lines[BOOK_MAX_LINES];
  uint8_t lineCount = 0;

  while (book.available() && lineCount < BOOK_MAX_LINES) {
    lines[lineCount] = readWrappedLine();
    if (lines[lineCount].length() > 0 || book.available()) lineCount++;
  }

  pageEnd = book.position();

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
  renderPageAt(pageEnd, true);
}

void previousPage() {
  if (historyCount == 0) return;
  renderPageAt(popHistory(), false);
}

// ---------------- MENUS ----------------
void renderHome() {
  beginFrame();
  for (uint8_t i = 0; i < HOME_ITEM_COUNT; i++) {
    String label = (i == homeSelection) ? "> " : "  ";
    label += HOME_ITEMS[i];
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + i * MENU_LINE_HEIGHT, label.c_str(), BLACK, MENU_FONT);
  }
  endFrame();
}

void renderChooseBook() {
  beginFrame();
  if (bookCount == 0) {
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, "No books yet.", BLACK, MENU_FONT);
    EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + MENU_LINE_HEIGHT, "Upload one first.", BLACK, MENU_FONT);
  } else {
    for (uint8_t i = 0; i < bookCount; i++) {
      String label = (i == chooseSelection) ? "> " : "  ";
      String name = bookList[i];
      if (name.length() > 26) name = name.substring(0, 23) + "...";
      label += name;
      EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + i * MENU_LINE_HEIGHT, label.c_str(), BLACK, MENU_FONT);
    }
  }
  endFrame();
}

void renderWifiInfo() {
  beginFrame();
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN, "Connect to Wi-Fi:", BLACK, MENU_FONT);
  String ssidLine = "SSID: ";
  ssidLine += AP_NAME;
  String passLine = "Pass: ";
  passLine += AP_PASS;
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + MENU_LINE_HEIGHT, ssidLine.c_str(), BLACK, MENU_FONT);
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 2 * MENU_LINE_HEIGHT, passLine.c_str(), BLACK, MENU_FONT);
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 3 * MENU_LINE_HEIGHT, "Then open:", BLACK, MENU_FONT);
  EPD_ShowString(MENU_LEFT_MARGIN, MENU_TOP_MARGIN + 4 * MENU_LINE_HEIGHT, "192.168.4.1", BLACK, MENU_FONT);
  endFrame();
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
        renderPageAt(pageStart, false);
      }
      break;
    case 1:  // Choose Book
      listBooks();
      chooseSelection = 0;
      currentScreen = SCREEN_CHOOSE_BOOK;
      renderChooseBook();
      break;
    case 2:  // Connect to Wi-Fi
      currentScreen = SCREEN_WIFI_INFO;
      renderWifiInfo();
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

  if (menuTapped || backTapped || dialDownTapped || dialUpTapped || dialPressTapped) touchActivity();

  switch (currentScreen) {
    case SCREEN_READING:
      if (dialDownTapped) nextPage();
      if (dialUpTapped) previousPage();
      if (dialPressTapped) enterHome();
      if (menuTapped) previousChapter();
      if (backTapped) nextChapter();
      break;

    case SCREEN_HOME:
      if (dialDownTapped) {
        homeSelection = (homeSelection + 1) % HOME_ITEM_COUNT;
        renderHome();
      }
      if (dialUpTapped) {
        homeSelection = (homeSelection + HOME_ITEM_COUNT - 1) % HOME_ITEM_COUNT;
        renderHome();
      }
      if (backTapped || dialPressTapped) selectHomeItem();
      if (menuTapped && currentBookName.length() > 0) {
        currentScreen = SCREEN_READING;
        renderPageAt(pageStart, false);
      }
      break;

    case SCREEN_CHOOSE_BOOK:
      if (bookCount > 0) {
        if (dialDownTapped) {
          chooseSelection = (chooseSelection + 1) % bookCount;
          renderChooseBook();
        }
        if (dialUpTapped) {
          chooseSelection = (chooseSelection + bookCount - 1) % bookCount;
          renderChooseBook();
        }
        if (backTapped || dialPressTapped) openBook(bookList[chooseSelection]);
      }
      if (menuTapped) enterHome();
      break;

    case SCREEN_WIFI_INFO:
      if (menuTapped || backTapped || dialPressTapped) enterHome();
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

  EPD_Sleep();
  epdNeedsFullInit = true;  // the panel needs a hardware reset to wake back up
  WiFi.mode(WIFI_OFF);
  btStop();

  uint64_t wakeMask = (1ULL << BTN_MENU) | (1ULL << BTN_BACK) | (1ULL << DIAL_DOWN) |
                      (1ULL << DIAL_UP) | (1ULL << DIAL_PRESS);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_light_sleep_start();

  touchActivity();
  setupAP();
  setupWebServer();
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

  setupAP();
  setupWebServer();

  currentBookName = loadCurrentBookName();
  if (currentBookName.length() == 0 || !LittleFS.exists(bookPath(currentBookName))) {
    listBooks();
    currentBookName = (bookCount > 0) ? bookList[0] : "";
  }

  uint32_t saved = currentBookName.length() > 0 ? loadSavedPosition(currentBookName) : 0;
  reopenBookAt(saved);
  indexChapters();
  currentScreen = SCREEN_READING;
  renderPageAt(saved, false);
}

void loop() {
  server.handleClient();
  handleButtons();
  maybeSleep();
  delay(20);
}
