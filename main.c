#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "bsp/board_api.h"
#include "tusb.h"

// xinput host driver supplied by Ryzee119
#include "xinput_host.h"

// ----------------------------------------------------------------------------
// Register the xinput class driver with TinyUSB
// (Required hook — TinyUSB calls this to discover custom drivers)
// ----------------------------------------------------------------------------
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

// ----------------------------------------------------------------------------
// PIN CONFIGURATION
// ----------------------------------------------------------------------------
#define SPI_PORT spi0
#define PIN_SCK  2
#define PIN_MOSI 3
#define PIN_MISO 4
#define PIN_CE   16
#define PIN_CSN  17
#define PIN_LED  PICO_DEFAULT_LED_PIN

// ----------------------------------------------------------------------------
// NRF24L01+ REGISTERS & COMMANDS
// ----------------------------------------------------------------------------
#define NRF_R_REGISTER    0x00
#define NRF_W_REGISTER    0x20
#define NRF_W_TX_PAYLOAD  0xA0
#define NRF_FLUSH_TX      0xE1
#define NRF_FLUSH_RX      0xE2
#define NRF_NOP           0xFF

#define REG_CONFIG        0x00
#define REG_EN_AA         0x01
#define REG_EN_RXADDR     0x02
#define REG_SETUP_AW      0x03
#define REG_SETUP_RETR    0x04
#define REG_RF_CH         0x05
#define REG_RF_SETUP      0x06
#define REG_STATUS        0x07
#define REG_RX_ADDR_P0    0x0A
#define REG_TX_ADDR       0x10
#define REG_RX_PW_P0      0x11

// ----------------------------------------------------------------------------
// NRF24L01+ HELPERS
// ----------------------------------------------------------------------------
static inline void csn_put(bool state) {
    gpio_put(PIN_CSN, state);
}

static inline void ce_put(bool state) {
    gpio_put(PIN_CE, state);
}

static uint8_t nrf_transfer(uint8_t dat) {
    uint8_t rx;
    spi_write_read_blocking(SPI_PORT, &dat, &rx, 1);
    return rx;
}

static void nrf_write_register(uint8_t reg, uint8_t val) {
    csn_put(0);
    nrf_transfer(NRF_W_REGISTER | (reg & 0x1F));
    nrf_transfer(val);
    csn_put(1);
}

static void nrf_write_register_multi(uint8_t reg, const uint8_t *buf, uint8_t len) {
    csn_put(0);
    nrf_transfer(NRF_W_REGISTER | (reg & 0x1F));
    spi_write_blocking(SPI_PORT, buf, len);
    csn_put(1);
}

static void nrf_cmd(uint8_t cmd) {
    csn_put(0);
    nrf_transfer(cmd);
    csn_put(1);
}

static void nrf_init(void) {
    // 1. SPI Setup
    spi_init(SPI_PORT, 2000000); // 2MHz SPI
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    // 2. CE and CSN setup
    gpio_init(PIN_CE);
    gpio_set_dir(PIN_CE, GPIO_OUT);
    ce_put(0); // Standby

    gpio_init(PIN_CSN);
    gpio_set_dir(PIN_CSN, GPIO_OUT);
    csn_put(1); // Deselect

    sleep_ms(15); // Power on delay

    // 3. Configure per ESB requirements
    // EN_AA = 0 (Auto-ACK disabled)
    nrf_write_register(REG_EN_AA, 0x00);
    
    // SETUP_AW = 3 (5-byte address)
    nrf_write_register(REG_SETUP_AW, 0x03);

    // SETUP_RETR = 0 (No retransmissions)
    nrf_write_register(REG_SETUP_RETR, 0x00);

    // RF_CH = 2 (2402 MHz)
    nrf_write_register(REG_RF_CH, 2);

    // RF_SETUP = 2 Mbps, 0 dBm
    // Bit 3 = 1 (2Mbps), Bits 2:1 = 3 (0dBm) -> 0x0E (14)
    nrf_write_register(REG_RF_SETUP, 0x0E);

    // Address = 0xE7E7E7E7E7
    uint8_t addr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    nrf_write_register_multi(REG_TX_ADDR, addr, 5);
    nrf_write_register_multi(REG_RX_ADDR_P0, addr, 5);

    // RX_PW_P0 = 14 bytes
    nrf_write_register(REG_RX_PW_P0, 14);

    // CONFIG: 
    // Bit 6: MASK_RX_DR (1)
    // Bit 5: MASK_TX_DS (1)
    // Bit 4: MASK_MAX_RT(1)
    // Bit 3: EN_CRC (1)
    // Bit 2: CRCO (1 for 2 bytes / 16-bit)
    // Bit 1: PWR_UP (1)
    // Bit 0: PRIM_RX (0 for PTX)
    // Binary: 0111 1110 = 0x7E
    nrf_write_register(REG_CONFIG, 0x7E);

    sleep_ms(2); // Power up delay
    
    nrf_cmd(NRF_FLUSH_TX);
    nrf_cmd(NRF_FLUSH_RX);
    nrf_write_register(REG_STATUS, 0x70); // Clear interrupts
}

