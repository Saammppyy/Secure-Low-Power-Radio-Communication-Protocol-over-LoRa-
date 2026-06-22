// ============================================================
//  NODE A — Collision avoidance update
//  Changes:
//  1. Heartbeat suppressed while txState != TX_IDLE
//  2. Randomized jitter added to retry timing
//  3. Short transmit cooldown after sending an ACK
// ============================================================

#include <RadioLib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "mbedtls/aes.h"

#define NODE_ID        1
#define PARTNER_ID     2
#define BROADCAST      0xFF

#define PKT_DATA       0x01
#define PKT_ACK        0x02
#define PKT_HEARTBEAT  0x03
#define PKT_ALERT      0x04

#define FLAG_ENCRYPTED  0x01
#define FLAG_NEEDS_ACK  0x02
#define PROTO_VER       0x01

#define SEND_INTERVAL_MS   10000
#define HEARTBEAT_MS       60000
#define DATA_ACK_TIMEOUT    4000
#define MSG_ACK_TIMEOUT     8000
#define DATA_MAX_RETRIES       3
#define MSG_MAX_RETRIES        3
#define LINK_LOST_MS       180000
#define BUTTON_PIN            38
#define HOLD_TIME_MS        2000
#define RX_TIMEOUT           600
#define TX_COOLDOWN_MS       150  // cooldown after sending ACK

const uint8_t AES_KEY[16] = {
  0x4C,0x6F,0x52,0x61,0x4D,0x65,0x73,0x68,
  0x4B,0x69,0x49,0x54,0x32,0x30,0x32,0x35
};
const uint8_t AES_IV[16] = {
  0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
  0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
};

const char* MESSAGES[] = {
  "SOS","All Clear","Need Medical",
  "Need Food","Evacuate Now","Send Help"
};
const int MSG_COUNT = 6;
int selectedMsg = 0;

#define OLED_SDA  17
#define OLED_SCL  18
#define OLED_RST  21
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);
SX1262 radio = new Module(8, 14, 12, 13);

enum TxState { TX_IDLE, TX_WAIT_DATA_ACK, TX_WAIT_MSG_ACK };
TxState txState = TX_IDLE;

uint8_t  pendingSeq      = 0;
char     pendingPayload[64];
char     pendingAlert[32];
int      pendingRetry    = 0;
uint32_t ackDeadline     = 0;
bool     alertPending    = false;

uint8_t  mySeq           = 0;
uint8_t  lastSeenSeq     = 255;
uint32_t lastSentTime    = 0;
uint32_t lastHeartbeat   = 0;
uint32_t lastPartnerSeen = 0;
bool     linkAlive       = false;
float    lastRSSI        = 0;
uint32_t txCooldownUntil = 0;  // suppress our own TX briefly after ACK

bool     btnLastState    = HIGH;
uint32_t btnPressTime    = 0;
bool     holdSent        = false;
bool     showingMenu     = false;
uint32_t menuTimeout     = 0;

// ─── CRC-16 ───────────────────────────────────────────────────
uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc=0xFFFF;
  for(size_t i=0;i<len;i++){
    crc^=(uint16_t)data[i]<<8;
    for(int j=0;j<8;j++)
      crc=(crc&0x8000)?(crc<<1)^0x1021:(crc<<1);
  }
  return crc;
}

// ─── AES ──────────────────────────────────────────────────────
int aesEncrypt(const uint8_t* plain,int len,uint8_t* cipher){
  int padded=((len/16)+1)*16;
  uint8_t buf[256]={0}; memcpy(buf,plain,len);
  uint8_t pb=padded-len;
  for(int i=len;i<padded;i++) buf[i]=pb;
  uint8_t iv[16]; memcpy(iv,AES_IV,16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx,AES_KEY,128);
  mbedtls_aes_crypt_cbc(&ctx,MBEDTLS_AES_ENCRYPT,padded,iv,buf,cipher);
  mbedtls_aes_free(&ctx);
  return padded;
}

int aesDecrypt(const uint8_t* cipher,int len,uint8_t* plain){
  uint8_t iv[16]; memcpy(iv,AES_IV,16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx,AES_KEY,128);
  mbedtls_aes_crypt_cbc(&ctx,MBEDTLS_AES_DECRYPT,len,iv,cipher,plain);
  mbedtls_aes_free(&ctx);
  uint8_t pb=plain[len-1];
  return (pb>16)?len:len-pb;
}

