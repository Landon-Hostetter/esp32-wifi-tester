// ESP32 WiFi Tester - Step 2: button test
// Shows live button state and the last press (short or long) on the OLED.
// Wire each button between its GPIO pin and GND (internal pull-ups are used).

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS  0x3C
#define PIN_SDA       21
#define PIN_SCL       22

#define DEBOUNCE_MS   30
#define LONG_PRESS_MS 1000

const uint8_t BTN_PINS[3]   = {25, 26, 27};   // change here if you wire differently
const char*   BTN_NAMES[3]  = {"UP", "DOWN", "SELECT"};

bool rawState[3]      = {false, false, false};  // true = pressed (pin reads LOW)
bool stableState[3]   = {false, false, false};
unsigned long lastChangeMs[3] = {0, 0, 0};
unsigned long pressStartMs[3] = {0, 0, 0};

String lastEvent = "none";
unsigned int eventCount = 0;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void draw() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Button test");

  // Three boxes: filled while the button is held down
  for (int i = 0; i < 3; i++) {
    int x = i * 43;
    int y = 14;
    if (stableState[i]) {
      display.fillRect(x, y, 40, 20, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.drawRect(x, y, 40, 20, SSD1306_WHITE);
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(x + 2, y + 6);
    display.print(BTN_NAMES[i]);
  }

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 42);
  display.print("Last: ");
  display.print(lastEvent);
  display.setCursor(0, 54);
  display.print("Presses: ");
  display.print(eventCount);
  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 not found - check wiring and address");
    while (true) delay(1000);
  }

  for (int i = 0; i < 3; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }
  draw();
}

void loop() {
  bool changed = false;
  unsigned long now = millis();

  for (int i = 0; i < 3; i++) {
    bool raw = (digitalRead(BTN_PINS[i]) == LOW);

    // Restart the debounce timer whenever the raw reading flips
    if (raw != rawState[i]) {
      rawState[i] = raw;
      lastChangeMs[i] = now;
    }

    // Accept the new state once it has been steady long enough
    if ((now - lastChangeMs[i]) > DEBOUNCE_MS && raw != stableState[i]) {
      stableState[i] = raw;
      changed = true;

      if (raw) {
        pressStartMs[i] = now;               // button just went down
      } else {                               // button just released
        unsigned long held = now - pressStartMs[i];
        lastEvent = String(BTN_NAMES[i]) + (held >= LONG_PRESS_MS ? " long" : " short");
        eventCount++;
        Serial.println(lastEvent);
      }
    }
  }

  if (changed) draw();
}