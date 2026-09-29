// ESP32 WiFi Tester - Step 4 (rebuilt): menu architecture
//
// Navigation tree:
//   MENU
//   └─ WiFi Scanner  -> NETWORKS (list of network names)
//        └─ [network] -> OPTIONS
//             ├─ Live Signal
//             ├─ Channel & Congestion
//             ├─ Latency
//             └─ Activity      (each is a placeholder for now)
//
// Controls:
//   UP / DOWN         : move the highlight (wraps around)
//   SELECT (short)    : open the highlighted item
//   SELECT (hold 1 s) : go back one level
//
// The network list uses TEST DATA (no WiFi code yet). It will be replaced
// by real scan results in the scanner step.
//
// Layout for the two-color OLED: rows 0-15 are yellow (header only),
// rows 16-63 are blue (lists and graphs).

#include <Wire.h>
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
const int LIST_W       = 124;  // highlight width (leaves room for the scrollbar)

// ---------- Types ----------
// Arduino inserts function prototypes above the first function in the file,
// so every type used in a function signature must be defined up here.
enum ButtonEvent { EVT_NONE, EVT_UP, EVT_DOWN, EVT_SELECT, EVT_BACK };

typedef const char* (*LabelFn)(int);   // a function that returns the text for list row i

// ---------- Screens ----------
enum Screen {
  SCREEN_MENU,
  SCREEN_NETWORKS,
  SCREEN_OPTIONS,
  SCREEN_LIVE,
  SCREEN_CHANNEL,
  SCREEN_LATENCY,
  SCREEN_ACTIVITY
};

Screen currentScreen = SCREEN_MENU;

// Main menu: add future features here (keep the two arrays in step)
const int MENU_COUNT = 1;
const char* MENU_ITEMS[MENU_COUNT]    = {"WiFi Scanner"};
const Screen MENU_TARGETS[MENU_COUNT] = {SCREEN_NETWORKS};

// Options shown after picking a network
const int OPTION_COUNT = 4;
const char* OPTION_ITEMS[OPTION_COUNT]    = {"Live Signal", "Channel & Congestion", "Latency", "Activity"};
const Screen OPTION_TARGETS[OPTION_COUNT] = {SCREEN_LIVE, SCREEN_CHANNEL, SCREEN_LATENCY, SCREEN_ACTIVITY};

// ---------- List state (one per list) ----------
struct ListState {
  int cursor;   // highlighted item
  int scroll;   // first visible item
};

ListState menuState = {0, 0};
ListState netState  = {0, 0};
ListState optState  = {0, 0};

// ---------- Networks ----------
const int MAX_NETS = 20;

struct Net {
  char    ssid[33];
  int32_t rssi;
  uint8_t channel;
  uint8_t bssid[6];   // unique hardware address: how networks are told apart internally
};

Net nets[MAX_NETS];
int netCount = 0;

char netLabels[MAX_NETS][24];   // what the list shows (name, plus "(n)" if names collide)

Net  selectedNet;
char selectedLabel[24] = "";

// TEST DATA: includes a mesh pair with the same name, two hidden networks,
// and a long name, so the labeling and scrolling can be checked.
void addTestNet(const char* ssid, int rssi, uint8_t channel, uint8_t id) {
  if (netCount >= MAX_NETS) return;
  Net &n = nets[netCount++];
  strncpy(n.ssid, ssid, sizeof(n.ssid) - 1);
  n.ssid[sizeof(n.ssid) - 1] = '\0';
  n.rssi    = rssi;
  n.channel = channel;
  const uint8_t addr[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, id};
  memcpy(n.bssid, addr, 6);
}

void loadTestNetworks() {
  netCount = 0;
  addTestNet("HomeNet",                    -45,  6, 1);
  addTestNet("HomeNet",                    -58, 11, 2);
  addTestNet("Neighbor_WiFi",              -67,  1, 3);
  addTestNet("CoffeeShop_Guest",           -74,  6, 4);
  addTestNet("(hidden)",                   -79, 11, 5);
  addTestNet("(hidden)",                   -83,  1, 6);
  addTestNet("A_Very_Long_Network_Name_2", -86,  3, 7);
  addTestNet("xfinitywifi",                -88,  6, 8);
}

// Same name more than once -> "Name (1)", "Name (2)" ...; otherwise just the name.
void buildNetLabels() {
  for (int i = 0; i < netCount; i++) {
    int total = 0;
    int order = 0;
    for (int j = 0; j < netCount; j++) {
      if (strcmp(nets[j].ssid, nets[i].ssid) == 0) {
        total++;
        if (j <= i) order++;
      }
    }
    if (total > 1) snprintf(netLabels[i], sizeof(netLabels[i]), "%.15s (%d)", nets[i].ssid, order);
    else           snprintf(netLabels[i], sizeof(netLabels[i]), "%.20s", nets[i].ssid);
  }
}

// ---------- Button events ----------
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

// ---------- List helpers ----------
const char* menuLabel(int i)   { return MENU_ITEMS[i]; }
const char* optionLabel(int i) { return OPTION_ITEMS[i]; }
const char* netLabel(int i)    { return netLabels[i]; }

