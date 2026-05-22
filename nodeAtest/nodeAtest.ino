// ============================================================
//  Board A — Transmitter Test
//  Heltec LoRa 32 V3 (SX1262) — NO Heltec library
//  Uses: RadioLib + Adafruit_GFX + Adafruit_SSD1306
// ============================================================

#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED
#define NODE_ID 1
##define PARTNER_ID 2
#define OLED_RST    21
#define SCREEN_W    128
#define SCREEN_H    64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RST);

// SX1262 pins on Heltec V3
SX1262 radio = new Module(8, 14, 12, 13);

int counter = 0;

void setupOLED() {
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(100);
  digitalWrite(OLED_RST, HIGH);
  delay(100);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED init failed");
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Node A — TX Test");
  display.display();
  Serial.println("[OK] OLED ready.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("=== Node A Transmitter ===");

  setupOLED();

  // Init SX1262
  int state = radio.begin(433.0, 125.0, 9, 5, 0x12, 17);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERROR] Radio init failed: %d\n", state);
    display.println("Radio FAILED");
    display.println("Code: " + String(state));
    display.display();
    while (true);
  }

  Serial.println("[OK] Radio ready.");
  display.println("Radio OK");
  display.display();
}

void loop() {
  String message = "Hello from Node A #" + String(counter);
  Serial.printf("[TX] Sending: %s\n", message.c_str());

  int state = radio.transmit(message);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("[TX] Success #%d\n", counter);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Node A — TX");
    display.println("Sent #" + String(counter));
    display.display();
  } else {
    Serial.printf("[TX] Failed: %d\n", state);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("TX FAILED");
    display.println("Code: " + String(state));
    display.display();
  }

  counter++;
  delay(2000);
}