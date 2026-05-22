// ============================================================
//  NODE B — Receiver
//  LoRa Secure Communication Protocol
//  Heltec LoRa 32 V3 (SX1262)
// ============================================================

#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "mbedtls/aes.h"

#define NODE_ID       2
#define PARTNER_ID    1
#define BROADCAST     0xFF

#define PKT_DATA      0x01
#define PKT_ACK       0x02
#define PKT_HEARTBEAT 0x03
#define PKT_ALERT     0x04

#define FLAG_ENCRYPTED  0x01
#define FLAG_NEEDS_ACK  0x02

#define PROTO_VER       0x01

#define HEARTBEAT_MS    60000
#define LINK_LOST_MS    180000

const uint8_t AES_KEY[16] = {
  0x4C,0x6F,0x52,0x61,
  0x4D,0x65,0x73,0x68,
  0x4B,0x69,0x49,0x54,
  0x32,0x30,0x32,0x35
};
const uint8_t AES_IV[16] = {
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
  0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
};

#define OLED_SDA  17
#define OLED_SCL  18
#define OLED_RST  21
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);
SX1262 radio = new Module(8, 14, 12, 13);

uint8_t  mySeq           = 0;
uint8_t  lastSeenSeq     = 255;
uint32_t lastHeartbeat   = 0;
uint32_t lastPartnerSeen = 0;
bool     linkAlive       = true;
float    lastRSSI        = 0;
float    lastSNR         = 0;
int      packetsReceived = 0;

uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int j = 0; j < 8; j++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

int aesEncrypt(const uint8_t* plain, int plainLen, uint8_t* cipher) {
  int padded = ((plainLen / 16) + 1) * 16;
  uint8_t buf[256] = {0};
  memcpy(buf, plain, plainLen);
  uint8_t padByte = padded - plainLen;
  for (int i = plainLen; i < padded; i++) buf[i] = padByte;
  uint8_t iv[16]; memcpy(iv, AES_IV, 16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, AES_KEY, 128);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, padded, iv, buf, cipher);
  mbedtls_aes_free(&ctx);
  return padded;
}

int aesDecrypt(const uint8_t* cipher, int cipherLen, uint8_t* plain) {
  uint8_t iv[16]; memcpy(iv, AES_IV, 16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, AES_KEY, 128);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipherLen, iv, cipher, plain);
  mbedtls_aes_free(&ctx);
  uint8_t padByte = plain[cipherLen - 1];
  if (padByte > 16) return cipherLen;
  return cipherLen - padByte;
}

void updateDisplay(String l1, String l2, String l3, String l4) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,  0); display.println(l1);
  display.setCursor(0, 16); display.println(l2);
  display.setCursor(0, 32); display.println(l3);
  display.setCursor(0, 48); display.println(l4);
  display.display();
}

bool transmitBytes(const uint8_t* data, size_t len) {
  return radio.transmit(data, len) == RADIOLIB_ERR_NONE;
}

void sendPacket(uint8_t dst, uint8_t type,
                const char* plainPayload, bool needsAck) {
  uint8_t buf[256];
  memset(buf, 0, sizeof(buf));

  bool hasPayload = (plainPayload && strlen(plainPayload) > 0);

  uint8_t encrypted[224] = {0};
  uint16_t encLen = 0;
  if (hasPayload)
    encLen = aesEncrypt((const uint8_t*)plainPayload,
                        strlen(plainPayload), encrypted);

  buf[0] = NODE_ID;
  buf[1] = dst;
  buf[2] = type;
  buf[3] = mySeq++;
  buf[4] = PROTO_VER;
  buf[5] = (hasPayload ? FLAG_ENCRYPTED : 0) | (needsAck ? FLAG_NEEDS_ACK : 0);
  buf[6] = (encLen >> 8) & 0xFF;
  buf[7] = encLen & 0xFF;
  if (encLen > 0) memcpy(&buf[8], encrypted, encLen);

  size_t bodyLen = 8 + encLen;
  uint16_t crc = crc16(buf, bodyLen);
  buf[bodyLen]     = (crc >> 8) & 0xFF;
  buf[bodyLen + 1] = crc & 0xFF;

  transmitBytes(buf, bodyLen + 2);
}

void sendAck(uint8_t dst, uint8_t ackSeq) {
  uint8_t buf[10];
  memset(buf, 0, sizeof(buf));
  buf[0] = NODE_ID;
  buf[1] = dst;
  buf[2] = PKT_ACK;
  buf[3] = ackSeq;
  buf[4] = PROTO_VER;
  buf[5] = 0;
  buf[6] = 0;
  buf[7] = 0;
  uint16_t crc = crc16(buf, 8);
  buf[8] = (crc >> 8) & 0xFF;
  buf[9] = crc & 0xFF;
  delay(50);
  transmitBytes(buf, 10);
  Serial.printf("[ACK] Sent for seq=%d\n", ackSeq);
}