void moveList(ListState &st, int count, int dir) {
  if (count <= 0) return;
  st.cursor = (st.cursor + dir + count) % count;
  if (st.cursor < st.scroll) st.scroll = st.cursor;
  if (st.cursor >= st.scroll + VISIBLE_ROWS) st.scroll = st.cursor - VISIBLE_ROWS + 1;
}

// Header in the yellow band, up to 4 rows in the blue area, scrollbar if the list is longer
void drawList(const char* header, int count, const ListState &st, LabelFn label) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print(header);

  if (count <= 0) {
    display.setCursor(4, 30);
    display.print("Nothing here");
    display.display();
    return;
  }

  for (int i = 0; i < VISIBLE_ROWS; i++) {
    int idx = st.scroll + i;
    if (idx >= count) break;

    int rowTop = BLUE_TOP + i * ROW_H;   // 16, 28, 40, 52
    if (idx == st.cursor) {
      display.fillRect(0, rowTop, LIST_W, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, rowTop + 2);
    display.print(label(idx));
  }

  // Scrollbar (2 px wide at the right edge) when there is more than one page
  if (count > VISIBLE_ROWS) {
    int trackH = VISIBLE_ROWS * ROW_H;   // 48
    int thumbH = trackH * VISIBLE_ROWS / count;
    if (thumbH < 4) thumbH = 4;
    int thumbY = BLUE_TOP + (trackH - thumbH) * st.scroll / (count - VISIBLE_ROWS);
    display.fillRect(126, thumbY, 2, thumbH, SSD1306_WHITE);
  }

  display.display();
}

// ---------- Drawing ----------
void drawToolPlaceholder(const char* title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 4);
  display.print(title);

  display.setCursor(0, 18);
  display.print(selectedLabel);

  char info[24];
  snprintf(info, sizeof(info), "ch %d   %d dBm", selectedNet.channel, (int)selectedNet.rssi);
  display.setCursor(0, 30);
  display.print(info);

  display.setCursor(0, 42);
  display.print("Coming soon");

  display.setCursor(0, 54);
  display.print("Hold SELECT: back");
  display.display();
}

void draw() {
  char header[24];
  switch (currentScreen) {
    case SCREEN_MENU:
      drawList("MENU", MENU_COUNT, menuState, menuLabel);
      break;
    case SCREEN_NETWORKS:
      snprintf(header, sizeof(header), "Networks: %d", netCount);
      drawList(header, netCount, netState, netLabel);
      break;
    case SCREEN_OPTIONS:
      drawList(selectedLabel, OPTION_COUNT, optState, optionLabel);
      break;
    case SCREEN_LIVE:     drawToolPlaceholder(OPTION_ITEMS[0]); break;
    case SCREEN_CHANNEL:  drawToolPlaceholder(OPTION_ITEMS[1]); break;
    case SCREEN_LATENCY:  drawToolPlaceholder(OPTION_ITEMS[2]); break;
    case SCREEN_ACTIVITY: drawToolPlaceholder(OPTION_ITEMS[3]); break;
  }
}

// ---------- Logic ----------
void selectNetwork() {
  if (netCount == 0) return;
  selectedNet = nets[netState.cursor];
  strncpy(selectedLabel, netLabels[netState.cursor], sizeof(selectedLabel) - 1);
  selectedLabel[sizeof(selectedLabel) - 1] = '\0';

  optState.cursor = 0;
  optState.scroll = 0;
  currentScreen = SCREEN_OPTIONS;
}

void handleEvent(ButtonEvent evt) {
  switch (currentScreen) {
    case SCREEN_MENU:
      if (evt == EVT_UP)     moveList(menuState, MENU_COUNT, -1);
      if (evt == EVT_DOWN)   moveList(menuState, MENU_COUNT, +1);
      if (evt == EVT_SELECT) {
        currentScreen = MENU_TARGETS[menuState.cursor];
        // Later: start the real WiFi scan here when entering SCREEN_NETWORKS
      }
      break;

    case SCREEN_NETWORKS:
      if (evt == EVT_UP)     moveList(netState, netCount, -1);
      if (evt == EVT_DOWN)   moveList(netState, netCount, +1);
      if (evt == EVT_SELECT) selectNetwork();
      if (evt == EVT_BACK)   currentScreen = SCREEN_MENU;
      break;

    case SCREEN_OPTIONS:
      if (evt == EVT_UP)     moveList(optState, OPTION_COUNT, -1);
      if (evt == EVT_DOWN)   moveList(optState, OPTION_COUNT, +1);
      if (evt == EVT_SELECT) currentScreen = OPTION_TARGETS[optState.cursor];
      if (evt == EVT_BACK)   currentScreen = SCREEN_NETWORKS;
      break;

    default:   // the four tool screens
      if (evt == EVT_BACK)   currentScreen = SCREEN_OPTIONS;
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

  loadTestNetworks();
  buildNetLabels();
  draw();
}

void loop() {
  ButtonEvent evt = readButtons();
  if (evt != EVT_NONE) handleEvent(evt);
}