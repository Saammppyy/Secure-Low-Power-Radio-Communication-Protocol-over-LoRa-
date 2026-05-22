// ============================================================
//  Board B — Receiver Test
//  Heltec LoRa 32 V3 (SX1262) — NO Heltec library
//  Listens for packets from Node A and prints them
// ============================================================

#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED
#define OLED_SDA    17
#define OLED_SCL    18
#define OLED_RST    21
#define SCREEN_W    128
#define SCREEN_H    64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RST);

// SX1262 — same pins as Board A
SX1262 radio = new Module(8, 14, 12, 13);

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
  display.println("Node B — RX Test");
  display.println("Waiting...");
  display.display();
  Serial.println("[OK] OLED ready.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("=== Node B Receiver ===");

  setupOLED();

  int state = radio.begin(433.0, 125.0, 9, 5, 0x12, 17);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERROR] Radio init failed: %d\n", state);
    display.println("Radio FAILED");
    display.println("Code: " + String(state));
    display.display();
    while (true);
  }

  Serial.println("[OK] Radio ready. Listening...");
  display.println("Radio OK");
  display.println("Listening...");
  display.display();
}

void loop() {
  String received = "";
  int state = radio.receive(received);

  if (state == RADIOLIB_ERR_NONE) {
    // Packet received successfully
    Serial.println("─────────────────────");
    Serial.printf("[RX] Message : %s\n", received.c_str());
    Serial.printf("[RX] RSSI    : %.1f dBm\n", radio.getRSSI());
    Serial.printf("[RX] SNR     : %.1f dB\n",  radio.getSNR());
    Serial.println("─────────────────────");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Node B — RX");
    display.println(received.substring(0, 20));
    display.print("RSSI: ");
    display.print(radio.getRSSI());
    display.println(" dBm");
    display.print("SNR:  ");
    display.print(radio.getSNR());
    display.println(" dB");
    display.display();

  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // Nothing received — keep listening silently
    Serial.print(".");

  } else {
    // Actual error
    Serial.printf("[ERR] Receive failed: %d\n", state);
  }
}