// NRF24L01+ sends payload bytes MSBit-first on air, but NRF52840 RADIO
// (Nrf_2Mbit mode) expects payload bytes LSBit-first.  Reverse each byte
// so the NRF52840 receiver reconstructs the correct values.
static inline uint8_t reverse_bits(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

static void nrf_transmit(const uint8_t *payload, uint8_t len) {
    uint8_t reversed[14];
    for (uint8_t i = 0; i < len; i++) {
        reversed[i] = reverse_bits(payload[i]);
    }

    nrf_cmd(NRF_FLUSH_TX);
    nrf_write_register(REG_STATUS, 0x70); // Clear any pending TX flags

    csn_put(0);
    nrf_transfer(NRF_W_TX_PAYLOAD);
    spi_write_blocking(SPI_PORT, reversed, len);
    csn_put(1);

    // Pulse CE to transmit (>= 10us required)
    ce_put(1);
    busy_wait_us(15);
    ce_put(0);
}

// ----------------------------------------------------------------------------
// STATE & FILTERING
// ----------------------------------------------------------------------------
static xinput_gamepad_t last_pad = {0};
static uint32_t last_tx_time = 0;
static bool controller_connected = false;
static uint32_t last_led_toggle = 0;
static bool led_state = false;

static inline bool passes_deadzone(const xinput_gamepad_t* a, const xinput_gamepad_t* b) {
    if (a->wButtons != b->wButtons) return true;
    
    if (abs((int)a->sThumbLX - (int)b->sThumbLX) > 256) return true;
    if (abs((int)a->sThumbLY - (int)b->sThumbLY) > 256) return true;
    if (abs((int)a->sThumbRX - (int)b->sThumbRX) > 256) return true;
    if (abs((int)a->sThumbRY - (int)b->sThumbRY) > 256) return true;
    
    if (abs((int)a->bLeftTrigger - (int)b->bLeftTrigger) > 8) return true;
    if (abs((int)a->bRightTrigger - (int)b->bRightTrigger) > 8) return true;
    
    return false;
}

static void build_and_send_payload(const xinput_gamepad_t *p_pad) {
    uint8_t payload[14];
    
    // Bytes 0-1: Buttons
    payload[0]  = (uint8_t)(p_pad->wButtons & 0xFF);
    payload[1]  = (uint8_t)((p_pad->wButtons >> 8) & 0xFF);
    
    // Bytes 2-3: Left Stick X
    payload[2]  = (uint8_t)(p_pad->sThumbLX & 0xFF);
    payload[3]  = (uint8_t)((p_pad->sThumbLX >> 8) & 0xFF);
    
    // Bytes 4-5: Left Stick Y
    payload[4]  = (uint8_t)(p_pad->sThumbLY & 0xFF);
    payload[5]  = (uint8_t)((p_pad->sThumbLY >> 8) & 0xFF);
    
    // Bytes 6-7: Right Stick X
    payload[6]  = (uint8_t)(p_pad->sThumbRX & 0xFF);
    payload[7]  = (uint8_t)((p_pad->sThumbRX >> 8) & 0xFF);
    
    // Bytes 8-9: Right Stick Y
    payload[8]  = (uint8_t)(p_pad->sThumbRY & 0xFF);
    payload[9]  = (uint8_t)((p_pad->sThumbRY >> 8) & 0xFF);
    
    // Byte 10: Left Trigger
    payload[10] = p_pad->bLeftTrigger;
    
    // Byte 11: Right Trigger
    payload[11] = p_pad->bRightTrigger;
    
    // Bytes 12-13: Padding (Zeroed)
    payload[12] = 0x00;
    payload[13] = 0x00;
    
    nrf_transmit(payload, 14);
}

// ----------------------------------------------------------------------------
// TINYUSB HOST CALLBACKS
// ----------------------------------------------------------------------------
void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf) {
    printf("\n========================================\n");
    printf("  XINPUT CONTROLLER CONNECTED!\n");
    printf("  dev_addr=%d, instance=%d\n", dev_addr, instance);

    const char *type_str;
    switch (xinput_itf->type) {
        case XBOXONE:          type_str = "Xbox One";          break;
        case XBOX360_WIRELESS: type_str = "Xbox 360 Wireless"; break;
        case XBOX360_WIRED:    type_str = "Xbox 360 Wired";    break;
        case XBOXOG:           type_str = "Xbox OG";           break;
        default:               type_str = "Unknown";           break;
    }
    printf("  Type: %s\n", type_str);
    printf("========================================\n\n");

    controller_connected = true;
    gpio_put(PIN_LED, 1);  // Solid LED = connected

    // For Xbox 360 Wireless, must wait for connection packet
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    // Turn on controller LEDs
    if (xinput_itf->type == XBOXONE) {
        // GIP LED init command for Xbox One controllers
        static const uint8_t gip_led_on[] = {0x0A, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14};
        tuh_xinput_send_report(dev_addr, instance, gip_led_on, sizeof(gip_led_on));
    } else {
        tuh_xinput_set_led(dev_addr, instance, 0, true);
        tuh_xinput_set_led(dev_addr, instance, 1, true);
    }

    // Turn off rumble
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);

    // Start receiving input reports
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("XINPUT UNMOUNTED\n");
    controller_connected = false;
    gpio_put(PIN_LED, 1); // Steady on = no controller
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const* xid_itf, uint16_t len) {
    (void)len;

    if (xid_itf->last_xfer_result != XFER_RESULT_SUCCESS) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    // For wireless controllers: check if actually connected
    if (xid_itf->type == XBOX360_WIRELESS && !xid_itf->connected) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    if (xid_itf->new_pad_data) {
        xinput_gamepad_t const *p_pad = &xid_itf->pad;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool changed = passes_deadzone(&last_pad, p_pad);
        bool heartbeat = (now - last_tx_time) >= 16;

        if (changed || heartbeat) {
            last_pad = *p_pad;
            build_and_send_payload(&last_pad);
            last_tx_time = now;

            // Debug: print button state every 200ms
            static uint32_t last_print = 0;
            if (now - last_print > 200) {
                last_print = now;
                printf("BTN:%04X ", p_pad->wButtons);
                if (p_pad->wButtons & 0x1000) printf("A ");
                if (p_pad->wButtons & 0x2000) printf("B ");
                if (p_pad->wButtons & 0x4000) printf("X ");
                if (p_pad->wButtons & 0x8000) printf("Y ");
                if (p_pad->wButtons & 0x0100) printf("LB ");
                if (p_pad->wButtons & 0x0200) printf("RB ");
                if (p_pad->wButtons & 0x0010) printf("ST ");
                if (p_pad->wButtons & 0x0020) printf("BK ");
                if (p_pad->wButtons & 0x0400) printf("GD ");
                if (p_pad->wButtons & 0x0001) printf("DU ");
                if (p_pad->wButtons & 0x0002) printf("DD ");
                if (p_pad->wButtons & 0x0004) printf("DL ");
                if (p_pad->wButtons & 0x0008) printf("DR ");
                if (p_pad->wButtons & 0x0040) printf("L3 ");
                if (p_pad->wButtons & 0x0080) printf("R3 ");
                printf("LT:%3d RT:%3d LX:%6d LY:%6d RX:%6d RY:%6d\n",
                    p_pad->bLeftTrigger, p_pad->bRightTrigger,
                    p_pad->sThumbLX, p_pad->sThumbLY,
                    p_pad->sThumbRX, p_pad->sThumbRY);
            }
        }
    }

    tuh_xinput_receive_report(dev_addr, instance);
}

