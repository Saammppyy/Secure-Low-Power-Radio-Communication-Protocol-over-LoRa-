# Secure-Low-Power-Radio-Communication-Protocol-over-LoRa



\# Secure Low-Power Radio Communication Protocol over LoRa



A custom communication protocol built from scratch on top of LoRa radio — with AES-128 encryption, packet integrity checking, and reliable delivery. Built on 2× Heltec LoRa 32 V3 boards.



\*\*Author:\*\* Sambhav Pattnaik — Electronics \& Computer Engineering, KIIT University



\---



\## The Problem



When a disaster hits — a cyclone, earthquake, or flood — cell towers and WiFi routers are the first things to fail. Emergency field units are left with no way to communicate securely without any external infrastructure.



\## My Solution



I built a custom radio communication system using LoRa — a long-range, low-power radio technology that works with zero infrastructure. Two devices talk directly to each other over 433 MHz radio waves that can travel 2–5 km in open terrain.



But instead of just sending plain text like most LoRa projects do, I designed my own communication protocol from scratch — with encryption, error checking, and reliable delivery built in at the byte level.



\---



\## What it does



\- \*\*Encrypts every message\*\* using AES-128-CBC — even if someone intercepts the radio signal with an SDR, they cannot read the contents

\- \*\*Verifies every packet\*\* with a CRC-16 checksum — if even one bit flips in transit due to interference, the packet is detected and discarded

\- \*\*Guarantees delivery\*\* with an ACK and retry system — the sender waits for confirmation and retransmits up to 3 times if needed

\- \*\*Detects link failure\*\* with heartbeat packets — if a node goes silent for 3 minutes, LINK LOST is displayed on the OLED

\- \*\*Shows live status\*\* on the built-in OLED display — signal strength, packet count, temperature, humidity



\---



\## Packet Structure



Every message is a compact binary packet — not plain text. I designed every byte:

┌─────┬─────┬──────┬─────┬─────┬───────┬─────┬──────────┬────────┐

│ SRC │ DST │ TYPE │ SEQ │ VER │ FLAGS │ LEN │ PAYLOAD  │ CRC-16 │

│ 1 B │ 1 B │  1 B │ 1 B │ 1 B │  1 B  │ 2 B │ 0–208 B  │  2 B   │

└─────┴─────┴──────┴─────┴─────┴───────┴─────┴──────────┴────────┘

| Field | What it does |

|-------|-------------|

| SRC / DST | Who sent it and who it's for |

| TYPE | DATA, ACK, HEARTBEAT, or ALERT |

| SEQ | Sequence number — used to drop duplicate packets |

| VER | Protocol version — for future compatibility |

| FLAGS | Is it encrypted? Does it need an ACK? Is it a retry? |

| LEN | How many bytes of payload follow |

| PAYLOAD | AES-128-CBC encrypted sensor data |

| CRC-16 | Checksum over the entire packet |



\---



\## Hardware



| Part | Details |

|------|---------|

| 2× Heltec WiFi LoRa 32 V3 | ESP32-S3 + SX1262 radio + OLED display + LiPo charging |

| Frequency | 433 MHz — legal in India, no licence needed |

| Range | 2–5 km open area |

| Cost | \~₹3,500 total |



\---



\## Libraries Used



| Library | Purpose |

|---------|---------|

| RadioLib by Jan Gromeš | Controls the SX1262 LoRa radio |

| Adafruit SSD1306 | OLED display |

| Adafruit GFX | OLED graphics |

| ArduinoJson | JSON payload formatting |

| mbedTLS (built-in) | AES-128 encryption — comes with ESP32 Arduino core |



\---



\## How to Flash



1\. Install Arduino IDE 2.x

2\. Add ESP32 board support URL in preferences:

&#x20;  `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package\_esp32\_index.json`

3\. Select board: \*\*Heltec WiFi LoRa 32(V3)\*\*

4\. Install libraries listed above via Library Manager

5\. Flash `NodeA/NodeA.ino` to Board A

6\. Flash `NodeB/NodeB.ino` to Board B

7\. Open Serial Monitor at \*\*115200 baud\*\*



\---



\## What you should see



\*\*Node A serial monitor:\*\*

=== NODE A — LoRa Secure Protocol ===

\[OK] Radio ready.

\[TX] DATA: {"t":27.3,"h":64.8,"b":3.21}

\[TX] seq=0 attempt=1 waiting ACK...

\[ACK] Confirmed seq=0



\*\*Node B serial monitor:\*\*

=== NODE B — LoRa Secure Protocol ===

\[OK] Radio ready. Listening for Node A...

\[RX] src=1 type=0x01 seq=0 RSSI=-17.0 SNR=11.2

\[DECRYPT] {"t":27.3,"h":64.8,"b":3.21}

Temp: 27.3C  Hum: 64.8%  Bat: 3.21V

\[ACK] Sent for seq=0



\---



\## Problems I ran into (and fixed)



\*\*1. OLED screen blank on boot\*\*

The Heltec V3 powers the OLED through GPIO36. You have to pull it LOW (not HIGH) to turn the display on. This is counterintuitive and not documented in most tutorials — found it in Heltec's own schematics.



\*\*2. CRC always failing\*\*

I was reading the CRC bytes from the wrong position in the buffer because I was trusting RadioLib's reported packet length, which is unreliable for short packets. Fixed by calculating the expected packet length from my own header's LEN field instead.



\*\*3. LINK LOST triggering incorrectly\*\*

The link-loss timer wasn't being reset when packets arrived during the ACK receive window — only during the main receive loop. Fixed by updating `lastPartnerSeen` in both places, and using `millis()` directly instead of a stale timestamp variable.



\---



\## What's next



\- \[ ] Python script for live RSSI/SNR graph

\- \[ ] Replay attack simulation to validate deduplication

\- \[ ] Range test — packet loss vs. distance measurements

\- \[ ] Extend to a 3-node mesh using this protocol as the base layer



