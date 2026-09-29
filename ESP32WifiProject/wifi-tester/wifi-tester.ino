// ESP32 WiFi Tester - Step 4: WiFi scanner
// Menu + scanner list sorted by signal strength.
//   UP / DOWN : move the highlight
//   SELECT    : open the highlighted item (in the scanner, picks a network for the meter)
//   Hold SELECT (1 s): go back
//
// Layout for the two-color OLED: rows 0-15 are yellow (header only),
// rows 16-63 are blue (lists and graphs).

#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Hardware ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS  0x3C
#define PIN_SDA       21
#define PIN_SCL       22

const uint8_t PIN_UP     = 25;
const uint8_t PIN_DOWN   = 26;
const uint8_t PIN_SELECT = 27;

#define DEBOUNCE_MS   30
#define LONG_PRESS_MS 1000

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- Layout ----------
const int BLUE_TOP     = 16;   // first blue row
const int ROW_H        = 12;
const int VISIBLE_ROWS = 4;    // 4 x 12 = 48 rows of blue

// ---------- Screens and menu ----------
enum Screen { SCREEN_MENU, SCREEN_SCANNER, SCREEN_METER, SCREEN_CHANNELS, SCREEN_ABOUT };

const int MENU_COUNT = 4;
const char* MENU_ITEMS[MENU_COUNT]    = {"WiFi Scanner", "Signal Meter", "Channel Chart", "About"};
const Screen MENU_TARGETS[MENU_COUNT] = {SCREEN_SCANNER, SCREEN_METER, SCREEN_CHANNELS, SCREEN_ABOUT};

Screen currentScreen = SCREEN_MENU;
int menuIndex = 0;

// ---------- Scan results ----------
const int MAX_NETS = 20;

struct Net {
  char    ssid[33];
  int32_t rssi;
  uint8_t channel;
  uint8_t bssid[6];
};

Net nets[MAX_NETS];
int  netCount  = 0;
bool scanning  = false;
int  cursorPos = 0;   // highlighted network
int  scrollTop = 0;   // first visible network

// The network picked for the signal meter (used in the next step)
Net  selectedNet;
bool haveSelection = false;

// ---------- Button events ----------
enum ButtonEvent { EVT_NONE, EVT_UP, EVT_DOWN, EVT_SELECT, EVT_BACK };

const uint8_t BTN_PINS[3] = {PIN_UP, PIN_DOWN, PIN_SELECT};   // 0=UP, 1=DOWN, 2=SELECT

bool rawState[3]    = {false, false, false};   // true = pressed (pin reads LOW)
bool stableState[3] = {false, false, false};
unsigned long lastChangeMs[3] = {0, 0, 0};
unsigned long selectPressMs   = 0;
bool selectLongFired          = false;

// Returns at most one event per call. UP/DOWN fire on press,
// SELECT fires on release (short) or after 1 s held (long = BACK).
ButtonEvent readButtons() {
  unsigned long now = millis();
  ButtonEvent evt = EVT_NONE;

  for (int i = 0; i < 3; i++) {
    bool raw = (digitalRead(BTN_PINS[i]) == LOW);

    if (raw != rawState[i]) {
      rawState[i] = raw;
      lastChangeMs[i] = now;
    }

    if ((now - lastChangeMs[i]) > DEBOUNCE_MS && raw != stableState[i]) {
      stableState[i] = raw;

      if (raw) {                                   // just pressed
        if (i == 0) evt = EVT_UP;
        else if (i == 1) evt = EVT_DOWN;
        else { selectPressMs = now; selectLongFired = false; }
      } else if (i == 2 && !selectLongFired) {     // SELECT released, no long press
        evt = EVT_SELECT;
      }
    }
  }

  // Long press on SELECT: fire once while still held
  if (stableState[2] && !selectLongFired && (now - selectPressMs) >= LONG_PRESS_MS) {
    selectLongFired = true;
    evt = EVT_BACK;
  }

  return evt;
}

// ---------- Scanning ----------
void startScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  netCount  = 0;
  cursorPos = 0;
  scrollTop = 0;

  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    scanning = true;                 // an earlier scan is still going: just wait for it
  } else {
    WiFi.scanDelete();               // clear old results
    WiFi.scanNetworks(true, true);   // async = true, include hidden networks
    scanning = true;
  }
}

// Call from loop(). Returns true when a scan just finished (screen needs a redraw).
bool checkScan() {
  if (!scanning) return false;

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;

  scanning = false;
  netCount = 0;

  if (n > 0) {
    for (int i = 0; i < n && i < MAX_NETS; i++) {
      String name = WiFi.SSID(i);
      if (name.length() == 0) name = "(hidden)";
      strncpy(nets[netCount].ssid, name.c_str(), sizeof(nets[netCount].ssid) - 1);
      nets[netCount].ssid[sizeof(nets[netCount].ssid) - 1] = '\0';
      nets[netCount].rssi    = WiFi.RSSI(i);
      nets[netCount].channel = WiFi.channel(i);
      memcpy(nets[netCount].bssid, WiFi.BSSID(i), 6);
      netCount++;
    }

    // Insertion sort: strongest signal first
    for (int i = 1; i < netCount; i++) {
      Net key = nets[i];
      int j = i - 1;
      while (j >= 0 && nets[j].rssi < key.rssi) {
        nets[j + 1] = nets[j];
        j--;
      }
      nets[j + 1] = key;
    }
  }

  WiFi.scanDelete();

  Serial.print("Scan done: ");
  Serial.print(netCount);
  Serial.println(" networks");
  for (int i = 0; i < netCount; i++) {
    Serial.print(nets[i].ssid);
    Serial.print("  ");
    Serial.print(nets[i].rssi);
    Serial.print(" dBm  ch ");
    Serial.println(nets[i].channel);
  }
  return true;
}

