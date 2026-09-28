// ESP32 WiFi Tester - Step 1: display test
// Board: ESP32 dev board | Display: SSD1306 0.96" 128x64 I2C

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS  0x3C   // try 0x3D if the screen stays blank
#define PIN_SDA       21
#define PIN_SCL       22

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 not found - check wiring and address");
    while (true) delay(1000);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("Hello!");

  display.setTextSize(1);
  display.println();
  display.println("ESP32 WiFi Tester");
  display.println("Display test OK");
  display.display();
}

void loop() {
  // Uptime counter on the bottom line to prove the screen updates
  display.fillRect(0, 56, SCREEN_WIDTH, 8, SSD1306_BLACK);
  display.setCursor(0, 56);
  display.print("Uptime: ");
  display.print(millis() / 1000);
  display.print(" s");
  display.display();
  delay(500);
}