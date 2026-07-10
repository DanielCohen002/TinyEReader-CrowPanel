/*
  Tiny ESP32-S3 E-Reader
  Elecrow CrowPanel ESP32 E-Paper HMI 2.13" (model DIE01021S, SSD1680Z/JD79661)

  Features:
  - Creates a Wi-Fi upload page at http://192.168.4.1
  - Accepts a plain .txt book and stores it in LittleFS
  - Remembers reading position
  - Real page-start history so Back goes to the actual previous page
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
#define BTN_MENU     2   // HOME/MENU
#define BTN_BACK     1   // EXIT/BACK
#define DIAL_DOWN    4   // NEXT
#define DIAL_UP      6   // PREV
#define DIAL_PRESS   5   // OK/CONF

#define EPD_POWER_PIN 7  // Must be driven HIGH or the panel stays blank.

extern uint8_t ImageBW[ALLSCREEN_BYTES];  // Software framebuffer, defined in EPD.cpp.

// ---------------- APP CONFIG ----------------
const char* AP_NAME = "PocketReader";
const char* AP_PASS = "12345678";

constexpr size_t MAX_FILE_SIZE = 1500 * 1024;
constexpr uint16_t PAGE_HISTORY_LIMIT = 128;
constexpr uint32_t SLEEP_AFTER_MS = 60000;
constexpr uint8_t FULL_REFRESH_EVERY = 8;  // Full refresh every N pages to clear ghosting.

// Text layout for the 8x16 font (EPD_W x EPD_H == 250 x 122 in landscape).
constexpr uint16_t LEFT_MARGIN = 4;
constexpr uint16_t TOP_MARGIN = 2;
constexpr uint16_t LINE_HEIGHT = 16;
constexpr uint8_t LINE_FONT = 16;
constexpr uint8_t CHARS_PER_LINE = 30;
constexpr uint8_t MAX_LINES = 6;
constexpr uint8_t FOOTER_FONT = 12;

WebServer server(80);

File book;
size_t fileSize = 0;
uint32_t pageStart = 0;
uint32_t pageEnd = 0;
uint32_t pageHistory[PAGE_HISTORY_LIMIT];
uint16_t historyCount = 0;
uint8_t pagesSinceFullRefresh = 0;
unsigned long lastActivity = 0;
bool epdAwake = false;

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

void wakeEpdIfNeeded() {
  if (epdAwake) return;
  EPD_Init();
  epdAwake = true;
}

void sleepEpd() {
  if (!epdAwake) return;
  EPD_Sleep();
  epdAwake = false;
}

void showMessage(const String& message) {
  wakeEpdIfNeeded();
  memset(ImageBW, WHITE, ALLSCREEN_BYTES);

  uint16_t y = TOP_MARGIN;
  int start = 0;
  String msg = message;
  while (start < (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    String line = (nl == -1) ? msg.substring(start) : msg.substring(start, nl);
    EPD_ShowString(LEFT_MARGIN, y, line.c_str(), BLACK, LINE_FONT);
    y += LINE_HEIGHT;
    start = (nl == -1) ? msg.length() : nl + 1;
  }

  EPD_DisplayImage(ImageBW);
  EPD_Update();
  EPD_Clear_R26H();
  pagesSinceFullRefresh = 0;
}

void savePosition() {
  File pos = LittleFS.open("/pos.txt", "w");
  if (!pos) return;
  pos.print(pageStart);
  pos.close();
}

uint32_t loadSavedPosition() {
  File pos = LittleFS.open("/pos.txt", "r");
  if (!pos) return 0;
  uint32_t saved = pos.parseInt();
  pos.close();
  return saved;
}

void reopenBookAt(uint32_t offset) {
  if (book) book.close();
  book = LittleFS.open("/book.txt", "r");
  if (!book) return;
  fileSize = book.size();
  if (offset > fileSize) offset = 0;
  pageStart = offset;
  book.seek(pageStart);
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

// ---------------- WIFI + WEB ----------------
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME, AP_PASS);
}

void handleRoot() {
  server.send(200, "text/html", uploadPage);
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File incoming;
  static bool tooLarge = false;

  if (upload.status == UPLOAD_FILE_START) {
    tooLarge = false;
    if (LittleFS.exists("/book.txt")) LittleFS.remove("/book.txt");
    if (LittleFS.exists("/pos.txt")) LittleFS.remove("/pos.txt");
    incoming = LittleFS.open("/book.txt", "w");
  }

  if (upload.status == UPLOAD_FILE_WRITE && incoming && !tooLarge) {
    if (incoming.size() + upload.currentSize > MAX_FILE_SIZE) {
      tooLarge = true;
      incoming.close();
      LittleFS.remove("/book.txt");
      return;
    }
    incoming.write(upload.buf, upload.currentSize);
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (incoming) incoming.close();
    historyCount = 0;
    pageStart = 0;
    pageEnd = 0;
    reopenBookAt(0);
    server.send(tooLarge ? 413 : 200, "text/plain", tooLarge ? "File too large" : "Upload complete");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, []() {}, handleUpload);
  server.begin();
}

// ---------------- READER ----------------
String readWrappedLine() {
  String line;

  while (book.available()) {
    char c = book.read();

    if (c == '\r') continue;
    if (c == '\n') break;

    line += c;
    if (line.length() >= CHARS_PER_LINE) {
      while (book.available()) {
        char next = book.peek();
        if (next == ' ' || next == '\n' || next == '\r') {
          book.read();
          if (next == '\n') break;
        } else {
          break;
        }
      }
      break;
    }
  }

  return line;
}

void renderPageAt(uint32_t offset, bool rememberPrevious) {
  if (!LittleFS.exists("/book.txt")) {
    showMessage("Upload a TXT at\n192.168.4.1");
    return;
  }

  if (rememberPrevious && pageStart != offset) pushHistory(pageStart);

  reopenBookAt(offset);
  if (!book) {
    showMessage("Book open failed");
    return;
  }

  String lines[MAX_LINES];
  uint8_t lineCount = 0;

  while (book.available() && lineCount < MAX_LINES) {
    lines[lineCount] = readWrappedLine();
    if (lines[lineCount].length() > 0 || book.available()) lineCount++;
  }

  pageEnd = book.position();

  wakeEpdIfNeeded();
  memset(ImageBW, WHITE, ALLSCREEN_BYTES);

  for (uint8_t i = 0; i < lineCount; i++) {
    EPD_ShowString(LEFT_MARGIN, TOP_MARGIN + (i * LINE_HEIGHT), lines[i].c_str(), BLACK, LINE_FONT);
  }

  char footer[24];
  snprintf(footer, sizeof(footer), "%lu/%lu", (unsigned long)pageEnd, (unsigned long)fileSize);
  EPD_ShowString(LEFT_MARGIN, EPD_H - FOOTER_FONT - 1, footer, BLACK, FOOTER_FONT);

  EPD_DisplayImage(ImageBW);

  bool doFull = pagesSinceFullRefresh >= FULL_REFRESH_EVERY;
  if (doFull) {
    EPD_Update();
    EPD_Clear_R26H();
    pagesSinceFullRefresh = 0;
  } else {
    EPD_PartUpdate();
    pagesSinceFullRefresh++;
  }

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

  if ((nowDialDown && !lastDialDown) || (nowDialPress && !lastDialPress)) {
    touchActivity();
    nextPage();
  }

  if ((nowDialUp && !lastDialUp) || (nowBack && !lastBack)) {
    touchActivity();
    previousPage();
  }

  if (nowMenu && !lastMenu) {
    touchActivity();
    showMessage("PocketReader\nWiFi: PocketReader\n192.168.4.1");
  }

  lastMenu = nowMenu;
  lastBack = nowBack;
  lastDialDown = nowDialDown;
  lastDialUp = nowDialUp;
  lastDialPress = nowDialPress;
}

void maybeSleep() {
  if (millis() - lastActivity < SLEEP_AFTER_MS) return;

  sleepEpd();
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

  wakeEpdIfNeeded();
  EPD_ALL_Fill(WHITE);
  EPD_Update();
  EPD_Clear_R26H();

  if (!LittleFS.begin(true)) {
    showMessage("LittleFS failed");
    while (true) delay(1000);
  }

  setupAP();
  setupWebServer();

  uint32_t saved = loadSavedPosition();
  reopenBookAt(saved);
  renderPageAt(saved, false);
}

void loop() {
  server.handleClient();
  handleButtons();
  maybeSleep();
  delay(20);
}