void receiveLoop() {
  uint8_t buf[256]; size_t len = sizeof(buf);
  int state = radio.receive(buf, len, 500);
  if (state == RADIOLIB_ERR_RX_TIMEOUT) return;
  if (state != RADIOLIB_ERR_NONE || len < 12) return;

  // Update partner seen as soon as any packet arrives from them
  if (buf[0] == PARTNER_ID)
    lastPartnerSeen = millis();

  // Derive length from header, not RadioLib's len
  uint16_t encLen  = ((uint16_t)buf[6] << 8) | buf[7];
  size_t bodyLen   = 8 + encLen;
  size_t totalLen  = bodyLen + 2;

  if (len < totalLen) {
    Serial.printf("[RX] Too short: got %d expected %d\n", len, totalLen);
    return;
  }

  uint16_t rxCRC   = ((uint16_t)buf[bodyLen] << 8) | buf[bodyLen + 1];
  uint16_t calcCRC = crc16(buf, bodyLen);
  if (rxCRC != calcCRC) {
    Serial.printf("[CRC FAIL] rx=0x%04X calc=0x%04X\n", rxCRC, calcCRC);
    return;
  }

  uint8_t  pktSrc     = buf[0];
  uint8_t  pktDst     = buf[1];
  uint8_t  pktType    = buf[2];
  uint8_t  pktSeq     = buf[3];
  uint8_t  pktFlags   = buf[5];
  uint16_t pktLen     = encLen;
  uint8_t* pktPayload = &buf[8];

  if (pktDst != NODE_ID && pktDst != BROADCAST) return;
  if (pktSeq == lastSeenSeq) { Serial.println("[DEDUP] dropping."); return; }
  lastSeenSeq = pktSeq;
  lastRSSI    = radio.getRSSI();
  lastSNR     = radio.getSNR();
  packetsReceived++;

  Serial.printf("[RX] src=%d type=0x%02X seq=%d len=%d RSSI=%.1f SNR=%.1f\n",
                pktSrc, pktType, pktSeq, pktLen, lastRSSI, lastSNR);

  char plain[256] = {0};
  if (pktLen > 0 && (pktFlags & FLAG_ENCRYPTED)) {
    int pl = aesDecrypt(pktPayload, pktLen, (uint8_t*)plain);
    plain[pl] = '\0';
    Serial.printf("[DECRYPT] %s\n", plain);
  }

  switch (pktType) {

    case PKT_DATA: {
      linkAlive = true;
      StaticJsonDocument<128> doc;
      DeserializationError err = deserializeJson(doc, plain);
      if (!err) {
        float tmp = doc["t"] | 0.0f;
        float hum = doc["h"] | 0.0f;
        float bat = doc["b"] | 0.0f;
        Serial.printf("  Temp: %.1fC  Hum: %.1f%%  Bat: %.2fV\n",
                      tmp, hum, bat);
        updateDisplay(
          "NODE B  rx#" + String(packetsReceived),
          "T:" + String(tmp,1) + "c H:" + String(hum,0) + "%",
          "Bat:" + String(bat,2) + "V",
          "RSSI:" + String(lastRSSI,0) + " SNR:" + String(lastSNR,0)
        );
      } else {
        Serial.println("[WARN] JSON parse failed.");
      }
      if (pktFlags & FLAG_NEEDS_ACK) sendAck(pktSrc, pktSeq);
      break;
    }

    case PKT_HEARTBEAT:
      Serial.printf("[HB] Heartbeat from Node %d\n", pktSrc);
      linkAlive = true;
      sendPacket(pktSrc, PKT_HEARTBEAT, "", false);
      updateDisplay("NODE B", "Link: ALIVE",
                    "RSSI:" + String(lastRSSI,0) + "dBm",
                    "Pkts rx: " + String(packetsReceived));
      break;

    case PKT_ALERT:
      Serial.printf("[!!!] ALERT from Node %d: %s\n", pktSrc, plain);
      for (int i = 0; i < 3; i++) {
        updateDisplay("!! ALERT !!",
                      "From Node " + String(pktSrc),
                      String(plain).substring(0, 20),
                      "RSSI:" + String(lastRSSI,0));
        delay(300);
        display.clearDisplay(); display.display();
        delay(200);
      }
      updateDisplay("!! ALERT !!",
                    "From Node " + String(pktSrc),
                    String(plain).substring(0, 20),
                    "RSSI:" + String(lastRSSI,0));
      if (pktFlags & FLAG_NEEDS_ACK) sendAck(pktSrc, pktSeq);
      linkAlive = true;
      break;

    default:
      Serial.printf("[WARN] Unknown type 0x%02X\n", pktType);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== NODE B — LoRa Secure Protocol ===");

  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);  delay(100);
  digitalWrite(OLED_RST, HIGH); delay(100);
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW);
  delay(200);
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("[ERROR] OLED failed");
  updateDisplay("NODE B", "Booting...", "AES-128-CBC", "Listening...");

  int s = radio.begin(433.0, 125.0, 9, 5, 0x12, 17);
  if (s != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERROR] Radio failed: %d\n", s);
    updateDisplay("NODE B", "RADIO FAILED", "Code:" + String(s), "");
    while (true);
  }
  Serial.println("[OK] Radio ready. Listening for Node A...");
  updateDisplay("NODE B", "Radio: OK", "AES-128: ON", "Waiting for A...");
  delay(1000);
}

void loop() {
  uint32_t now = millis();

receiveLoop();

now = millis(); // refresh after receive

  if (now - lastHeartbeat > HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.println("[HB] Sending heartbeat.");
    sendPacket(PARTNER_ID, PKT_HEARTBEAT, "", false);
  }

  if (lastPartnerSeen > 0 && (millis() - lastPartnerSeen) > LINK_LOST_MS) {
    if (linkAlive) {
      linkAlive = false;
      Serial.println("[WARN] LINK LOST — Node A silent >90s");
      updateDisplay("NODE B", "!! LINK LOST !!",
                    "Node A silent >90s", "Listening...");
    }
  }
}