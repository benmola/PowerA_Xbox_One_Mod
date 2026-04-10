/*
 * Pico Gamepad Host — Using Ryzee119's tusb_xinput driver
 * 
 * This firmware uses the Pico's NATIVE USB port in HOST mode to read
 * an Xbox One / 360 controller via the tusb_xinput TinyUSB extension.
 * The library handles the proprietary GIP/XInput initialization automatically.
 *
 * HARDWARE SETUP:
 *   - Controller plugs into the Pico's USB-C port (via micro-USB to USB-A 
 *     adapter cable — controller's cable plugs into a USB-A female breakout
 *     wired to the Pico's USB-C port... OR directly into the USB-C port
 *     with an OTG adapter).
 *   - Pico is powered via VSYS pin (3.3V-5V) since the USB port is in host mode.
 *     During prototyping, power VSYS from a separate USB cable or bench supply.
 *   - Debug serial output on UART0: GP4 (TX), GP5 (RX) at 115200 baud.
 *     Connect a USB-to-TTL adapter to GP4 and GND to see printf output.
 *
 * WIRING SUMMARY:
 *   GP4 (pin 6)  → USB-TTL adapter RX (for debug output)
 *   GND (pin 8)  → USB-TTL adapter GND
 *   VSYS (pin 39) → 5V power supply (or second USB cable's 5V)
 *   USB-C port   → Controller (via adapter)
 *
 * SOFTWARE:
 *   - Pico SDK + TinyUSB (host mode) + Ryzee119/tusb_xinput
 *   - All Xbox init/handshake handled by the library
 *   - Callbacks fire on mount, unmount, and report received
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "bsp/board_api.h"
#include "tusb.h"
#include "xinput_host.h"

//--------------------------------------------------------------------
// Configuration
//--------------------------------------------------------------------
#define LED_PIN     25

// UART pins for future NRF52840 communication
#define NRF_UART_ID   uart1
#define NRF_UART_BAUD 115200  // Must match SerialPowerA.ino Serial1 baud
#define NRF_UART_TX   8   // GP8 (pin 11) — UART1 TX
#define NRF_UART_RX   9   // GP9 (pin 12) — UART1 RX

#define PACKET_HEADER 0xAA
#define PACKET_SIZE   15

//--------------------------------------------------------------------
// Gamepad state (latest parsed data, ready for NRF52840)
//--------------------------------------------------------------------
typedef struct {
    uint16_t buttons;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  left_x, left_y;
    int16_t  right_x, right_y;
    bool     connected;
    bool     updated;
} gamepad_state_t;

static gamepad_state_t gamepad = {0};

//--------------------------------------------------------------------
// Helper: send formatted text to NRF52840 via UART1
// (so we can see debug output through the NRF52840 serial bridge)
//--------------------------------------------------------------------
static void nrf_printf(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        uart_write_blocking(NRF_UART_ID, (const uint8_t *)buf, (size_t)len);
    }
}

//--------------------------------------------------------------------
// Register the xinput class driver with TinyUSB
// (Required hook — TinyUSB calls this to discover custom drivers)
//--------------------------------------------------------------------
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

//--------------------------------------------------------------------
// Callback: XInput device mounted (connected and initialized)
//--------------------------------------------------------------------
void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf) {
    printf("\n");
    printf("========================================\n");
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

    // Also notify through NRF52840 bridge
    nrf_printf("\r\n=== CONTROLLER CONNECTED! Type: %s ===\r\n\r\n", type_str);

    gamepad.connected = true;
    gpio_put(LED_PIN, 1);  // Solid LED = connected

    // For Xbox 360 Wireless, we must wait for a connection packet first
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    // Set LEDs
    if (xinput_itf->type == XBOXONE) {
        // Xbox One / GIP controllers need a GIP LED init command
        // (tuh_xinput_set_led is a no-op for Xbox One)
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

//--------------------------------------------------------------------
// Callback: XInput device unmounted (disconnected)
//--------------------------------------------------------------------
void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("\n");
    printf("XINPUT CONTROLLER DISCONNECTED (dev=%d, inst=%d)\n\n", dev_addr, instance);
    nrf_printf("=== CONTROLLER DISCONNECTED ===\r\n");

    gamepad.connected = false;
    gamepad.buttons = 0;
    gamepad.left_trigger = 0;
    gamepad.right_trigger = 0;
    gamepad.left_x = 0;
    gamepad.left_y = 0;
    gamepad.right_x = 0;
    gamepad.right_y = 0;
    gamepad.updated = true;

    gpio_put(LED_PIN, 0);  // LED off = disconnected
}

//--------------------------------------------------------------------
// Callback: XInput report received (button/stick/trigger data)
//--------------------------------------------------------------------
void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                    xinputh_interface_t const *xid_itf, uint16_t len) {
    (void)len;

    if (xid_itf->last_xfer_result != XFER_RESULT_SUCCESS) {
        // Transfer failed — just re-queue and try again
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    // For wireless controllers: check if actually connected
    if (xid_itf->type == XBOX360_WIRELESS) {
        if (!xid_itf->connected) {
            tuh_xinput_receive_report(dev_addr, instance);
            return;
        }
    }

    // Only process if there's new pad data
    if (xid_itf->connected && xid_itf->new_pad_data) {
        const xinput_gamepad_t *p = &xid_itf->pad;

        // Store in our gamepad state struct
        gamepad.buttons       = p->wButtons;
        gamepad.left_trigger  = p->bLeftTrigger;
        gamepad.right_trigger = p->bRightTrigger;
        gamepad.left_x        = p->sThumbLX;
        gamepad.left_y        = p->sThumbLY;
        gamepad.right_x       = p->sThumbRX;
        gamepad.right_y       = p->sThumbRY;
        gamepad.updated       = true;
    }

    // IMPORTANT: Must re-queue to keep receiving reports
    tuh_xinput_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// Send parsed gamepad data over UART to NRF52840 (binary packet)
// Called from main loop when gamepad.updated is true
//--------------------------------------------------------------------
static void uart_send_to_nrf(void) {
    uint8_t pkt[PACKET_SIZE];
    pkt[0]  = PACKET_HEADER;
    pkt[1]  = gamepad.buttons & 0xFF;
    pkt[2]  = (gamepad.buttons >> 8) & 0xFF;
    pkt[3]  = gamepad.left_x & 0xFF;
    pkt[4]  = (gamepad.left_x >> 8) & 0xFF;
    pkt[5]  = gamepad.left_y & 0xFF;
    pkt[6]  = (gamepad.left_y >> 8) & 0xFF;
    pkt[7]  = gamepad.right_x & 0xFF;
    pkt[8]  = (gamepad.right_x >> 8) & 0xFF;
    pkt[9]  = gamepad.right_y & 0xFF;
    pkt[10] = (gamepad.right_y >> 8) & 0xFF;
    pkt[11] = gamepad.left_trigger;
    pkt[12] = gamepad.right_trigger;
    pkt[13] = 0;  // Reserved (dpad is in buttons for XInput)
    uint8_t cs = 0;
    for (int i = 1; i < 14; i++) cs ^= pkt[i];
    pkt[14] = cs;

    uart_write_blocking(NRF_UART_ID, pkt, PACKET_SIZE);
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------
int main(void) {
    // Board init (sets up clocks, pins, etc.)
    board_init();

    // LED setup
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // 5 fast blinks = firmware alive
    for (int i = 0; i < 5; i++) {
        gpio_put(LED_PIN, 1); sleep_ms(100);
        gpio_put(LED_PIN, 0); sleep_ms(100);
    }

    // Init stdio over UART (debug printf on GP4/GP5)
    // Note: pico_enable_stdio_uart is set in CMakeLists.txt
    // Default UART0 pins are GP0/GP1, but we override to GP4/GP5
    // via compile definitions in CMakeLists.txt
    stdio_init_all();

    printf("\n\n");
    printf("==========================================\n");
    printf("  Pico Gamepad Host — tusb_xinput\n");
    printf("==========================================\n");
    printf("USB Host on native USB-C port\n");
    printf("Debug UART on GP4(TX) @ 115200\n");
    printf("NRF52840 UART on GP8(TX)/GP9(RX) @ %d\n", NRF_UART_BAUD);
    printf("Waiting for Xbox controller...\n\n");

    // Init UART1 for NRF52840 communication
    uart_init(NRF_UART_ID, NRF_UART_BAUD);
    gpio_set_function(NRF_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(NRF_UART_RX, GPIO_FUNC_UART);

    // Also send banner to NRF52840 serial bridge
    nrf_printf("\r\n==========================================\r\n");
    nrf_printf("  Pico Gamepad Host — tusb_xinput\r\n");
    nrf_printf("==========================================\r\n");
    nrf_printf("Waiting for Xbox controller...\r\n\r\n");

    // Init TinyUSB Host (rhport 0 = native USB controller)
    tuh_init(0);

    printf("TinyUSB Host initialized. Plug in controller.\n\n");

    // Main loop
    uint32_t last_print = 0;
    uint32_t last_blink = 0;
    bool led_state = false;

    while (true) {
        // Run TinyUSB host task (handles enumeration, transfers, callbacks)
        tuh_task();

        uint32_t now = board_millis();

        // Slow blink while waiting for controller
        if (!gamepad.connected && (now - last_blink > 500)) {
            last_blink = now;
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
        }

        // When new data arrives, print and send to NRF52840
        if (gamepad.updated) {
            gamepad.updated = false;

            // Print human-readable output every 200ms
            if (now - last_print > 200) {
                last_print = now;

                // Line 1: buttons (72 chars — fits in 80-col terminal)
                printf("BTN:%04X A:%d B:%d X:%d Y:%d LB:%d RB:%d "
                       "ST:%d BK:%d GD:%d L3:%d R3:%d D:%d%d%d%d\r\n",
                    gamepad.buttons,
                    (gamepad.buttons & XINPUT_GAMEPAD_A) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_B) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_X) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_Y) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_START) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_BACK) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_GUIDE) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_LEFT_THUMB) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_RIGHT_THUMB) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0);
                // Line 2: triggers + sticks (55 chars)
                printf("  LT:%3d RT:%3d LX:%6d LY:%6d RX:%6d RY:%6d\r\n",
                    gamepad.left_trigger,
                    gamepad.right_trigger,
                    gamepad.left_x, gamepad.left_y,
                    gamepad.right_x, gamepad.right_y);

                // Also send to NRF52840 serial bridge
                nrf_printf("BTN:%04X A:%d B:%d X:%d Y:%d LB:%d RB:%d "
                           "ST:%d BK:%d GD:%d L3:%d R3:%d D:%d%d%d%d\r\n",
                    gamepad.buttons,
                    (gamepad.buttons & XINPUT_GAMEPAD_A) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_B) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_X) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_Y) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_START) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_BACK) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_GUIDE) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_LEFT_THUMB) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_RIGHT_THUMB) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0,
                    (gamepad.buttons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0);
                nrf_printf("  LT:%3d RT:%3d LX:%6d LY:%6d RX:%6d RY:%6d\r\n",
                    gamepad.left_trigger,
                    gamepad.right_trigger,
                    gamepad.left_x, gamepad.left_y,
                    gamepad.right_x, gamepad.right_y);
            }

            // Binary packets disabled during debug — re-enable for production
            // uart_send_to_nrf();
        }
    }

    return 0;
}
