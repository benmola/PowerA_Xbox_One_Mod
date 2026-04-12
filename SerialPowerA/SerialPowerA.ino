#include "Adafruit_TinyUSB.h"

// ============================================================
//  SerialPowerA — NRF52840 UART Bridge (Step 2 Test)
//
//  Default Serial1 pins on Adafruit Feather nRF52840:
//    RX = P0.24 (D0)
//    TX = P0.25 (D1)
//  Connect Pico GP8 to the NRF52840 D0 (RX) pin instead of P0.08
// ============================================================

#define PACKET_HEADER 0xAA
#define PACKET_SIZE   15
#define LED_PIN       LED_BUILTIN

static uint8_t pkt_buf[PACKET_SIZE];
static uint8_t pkt_idx = 0;
static uint32_t last_heartbeat = 0;
static uint32_t byte_count = 0;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // LED on = booting

  Serial.begin(115200);
  delay(3000);

  Serial.println("==========================================");
  Serial.println("  NRF52840 UART Bridge — SerialPowerA");
  Serial.println("==========================================");

  Serial1.begin(115200);  // Use default pins (D0=RX, D1=TX)

  Serial.println("Serial1 on default pins (D0 RX / D1 TX) @ 115200");
  Serial.println("Waiting for Pico data...");
  Serial.println();

  digitalWrite(LED_PIN, LOW);  // LED off = ready
}

void loop() {
  // Heartbeat: blink LED + print status every 5 seconds
  uint32_t now = millis();
  if (now - last_heartbeat > 5000) {
    last_heartbeat = now;
    digitalWrite(LED_PIN, HIGH);
    Serial.printf("[HEARTBEAT] uptime=%lus bytes_rx=%lu\r\n", now / 1000, byte_count);
    delay(50);
    digitalWrite(LED_PIN, LOW);
  }

  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    byte_count++;

    // Text pass-through (printable ASCII + newlines)
    if (pkt_idx == 0 && b != PACKET_HEADER) {
      Serial.write(b);
      continue;
    }

    // Binary packet assembly
    if (b == PACKET_HEADER && pkt_idx == 0) {
      pkt_buf[0] = b;
      pkt_idx = 1;
    } else if (pkt_idx > 0) {
      pkt_buf[pkt_idx++] = b;

      if (pkt_idx >= PACKET_SIZE) {
        uint8_t cs = 0;
        for (int i = 1; i < 14; i++) cs ^= pkt_buf[i];

        if (cs == pkt_buf[14]) {
          uint16_t buttons = pkt_buf[1] | (pkt_buf[2] << 8);
          int16_t lx = pkt_buf[3] | (pkt_buf[4] << 8);
          int16_t ly = pkt_buf[5] | (pkt_buf[6] << 8);
          int16_t rx = pkt_buf[7] | (pkt_buf[8] << 8);
          int16_t ry = pkt_buf[9] | (pkt_buf[10] << 8);
          uint8_t lt = pkt_buf[11];
          uint8_t rt = pkt_buf[12];

          Serial.printf("[PKT] BTN:%04X LT:%3d RT:%3d LX:%6d LY:%6d RX:%6d RY:%6d\r\n",
                        buttons, lt, rt, lx, ly, rx, ry);
        } else {
          Serial.println("[PKT] BAD CHECKSUM");
        }
        pkt_idx = 0;
      }
    }
  }
}
