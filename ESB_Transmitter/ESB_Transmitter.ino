#include "Adafruit_TinyUSB.h"
#include "nrf_sdm.h"

// ============================================================
//  ESB Transmitter — NRF52840 #1
//
//  Receives binary gamepad packets from Pico via UART (Serial1)
//  Transmits payload via 2.4GHz radio to ESB Receiver
//
//  UART: Serial1 default pins (D0=RX, D1=TX) @ 115200
//  Radio: 2 Mbps, Channel 2 (2402 MHz), 0 dBm
//  Address: E7:E7:E7:E7:E7 (5-byte)
//  Payload: 14 bytes, CRC16
// ============================================================

// ---- Radio config (must match receiver) ----
#define RADIO_CHANNEL     2               // 2402 MHz
#define PAYLOAD_LEN       14              // bytes
#define RADIO_BASE_ADDR   0xE7E7E7E7UL
#define RADIO_PREFIX_BYTE 0xE7

// ---- UART packet format from Pico ----
#define PKT_HEADER 0xAA
#define PKT_SIZE   15

#define LED_PIN LED_BUILTIN

static uint8_t radio_pkt[PAYLOAD_LEN] __attribute__((aligned(4)));
static uint8_t uart_buf[PKT_SIZE];
static uint8_t uart_idx = 0;

static uint32_t tx_count = 0;
static uint32_t cs_errors = 0;
static uint32_t last_status_ms = 0;

// ---- Radio ----

void radio_init() {
  // Start HFCLK (required for radio)
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  while (!NRF_CLOCK->EVENTS_HFCLKSTARTED);

  NRF_RADIO->POWER = 1;

  // TX power: 0 dBm
  NRF_RADIO->TXPOWER = 0;

  // Frequency: 2400 + RADIO_CHANNEL = 2402 MHz
  NRF_RADIO->FREQUENCY = RADIO_CHANNEL;

  // Mode: 2 Mbps Nordic proprietary
  NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;

  // Packet format: no length field on air (static payload)
  NRF_RADIO->PCNF0 = 0;

  // Payload: 14 bytes static, 4-byte base address
  NRF_RADIO->PCNF1 = (PAYLOAD_LEN << RADIO_PCNF1_STATLEN_Pos) |
                      (PAYLOAD_LEN << RADIO_PCNF1_MAXLEN_Pos)  |
                      (4 << RADIO_PCNF1_BALEN_Pos);

  // Address: E7:E7:E7:E7:E7 (4-byte base + 1-byte prefix)
  NRF_RADIO->BASE0   = RADIO_BASE_ADDR;
  NRF_RADIO->PREFIX0 = RADIO_PREFIX_BYTE;
  NRF_RADIO->TXADDRESS = 0;  // TX on logical address 0

  // CRC: 16-bit CRC-CCITT
  NRF_RADIO->CRCCNF  = RADIO_CRCCNF_LEN_Two;
  NRF_RADIO->CRCPOLY = 0x11021;
  NRF_RADIO->CRCINIT = 0xFFFF;

  // Packet buffer pointer
  NRF_RADIO->PACKETPTR = (uint32_t)radio_pkt;

  // Shortcut: auto-disable radio after TX complete
  NRF_RADIO->SHORTS = RADIO_SHORTS_END_DISABLE_Msk;
}

void radio_send() {
  NRF_RADIO->EVENTS_READY    = 0;
  NRF_RADIO->EVENTS_END      = 0;
  NRF_RADIO->EVENTS_DISABLED = 0;

  NRF_RADIO->TASKS_TXEN = 1;               // Start TX ramp-up
  while (!NRF_RADIO->EVENTS_READY);         // Wait for ramp-up (~130us)

  NRF_RADIO->TASKS_START = 1;               // Start transmitting
  while (!NRF_RADIO->EVENTS_DISABLED);      // Wait for TX + auto-disable
}

// ---- Main ----

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200);
  delay(3000);

  Serial.println("==========================================");
  Serial.println("  NRF52840 ESB Transmitter");
  Serial.println("==========================================");

  // Disable SoftDevice to get direct RADIO access
  sd_softdevice_disable();

  // UART from Pico (default Serial1 pins: D0=RX, D1=TX)
  Serial1.begin(115200);

  // Init radio for TX
  radio_init();

  Serial.println("Radio: 2Mbps, Ch2 (2402MHz), 0dBm");
  Serial.println("Address: E7:E7:E7:E7:E7");
  Serial.printf("Payload: %d bytes, CRC16\r\n", PAYLOAD_LEN);
  Serial.println("UART: Serial1 default pins @ 115200");
  Serial.println("Waiting for Pico binary packets...");
  Serial.println();

  digitalWrite(LED_PIN, LOW);
}

void loop() {
  // Read UART for binary packets from Pico
  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    // Sync to header byte
    if (uart_idx == 0) {
      if (b == PKT_HEADER) {
        uart_buf[0] = b;
        uart_idx = 1;
      } else {
        // Pass through text data to USB serial (debug)
        Serial.write(b);
      }
      continue;
    }

    uart_buf[uart_idx++] = b;

    if (uart_idx >= PKT_SIZE) {
      // Verify checksum (XOR of bytes 1-13)
      uint8_t cs = 0;
      for (int i = 1; i < 14; i++) cs ^= uart_buf[i];

      if (cs == uart_buf[14]) {
        // Copy payload (bytes 1-14) to radio buffer and transmit
        memcpy(radio_pkt, &uart_buf[1], PAYLOAD_LEN);
        radio_send();
        tx_count++;
        digitalToggle(LED_PIN);
      } else {
        cs_errors++;
      }
      uart_idx = 0;
    }
  }

  // Status report every 5 seconds
  uint32_t now = millis();
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    Serial.printf("[TX] uptime=%lus packets=%lu cs_err=%lu\r\n",
                  now / 1000, tx_count, cs_errors);
  }
}