// ─── OLED ─────────────────────────────────────────────────────
void showStatus(){
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(0,0); display.println("NODE A");
  if(linkAlive){
    display.fillCircle(8,24,6,WHITE);
    display.setCursor(20,20); display.println("LINKED");
  } else {
    display.drawCircle(8,24,6,WHITE);
    display.setCursor(20,20); display.println("NO LINK");
  }
  if(txState==TX_WAIT_MSG_ACK){
    display.setCursor(0,38); display.println("Sending msg...");
  }
  display.setCursor(0,48); display.println("[PRESS BTN: MSG]");
  display.display();
}

void showMessageMenu(){
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(0, 0); display.println("SELECT MESSAGE:");
  display.setCursor(0,16); display.println("> "+String(MESSAGES[selectedMsg]));
  display.setCursor(0,32); display.println("[PRESS: next]");
  display.setCursor(0,48); display.println("[HOLD 2s: send]");
  display.display();
}

void updateDisplay(String l1,String l2,String l3,String l4){
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(0, 0); display.println(l1);
  display.setCursor(0,16); display.println(l2);
  display.setCursor(0,32); display.println(l3);
  display.setCursor(0,48); display.println(l4);
  display.display();
}

// ─── Transmit packet — seq passed in, does NOT increment mySeq ─
void transmitPacketWithSeq(uint8_t dst, uint8_t type,
                            const char* plain, bool needsAck,
                            uint8_t seq) {
  uint8_t buf[256]; memset(buf,0,256);
  bool has=(plain&&strlen(plain)>0);
  uint8_t enc[224]={0}; uint16_t encLen=0;
  if(has) encLen=aesEncrypt((const uint8_t*)plain,strlen(plain),enc);
  buf[0]=NODE_ID; buf[1]=dst; buf[2]=type; buf[3]=seq;
  buf[4]=PROTO_VER;
  buf[5]=(has?FLAG_ENCRYPTED:0)|(needsAck?FLAG_NEEDS_ACK:0);
  buf[6]=(encLen>>8)&0xFF; buf[7]=encLen&0xFF;
  if(encLen>0) memcpy(&buf[8],enc,encLen);
  size_t bl=8+encLen;
  uint16_t crc=crc16(buf,bl);
  buf[bl]=(crc>>8)&0xFF; buf[bl+1]=crc&0xFF;
  radio.transmit(buf,bl+2);
}

uint8_t transmitNewPacket(uint8_t dst, uint8_t type,
                           const char* plain, bool needsAck){
  uint8_t seq=mySeq++;
  transmitPacketWithSeq(dst,type,plain,needsAck,seq);
  return seq;
}

void sendAck(uint8_t dst,uint8_t seq){
  uint8_t a[10]={0};
  a[0]=NODE_ID;a[1]=dst;a[2]=PKT_ACK;a[3]=seq;a[4]=PROTO_VER;
  uint16_t c=crc16(a,8);a[8]=(c>>8)&0xFF;a[9]=c&0xFF;
  radio.transmit(a,10);
  txCooldownUntil=millis()+TX_COOLDOWN_MS;  // give ACK priority on channel
}

// ─── Quick listen for ACK — used before retry ─────────────────
bool quickListenForAck(uint8_t expectedSeq, uint32_t windowMs){
  uint32_t deadline=millis()+windowMs;
  while(millis()<deadline){
    uint8_t rx[256]; size_t rxLen=sizeof(rx);
    int state=radio.receive(rx,rxLen,RX_TIMEOUT);
    if(state==RADIOLIB_ERR_NONE&&rxLen>=10){
      if(rx[0]==PARTNER_ID) lastPartnerSeen=millis();
      if(rx[2]==PKT_ACK&&rx[1]==NODE_ID&&rx[3]==expectedSeq){
        lastRSSI=radio.getRSSI();
        return true;
      }
    }
  }
  return false;
}

// ─── Start DATA send ──────────────────────────────────────────
void startDataSend(const char* payload){
  if(txState!=TX_IDLE) return;
  strncpy(pendingPayload,payload,63); pendingPayload[63]='\0';
  pendingRetry=0;
  pendingSeq=mySeq++;
  transmitPacketWithSeq(PARTNER_ID,PKT_DATA,payload,true,pendingSeq);
  ackDeadline=millis()+DATA_ACK_TIMEOUT;
  txState=TX_WAIT_DATA_ACK;
  Serial.printf("[TX] DATA seq=%d\n",pendingSeq);
}

