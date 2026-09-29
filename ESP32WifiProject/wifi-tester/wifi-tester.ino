// ESP32 WiFi Tester - Step 3: menu skeleton
// UP / DOWN move the highlight, SELECT opens a screen,
// holding SELECT for 1 second goes back to the menu.
// Sub-screens are placeholders for now.

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

// ---------- Screens and menu ----------
enum Screen { SCREEN_MENU, SCREEN_SCANNER, SCREEN_METER, SCREEN_CHANNELS, SCREEN_ABOUT };

const int MENU_COUNT = 4;
const char* MENU_ITEMS[MENU_COUNT]   = {"WiFi Scanner", "Signal Meter", "Channel Chart", "About"};
const Screen MENU_TARGETS[MENU_COUNT] = {SCREEN_SCANNER, SCREEN_METER, SCREEN_CHANNELS, SCREEN_ABOUT};

Screen currentScreen = SCREEN_MENU;
int menuIndex = 0;

// ---------- Button events ----------
enum ButtonEvent { EVT_NONE, EVT_UP, EVT_DOWN, EVT_SELECT, EVT_BACK };

const uint8_t BTN_PINS[3] = {PIN_UP, PIN_DOWN, PIN_SELECT};   // index 0=UP, 1=DOWN, 2=SELECT

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

// ---------- Drawing ----------
void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("MENU");
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  for (int i = 0; i < MENU_COUNT; i++) {
    int y = 14 + i * 12;
    if (i == menuIndex) {
      display.fillRect(0, y - 2, SCREEN_WIDTH, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(MENU_ITEMS[i]);
  }
  display.display();
}

void drawPlaceholder(const char* title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(title);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 24);
  display.print("Coming soon");

  display.setCursor(0, 54);
  display.print("Hold SELECT: back");
  display.display();
}

void draw() {
  switch (currentScreen) {
    case SCREEN_MENU:     drawMenu(); break;
    case SCREEN_SCANNER:  drawPlaceholder("WiFi Scanner"); break;
    case SCREEN_METER:    drawPlaceholder("Signal Meter"); break;
    case SCREEN_CHANNELS: drawPlaceholder("Channel Chart"); break;
    case SCREEN_ABOUT:    drawPlaceholder("About"); break;
  }
}

// ---------- Logic ----------
void handleEvent(ButtonEvent evt) {
  if (currentScreen == SCREEN_MENU) {
    if (evt == EVT_UP)   menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
    if (evt == EVT_DOWN) menuIndex = (menuIndex + 1) % MENU_COUNT;
    if (evt == EVT_SELECT) currentScreen = MENU_TARGETS[menuIndex];
  } else {
    if (evt == EVT_BACK) currentScreen = SCREEN_MENU;
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
  draw();
}

void loop() {
  ButtonEvent evt = readButtons();
  if (evt != EVT_NONE) handleEvent(evt);
}