void moveCursor(int dir) {
  if (netCount == 0) return;
  cursorPos += dir;
  if (cursorPos < 0) cursorPos = 0;
  if (cursorPos > netCount - 1) cursorPos = netCount - 1;

  if (cursorPos < scrollTop) scrollTop = cursorPos;
  if (cursorPos >= scrollTop + VISIBLE_ROWS) scrollTop = cursorPos - VISIBLE_ROWS + 1;
}

void selectNetwork() {
  if (netCount == 0) return;
  selectedNet = nets[cursorPos];
  haveSelection = true;
  currentScreen = SCREEN_METER;
}

// ---------- Drawing ----------
void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 4);
  display.print("MENU");

  for (int i = 0; i < MENU_COUNT; i++) {
    int rowTop = BLUE_TOP + i * ROW_H;     // 16, 28, 40, 52
    if (i == menuIndex) {
      display.fillRect(0, rowTop, SCREEN_WIDTH, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, rowTop + 2);
    display.print(MENU_ITEMS[i]);
  }
  display.display();
}

void drawScanner() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Header (yellow band)
  display.setCursor(0, 4);
  if (scanning) {
    display.print("Scanning...");
  } else {
    display.print("Scan: ");
    display.print(netCount);
    display.print(" found");
  }

  if (!scanning && netCount == 0) {
    display.setCursor(4, 30);
    display.print("No networks found");
    display.display();
    return;
  }

  // List (blue area): name, signal (dBm), channel
  for (int i = 0; i < VISIBLE_ROWS; i++) {
    int idx = scrollTop + i;
    if (idx >= netCount) break;

    int rowTop = BLUE_TOP + i * ROW_H;
    if (idx == cursorPos) {
      display.fillRect(0, rowTop, SCREEN_WIDTH, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    char label[13];                                   // up to 12 characters of the name
    snprintf(label, sizeof(label), "%s", nets[idx].ssid);

    display.setCursor(2, rowTop + 2);
    display.print(label);
    display.setCursor(78, rowTop + 2);
    display.print(nets[idx].rssi);
    display.setCursor(110, rowTop + 2);
    display.print(nets[idx].channel);
  }
  display.display();
}

void drawMeterPlaceholder() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print("Signal Meter");

  display.setCursor(0, 24);
  if (haveSelection) {
    display.print("Selected:");
    display.setCursor(0, 36);
    display.print(selectedNet.ssid);
  } else {
    display.print("No network selected");
  }

  display.setCursor(0, 54);
  display.print("Hold SELECT: back");
  display.display();
}

void drawPlaceholder(const char* title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print(title);

  display.setCursor(0, 24);
  display.print("Coming soon");

  display.setCursor(0, 54);
  display.print("Hold SELECT: back");
  display.display();
}

void draw() {
  switch (currentScreen) {
    case SCREEN_MENU:     drawMenu(); break;
    case SCREEN_SCANNER:  drawScanner(); break;
    case SCREEN_METER:    drawMeterPlaceholder(); break;
    case SCREEN_CHANNELS: drawPlaceholder("Channel Chart"); break;
    case SCREEN_ABOUT:    drawPlaceholder("About"); break;
  }
}

// ---------- Logic ----------
void handleEvent(ButtonEvent evt) {
  switch (currentScreen) {
    case SCREEN_MENU:
      if (evt == EVT_UP)   menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
      if (evt == EVT_DOWN) menuIndex = (menuIndex + 1) % MENU_COUNT;
      if (evt == EVT_SELECT) {
        currentScreen = MENU_TARGETS[menuIndex];
        if (currentScreen == SCREEN_SCANNER) startScan();   // scan on entry
      }
      break;

    case SCREEN_SCANNER:
      if (evt == EVT_UP)     moveCursor(-1);
      if (evt == EVT_DOWN)   moveCursor(+1);
      if (evt == EVT_SELECT) selectNetwork();
      if (evt == EVT_BACK)   currentScreen = SCREEN_MENU;
      break;

    default:
      if (evt == EVT_BACK) currentScreen = SCREEN_MENU;
      break;
  }
  draw();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 not found - check wiring and address");
    while (true) delay(1000);
  }

  for (int i = 0; i < 3; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  draw();
}

void loop() {
  ButtonEvent evt = readButtons();
  if (evt != EVT_NONE) handleEvent(evt);

  // Scan runs in the background; redraw when results arrive
  if (currentScreen == SCREEN_SCANNER && checkScan()) draw();
}