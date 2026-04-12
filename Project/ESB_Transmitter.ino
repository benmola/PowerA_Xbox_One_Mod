/*
 * ESB_Transmitter.ino — NRF52840 ESB Transmitter
 *
 * Receives 15-byte gamepad packets from the Pico over UART,
 * validates checksum, and transmits the 14-byte payload via ESB
 * to the receiver dongle (NRF52840 #2).
 *
 * Hardware: NRF52840 dev board (Nice!Nano V2.0 compatible)
 * Board in Arduino IDE: "Adafruit Feather nRF52840 Express"
 * USB Stack: TinyUSB (for Serial debug only)
 *
 * Wiring:
 *   P0.08 (RX) ← Pico GP8 (TX, pin 11) — UART gamepad data
 *   P0.06 (TX) → Pico GP9 (RX, pin 12) — future use
 *   GND        → Pico GND
 *   B+/B-      → LiPo battery
 *   RAW        → External 5V (for charging)
 *
 * UART packet from Pico (15 bytes):
 *   Byte 0:    0xAA (header)
 *   Byte 1-13: gamepad data
 *   Byte 14:   checksum (XOR of bytes 1-13)
 *
 * ESB payload (14 bytes): bytes 1-14 of UART packet (data + checksum)
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_esb.h>

//--------------------------------------------------------------------
// Configuration
//--------------------------------------------------------------------
#define UART_BAUD       921600
#define PACKET_HEADER   0xAA
#define PACKET_SIZE     15
#define PAYLOAD_SIZE    14

#define ESB_CHANNEL     2   // 2402 MHz — must match receiver
#define ESB_MAX_RETRIES 5
#define ESB_RETRY_DELAY 500 // microseconds

//--------------------------------------------------------------------
// ESB state
//--------------------------------------------------------------------
static nrf_esb_payload_t tx_payload;
static volatile bool esb_tx_done = true;
static volatile uint32_t esb_tx_fail = 0;
static volatile uint32_t esb_tx_ok = 0;

//--------------------------------------------------------------------
// UART receive buffer
//--------------------------------------------------------------------
static uint8_t uart_buf[PACKET_SIZE];
static uint8_t uart_pos = 0;
static enum { WAIT_HEADER, READ_DATA } uart_state = WAIT_HEADER;

//--------------------------------------------------------------------
// ESB event handler
//--------------------------------------------------------------------
void nrf_esb_event_handler(nrf_esb_evt_t const *p_event) {
    switch (p_event->evt_id) {
        case NRF_ESB_EVENT_TX_SUCCESS:
            esb_tx_ok++;
            esb_tx_done = true;
            break;
        case NRF_ESB_EVENT_TX_FAILED:
            esb_tx_fail++;
            nrf_esb_flush_tx();
            esb_tx_done = true;
            break;
        default:
            break;
    }
}

//--------------------------------------------------------------------
// ESB initialization (PTX mode — transmitter)
//--------------------------------------------------------------------
bool esb_init(void) {
    nrf_esb_config_t config = NRF_ESB_DEFAULT_CONFIG;
    config.protocol            = NRF_ESB_PROTOCOL_ESB_DPL;
    config.mode                = NRF_ESB_MODE_PTX;        // Transmitter
    config.event_handler       = nrf_esb_event_handler;
    config.bitrate             = NRF_ESB_BITRATE_2MBPS;
    config.retransmit_count    = ESB_MAX_RETRIES;
    config.retransmit_delay    = ESB_RETRY_DELAY;
    config.tx_output_power     = NRF_ESB_TX_POWER_0DBM;
    config.payload_length      = PAYLOAD_SIZE;
    config.crc                 = NRF_ESB_CRC_16BIT;

    uint32_t err = nrf_esb_init(&config);
    if (err != NRF_SUCCESS) return false;

    // Set address (must match receiver)
    uint8_t base_addr[4] = {0xE7, 0xE7, 0xE7, 0xE7};
    uint8_t prefix[1] = {0xE7};
    nrf_esb_set_base_addr_0(base_addr);
    nrf_esb_set_prefixes(prefix, 1);
    nrf_esb_set_rf_channel(ESB_CHANNEL);

    // Prepare payload struct
    tx_payload.pipe   = 0;
    tx_payload.length = PAYLOAD_SIZE;
    memset(tx_payload.data, 0, PAYLOAD_SIZE);

    return true;
}

//--------------------------------------------------------------------
// Process one complete UART packet
//--------------------------------------------------------------------
void process_uart_packet(uint8_t *pkt) {
    // pkt[0] = 0xAA header (already consumed)
    // pkt[1..13] = gamepad data
    // pkt[14] = checksum

    // Validate checksum
    uint8_t cs = 0;
    for (int i = 1; i < 14; i++) {
        cs ^= pkt[i];
    }
    if (cs != pkt[14]) {
        // Bad checksum — discard
        return;
    }

    // Copy bytes 1-14 into ESB payload (data + checksum)
    memcpy(tx_payload.data, &pkt[1], PAYLOAD_SIZE);

    // Transmit if previous TX is done
    if (esb_tx_done) {
        esb_tx_done = false;
        nrf_esb_write_payload(&tx_payload);
    }
}

//--------------------------------------------------------------------
// Setup
//--------------------------------------------------------------------
void setup() {
    // LED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // 5 fast blinks = firmware alive
    for (int i = 0; i < 5; i++) {
        digitalWrite(LED_BUILTIN, HIGH); delay(100);
        digitalWrite(LED_BUILTIN, LOW);  delay(100);
    }

    // USB serial for debug
    Serial.begin(115200);

    // UART from Pico (Serial1 = P0.06 TX / P0.08 RX on NRF52840)
    Serial1.begin(UART_BAUD);

    delay(1000);

    Serial.println("==========================================");
    Serial.println("  ESB Transmitter — NRF52840 #1");
    Serial.println("==========================================");
    Serial.print("UART RX @ ");   Serial.print(UART_BAUD);   Serial.println(" baud");
    Serial.print("ESB PTX ch "); Serial.print(ESB_CHANNEL); Serial.println(", 2Mbps");
    Serial.println();

    // Initialize ESB
    if (esb_init()) {
        Serial.println("ESB initialized OK — waiting for UART data...");
    } else {
        Serial.println("ERROR: ESB init failed!");
    }

    Serial.println();
}

//--------------------------------------------------------------------
// Main loop
//--------------------------------------------------------------------
void loop() {
    static uint32_t last_print = 0;
    static uint32_t pkt_count = 0;
    static uint32_t last_blink = 0;
    static bool led_state = false;
    static bool ever_received = false;

    uint32_t now = millis();

    // Slow blink while waiting
    if (!ever_received && (now - last_blink > 500)) {
        last_blink = now;
        led_state = !led_state;
        digitalWrite(LED_BUILTIN, led_state);
    }

    // Read UART bytes from Pico
    while (Serial1.available()) {
        uint8_t b = Serial1.read();

        switch (uart_state) {
            case WAIT_HEADER:
                if (b == PACKET_HEADER) {
                    uart_buf[0] = b;
                    uart_pos = 1;
                    uart_state = READ_DATA;
                }
                break;

            case READ_DATA:
                uart_buf[uart_pos++] = b;
                if (uart_pos >= PACKET_SIZE) {
                    // Complete packet received
                    process_uart_packet(uart_buf);
                    pkt_count++;

                    if (!ever_received) {
                        ever_received = true;
                        digitalWrite(LED_BUILTIN, HIGH);
                        Serial.println("First UART packet received! Transmitting...");
                    }

                    uart_state = WAIT_HEADER;
                    uart_pos = 0;
                }
                break;
        }
    }

    // Debug print every 2 seconds
    if (now - last_print > 2000) {
        last_print = now;
        if (ever_received) {
            Serial.print("UART pkts: "); Serial.print(pkt_count);
            Serial.print("  ESB ok: ");  Serial.print(esb_tx_ok);
            Serial.print("  fail: ");    Serial.println(esb_tx_fail);
        }
    }
}