// ----------------------------------------------------------------------------
// MAIN
// ----------------------------------------------------------------------------
int main(void) {
    board_init();
    stdio_init_all();
    
    // LED indicator setup
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1); // Steady on = powered, no controller

    // Startup blink pattern: 3 quick blinks to confirm firmware is running
    for (int i = 0; i < 3; i++) {
        gpio_put(PIN_LED, 0); sleep_ms(100);
        gpio_put(PIN_LED, 1); sleep_ms(100);
    }

    printf("RP2040 Xbox Wireless Transmitter starting...\n");
    printf("Waiting for Xbox controller on USB host port...\n");

    // Init TinyUSB Host (rhport 0 = native USB controller)
    tuh_init(0);
    printf("TinyUSB Host initialized. Plug in controller.\n");

    nrf_init();
    
    while (1) {
        tuh_task();
        
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // LED: slow blink (~1Hz) while waiting for controller, solid when connected
        if (!controller_connected && (now - last_led_toggle) >= 500) {
            led_state = !led_state;
            gpio_put(PIN_LED, led_state);
            last_led_toggle = now;
        }

        // Ensure heartbeat fires even if no USB reports are parsed
        if ((now - last_tx_time) >= 16) {
            build_and_send_payload(&last_pad);
            last_tx_time = now;
        }
    }
    
    return 0;
}