// ─── Start ALERT send ─────────────────────────────────────────
void startAlertSend(const char* msg){
  if(txState!=TX_IDLE){
    alertPending=true;
    strncpy(pendingAlert,msg,31); pendingAlert[31]='\0';
    Serial.println("[MSG] Queued");
    return;
  }
  strncpy(pendingAlert,msg,31); pendingAlert[31]='\0';
  alertPending=false;
  pendingRetry=0;
  pendingSeq=mySeq++;
  transmitPacketWithSeq(PARTNER_ID,PKT_ALERT,msg,true,pendingSeq);
  ackDeadline=millis()+MSG_ACK_TIMEOUT;
  txState=TX_WAIT_MSG_ACK;
  Serial.printf("[MSG] ALERT seq=%d attempt=1\n",pendingSeq);
  showStatus();
}

// ─── Check TX state — retry or fail ──────────────────────────
void checkTxState(){
  if(txState==TX_IDLE){
    if(alertPending){ alertPending=false; startAlertSend(pendingAlert); }
    return;
  }
  if(millis()<ackDeadline) return;

  // Final listen before giving up
  if(quickListenForAck(pendingSeq,300)){
    if(txState==TX_WAIT_MSG_ACK){
      Serial.printf("[MSG] Late ACK seq=%d\n",pendingSeq);
      updateDisplay("** SENT OK **",String(pendingAlert),"Delivered","");
      delay(2000); showStatus();
    } else {
      Serial.printf("[DATA] Late ACK seq=%d\n",pendingSeq);
    }
    txState=TX_IDLE;
    if(alertPending){ alertPending=false; startAlertSend(pendingAlert); }
    return;
  }

  pendingRetry++;
  int maxR=(txState==TX_WAIT_MSG_ACK)?MSG_MAX_RETRIES:DATA_MAX_RETRIES;

  if(pendingRetry<maxR){
    // Randomized jitter before retransmitting — desyncs both nodes
    delay(random(50,250));
    const char* pl=(txState==TX_WAIT_MSG_ACK)?pendingAlert:pendingPayload;
    uint8_t     tp=(txState==TX_WAIT_MSG_ACK)?PKT_ALERT:PKT_DATA;
    uint32_t    to=(txState==TX_WAIT_MSG_ACK)?MSG_ACK_TIMEOUT:DATA_ACK_TIMEOUT;
    transmitPacketWithSeq(PARTNER_ID,tp,pl,true,pendingSeq);
    ackDeadline=millis()+to;
    Serial.printf("[RETRY] seq=%d attempt=%d/%d\n",
                  pendingSeq,pendingRetry+1,maxR);
  } else {
    if(txState==TX_WAIT_MSG_ACK){
      Serial.println("[MSG] FAILED");
      updateDisplay("!! FAILED !!",String(pendingAlert),"No response","Try again");
      delay(2000); showStatus();
    } else {
      Serial.println("[DATA] FAILED");
    }
    txState=TX_IDLE;
    if(alertPending){ alertPending=false; startAlertSend(pendingAlert); }
  }
}

// ─── Process received packet ──────────────────────────────────
void processPacket(uint8_t* buf,size_t len){
  if(buf[0]==PARTNER_ID){
    lastPartnerSeen=millis();
    if(!linkAlive){ linkAlive=true; showStatus(); }
  }

  uint16_t encLen=((uint16_t)buf[6]<<8)|buf[7];
  size_t bl=8+encLen,tl=bl+2;
  if(len<tl) return;
  if(crc16(buf,bl)!=(((uint16_t)buf[bl]<<8)|buf[bl+1])) return;

  uint8_t src=buf[0],dst=buf[1],type=buf[2],seq=buf[3];
  uint8_t flags=buf[5]; uint16_t plen=encLen;

  if(dst!=NODE_ID&&dst!=BROADCAST) return;

  // ACK handler — process even if seq matches lastSeenSeq (ACKs are short-lived)
  if(type==PKT_ACK){
    uint8_t ackedSeq=buf[3];
    if((txState==TX_WAIT_DATA_ACK||txState==TX_WAIT_MSG_ACK)
        &&ackedSeq==pendingSeq){
      if(txState==TX_WAIT_MSG_ACK){
        Serial.printf("[MSG] ACK confirmed seq=%d\n",ackedSeq);
        updateDisplay("** SENT OK **",String(pendingAlert),"Delivered","");
        delay(2000); showStatus();
      } else {
        Serial.printf("[DATA] ACK confirmed seq=%d\n",ackedSeq);
      }
      txState=TX_IDLE;
      if(alertPending){ alertPending=false; startAlertSend(pendingAlert); }
    }
    return;
  }

  // Dedup for non-ACK packets
  if(seq==lastSeenSeq){
    // This is our own retry being correctly ignored — but resend ACK
    // in case our previous ACK didn't make it
    if(flags&FLAG_NEEDS_ACK){
      sendAck(src,seq);
      Serial.printf("[ACK] Resent for duplicate seq=%d\n",seq);
    }
    return;
  }
  lastSeenSeq=seq;
  lastRSSI=radio.getRSSI();

  char plain[256]={0};
  if(plen>0&&(flags&FLAG_ENCRYPTED)){
    int pl=aesDecrypt(&buf[8],plen,(uint8_t*)plain);
    plain[pl]='\0';
  }

  Serial.printf("[RX] src=%d type=0x%02X seq=%d RSSI=%.1f\n",
                src,type,seq,lastRSSI);

  switch(type){
    case PKT_HEARTBEAT:
      Serial.printf("[HB] from Node %d\n",src);
      if(!showingMenu) showStatus();
      break;

    case PKT_ALERT:
      Serial.printf("[ALERT] From Node %d: %s\n",src,plain);
      sendAck(src,seq);
      for(int i=0;i<3;i++){
        updateDisplay("!! MESSAGE !!","From Node "+String(src),String(plain),"");
        delay(400); display.clearDisplay(); display.display(); delay(200);
      }
      updateDisplay("!! MESSAGE !!","From Node "+String(src),String(plain),"");
      delay(8000);
      showStatus();
      break;

    default: break;
  }
}

