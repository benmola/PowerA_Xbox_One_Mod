/*
 * ESB_Receiver.ino — NRF52840 ESB Receiver + Xbox 360 XInput USB Gamepad
 *
 * Receives gamepad data from NRF52840 #1 (transmitter) via Enhanced ShockBurst
 * and presents itself to the PC as an Xbox 360 controller using a custom
 * TinyUSB XInput device class driver.
 *
 * Windows automatically loads xusb22.sys — no drivers needed.
 * Works with every XInput game, Steam, Game Pass, etc.
 *
 * Hardware: NRF52840 dev board (Nice!Nano V2.0 compatible)
 * Board in Arduino IDE: "Adafruit Feather nRF52840 Express"
 * USB Stack: TinyUSB
 *
 * Radio packet format (14 bytes payload, from transmitter):
 *   Byte 0-1:  buttons (uint16, same layout as XInput wButtons)
 *   Byte 2-3:  left stick X (int16)
 *   Byte 4-5:  left stick Y (int16)
 *   Byte 6-7:  right stick X (int16)
 *   Byte 8-9:  right stick Y (int16)
 *   Byte 10:   left trigger (uint8, 0-255)
 *   Byte 11:   right trigger (uint8, 0-255)
 *   Byte 12:   reserved
 *   Byte 13:   checksum (XOR of bytes 0-12)
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf_esb.h>

// Include the XInput device driver
extern "C" {
#include "xinput_device.h"
}

//--------------------------------------------------------------------
// ESB Configuration
//--------------------------------------------------------------------
#define ESB_CHANNEL     2           // 2402 MHz — avoid WiFi
#define ESB_PIPE_ADDR   {0xE7, 0xE7, 0xE7, 0xE7}  // Must match transmitter

// ESB buffers
static nrf_esb_payload_t rx_payload;
static volatile bool esb_data_ready = false;

//--------------------------------------------------------------------
// XInput report (sent to PC)
//--------------------------------------------------------------------
static xinput_report_t xreport;

//--------------------------------------------------------------------
// Register XInput class driver with TinyUSB
//--------------------------------------------------------------------
extern "C" usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &xinput_device_driver;
}

//--------------------------------------------------------------------
// ESB event handler (called from ISR context)
//--------------------------------------------------------------------
void nrf_esb_event_handler(nrf_esb_evt_t const *p_event) {
    switch (p_event->evt_id) {
        case NRF_ESB_EVENT_RX_RECEIVED:
            // Data received — read from FIFO
            if (nrf_esb_read_rx_payload(&rx_payload) == NRF_SUCCESS) {
                esb_data_ready = true;
            }
            break;
        default:
            break;
    }
}

//--------------------------------------------------------------------
// ESB initialization (PRX mode — receiver)
//--------------------------------------------------------------------
bool esb_init(void) {
    nrf_esb_config_t config = NRF_ESB_DEFAULT_CONFIG;
    config.protocol            = NRF_ESB_PROTOCOL_ESB_DPL;
    config.mode                = NRF_ESB_MODE_PRX;       // Receiver
    config.event_handler       = nrf_esb_event_handler;
    config.bitrate             = NRF_ESB_BITRATE_2MBPS;
    config.crc                 = NRF_ESB_CRC_16BIT;
    config.payload_length      = 14;                     // Match transmitter

    uint32_t err = nrf_esb_init(&config);
    if (err != NRF_SUCCESS) return false;

    // Set address (must match transmitter)
    uint8_t base_addr[4] = ESB_PIPE_ADDR;
    uint8_t prefix[1] = {0xE7};
    nrf_esb_set_base_addr_0(base_addr);
    nrf_esb_set_prefixes(prefix, 1);
    nrf_esb_set_rf_channel(ESB_CHANNEL);

    // Start receiving
    err = nrf_esb_start_rx();
    return (err == NRF_SUCCESS);
}

//--------------------------------------------------------------------
// Parse radio payload → XInput report (zero remapping!)
//--------------------------------------------------------------------
void parse_radio_to_xinput(uint8_t *data, uint8_t len) {
    if (len < 13) return;

    // Validate checksum
    uint8_t cs = 0;
    for (int i = 0; i < 13; i++) cs ^= data[i];
    if (cs != data[13]) return; // Bad packet

    // Direct copy — radio packet layout matches XInput perfectly
    xreport.report_id    = 0x00;
    xreport.report_size  = 0x14; // 20 bytes
    xreport.wButtons     = data[0] | (data[1] << 8);
    xreport.sThumbLX     = (int16_t)(data[2] | (data[3] << 8));
    xreport.sThumbLY     = (int16_t)(data[4] | (data[5] << 8));
    xreport.sThumbRX     = (int16_t)(data[6] | (data[7] << 8));
    xreport.sThumbRY     = (int16_t)(data[8] | (data[9] << 8));
    xreport.bLeftTrigger  = data[10];
    xreport.bRightTrigger = data[11];
    memset(xreport.reserved, 0, sizeof(xreport.reserved));
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

    // CDC serial for debug (composite: XInput + CDC)
    Serial.begin(115200);
    delay(2000); // Wait for USB enumeration

    Serial.println("==========================================");
    Serial.println("  ESB Receiver — Xbox 360 XInput Device");
    Serial.println("==========================================");
    Serial.println("ESB PRX on channel 2, 2Mbps");
    Serial.println("USB: Xbox 360 Controller (045E:028E)");
    Serial.println();

    // Initialize XInput report to neutral
    memset(&xreport, 0, sizeof(xreport));
    xreport.report_id   = 0x00;
    xreport.report_size = 0x14;

    // Initialize ESB
    if (esb_init()) {
        Serial.println("ESB initialized OK — waiting for transmitter...");
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
    static uint32_t last_blink = 0;
    static uint32_t pkt_count = 0;
    static bool led_state = false;
    static bool ever_received = false;

    uint32_t now = millis();

    // Slow blink while waiting for first packet
    if (!ever_received && (now - last_blink > 500)) {
        last_blink = now;
        led_state = !led_state;
        digitalWrite(LED_BUILTIN, led_state);
    }

    // Process received ESB data
    if (esb_data_ready) {
        esb_data_ready = false;
        pkt_count++;

        if (!ever_received) {
            ever_received = true;
            digitalWrite(LED_BUILTIN, HIGH); // Solid = receiving
            Serial.println("First packet received! Controller active.");
        }

        // Parse radio payload into XInput report
        parse_radio_to_xinput(rx_payload.data, rx_payload.length);

        // Send to PC as Xbox 360 controller
        if (xinputd_ready()) {
            xinputd_send_report(&xreport);
        }

        // Debug print every 500ms
        if (now - last_print > 500) {
            last_print = now;
            Serial.print("PKT:");     Serial.print(pkt_count);
            Serial.print(" BTN:");    Serial.print(xreport.wButtons, HEX);
            Serial.print(" LX:");     Serial.print(xreport.sThumbLX);
            Serial.print(" LY:");     Serial.print(xreport.sThumbLY);
            Serial.print(" RX:");     Serial.print(xreport.sThumbRX);
            Serial.print(" RY:");     Serial.print(xreport.sThumbRY);
            Serial.print(" LT:");     Serial.print(xreport.bLeftTrigger);
            Serial.print(" RT:");     Serial.println(xreport.bRightTrigger);
        }
    }

    // Tiny yield
    delay(1);
}