void receiveLoop(){
  uint8_t buf[256]; size_t len=sizeof(buf);
  int state=radio.receive(buf,len,RX_TIMEOUT);
  if(state==RADIOLIB_ERR_NONE&&len>=12)
    processPacket(buf,len);
}

void handleButton(){
  bool btnState=digitalRead(BUTTON_PIN);
  uint32_t now=millis();

  if(btnState==LOW&&btnLastState==HIGH){
    btnPressTime=now; holdSent=false;
  }

  if(btnState==LOW&&!holdSent&&(now-btnPressTime)>=HOLD_TIME_MS){
    holdSent=true; showingMenu=false;
    Serial.printf("[BTN] Sending: %s\n",MESSAGES[selectedMsg]);
    updateDisplay("SENDING...",String(MESSAGES[selectedMsg]),"Queued...","");
    delay(200);
    startAlertSend(MESSAGES[selectedMsg]);
  }

  if(btnState==HIGH&&btnLastState==LOW&&!holdSent){
    selectedMsg=(selectedMsg+1)%MSG_COUNT;
    showingMenu=true;
    menuTimeout=now+5000;
    showMessageMenu();
  }

  if(showingMenu&&now>menuTimeout){
    showingMenu=false; showStatus();
  }

  btnLastState=btnState;
}

float readTemp(){ return 27.5+random(-20,20)*0.1f; }
float readHum() { return 65.0+random(-50,50)*0.1f; }
float readBat() { return (analogRead(1)/4095.0f)*3.3f*2.0f; }

void setup(){
  Serial.begin(115200); delay(1500);
  Serial.println("\n=== NODE A ===");
  pinMode(BUTTON_PIN,INPUT_PULLUP);
  pinMode(OLED_RST,OUTPUT);
  digitalWrite(OLED_RST,LOW); delay(100);
  digitalWrite(OLED_RST,HIGH); delay(100);
  pinMode(36,OUTPUT); digitalWrite(36,LOW); delay(200);
  Wire.begin(OLED_SDA,OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC,0x3C);
  updateDisplay("NODE A","Booting...","AES-128-CBC","");
  int s=radio.begin(433.0,125.0,9,5,0x12,17);
  if(s!=RADIOLIB_ERR_NONE){
    updateDisplay("NODE A","RADIO FAILED","Code:"+String(s),"");
    while(true);
  }
  Serial.println("[OK] Ready.");
  showStatus();
}

void loop(){
  uint32_t now=millis();
  handleButton();
  receiveLoop();
  checkTxState();

  if(txState==TX_IDLE&&now-lastSentTime>SEND_INTERVAL_MS){
    lastSentTime=now;
    StaticJsonDocument<64> doc;
    doc["t"]=readTemp(); doc["h"]=readHum(); doc["b"]=readBat();
    char payload[64]; serializeJson(doc,payload);
    Serial.printf("[TX] DATA: %s\n",payload);
    startDataSend(payload);
  }

  // Suppress heartbeat while waiting for an ACK or in cooldown
  if(txState==TX_IDLE && millis()>txCooldownUntil &&
     now-lastHeartbeat>HEARTBEAT_MS){
    lastHeartbeat=now;
    transmitNewPacket(PARTNER_ID,PKT_HEARTBEAT,"",false);
  }

  if(lastPartnerSeen>0&&(millis()-lastPartnerSeen)>LINK_LOST_MS){
    if(linkAlive){
      linkAlive=false;
      Serial.println("[WARN] LINK LOST");
      if(!showingMenu) showStatus();
    }
  }
}