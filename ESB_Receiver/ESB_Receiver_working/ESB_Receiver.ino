#include "Adafruit_TinyUSB.h"
#include "nrf_sdm.h"

// ============================================================
//  ESB Receiver + USB HID Gamepad — NRF52840 #2
//
//  Receives gamepad data via 2.4GHz radio from ESB Transmitter
//  Presents as USB HID Gamepad to the PC
//  Also keeps USB Serial (CDC) for debug output (composite device)
//
//  Radio: 2 Mbps, Channel 2 (2402 MHz)
//  Address: E7:E7:E7:E7:E7 (5-byte)
//  Payload: 14 bytes, CRC16
// ============================================================

// ---- Radio config (must match transmitter) ----
#define RADIO_CHANNEL     2
#define PAYLOAD_LEN       14
#define RADIO_BASE_ADDR   0xE7E7E7E7UL
#define RADIO_PREFIX_BYTE 0xE7

// ---- XInput button defines (from Pico binary packet) ----
#define BTN_DPAD_UP    0x0001
#define BTN_DPAD_DOWN  0x0002
#define BTN_DPAD_LEFT  0x0004
#define BTN_DPAD_RIGHT 0x0008
#define BTN_START      0x0010
#define BTN_BACK       0x0020
#define BTN_L3         0x0040
#define BTN_R3         0x0080
#define BTN_LB         0x0100
#define BTN_RB         0x0200
#define BTN_GUIDE      0x0400
#define BTN_A          0x1000
#define BTN_B          0x2000
#define BTN_X          0x4000
#define BTN_Y          0x8000

#define LED_PIN LED_BUILTIN

// ---- HID Report Descriptor ----
// Gamepad with 13 buttons, hat switch (D-pad), 4 stick axes (16-bit), 2 triggers (8-bit)
static const uint8_t hid_report_descriptor[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Gamepad)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)

  // 13 Buttons: A B X Y LB RB Back Start Guide L3 R3 + 2 reserved
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x0D,        //   Usage Maximum (13)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x0D,        //   Report Count (13)
  0x81, 0x02,        //   Input (Data, Var, Abs)
  // 3 padding bits to byte-align
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x03,        //   Report Count (3)
  0x81, 0x01,        //   Input (Constant)

  // Hat Switch (D-pad) — 4 bits + 4 padding
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x39,        //   Usage (Hat Switch)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x07,        //   Logical Maximum (7)
  0x35, 0x00,        //   Physical Minimum (0)
  0x46, 0x3B, 0x01,  //   Physical Maximum (315)
  0x65, 0x14,        //   Unit (Degrees)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x42,        //   Input (Data, Var, Abs, Null State)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x01,        //   Input (Constant)

  // Reset units
  0x65, 0x00,        //   Unit (None)
  0x45, 0x00,        //   Physical Maximum (0)

  // 4 Stick axes: X, Y (left), Z, Rz (right) — 16-bit signed
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x09, 0x32,        //   Usage (Z)
  0x09, 0x35,        //   Usage (Rz)
  0x16, 0x00, 0x80,  //   Logical Minimum (-32768)
  0x26, 0xFF, 0x7F,  //   Logical Maximum (32767)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x04,        //   Report Count (4)
  0x81, 0x02,        //   Input (Data, Var, Abs)

  // 2 Trigger axes: Rx, Ry — 8-bit unsigned
  0x09, 0x33,        //   Usage (Rx)
  0x09, 0x34,        //   Usage (Ry)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x02,        //   Input (Data, Var, Abs)

  0xC0               // End Collection
};

// Report: 2 (buttons) + 1 (hat) + 8 (sticks) + 2 (triggers) = 13 bytes
typedef struct __attribute__((packed)) {
  uint16_t buttons;   // bits 0-12 = buttons, 13-15 = padding
  uint8_t  hat;       // bits 0-3 = hat direction, 4-7 = padding
  int16_t  lx;
  int16_t  ly;
  int16_t  rx;
  int16_t  ry;
  uint8_t  lt;
  uint8_t  rt;
} gamepad_report_t;

// ---- USB HID ----
Adafruit_USBD_HID usb_hid;

// ---- Radio buffers ----
static uint8_t radio_pkt[PAYLOAD_LEN] __attribute__((aligned(4)));
static uint8_t local_pkt[PAYLOAD_LEN];

static uint32_t rx_count = 0;
static uint32_t crc_errors = 0;
static uint32_t addr_events = 0;
static uint32_t disabled_events = 0;
static uint32_t last_status_ms = 0;
static uint32_t last_print_ms = 0;

// ---- Button mapping: XInput bits → HID button bits ----
uint16_t map_buttons(uint16_t xb) {
  uint16_t hid = 0;
  if (xb & BTN_A)     hid |= (1 << 0);   // Button 1
  if (xb & BTN_B)     hid |= (1 << 1);   // Button 2
  if (xb & BTN_X)     hid |= (1 << 2);   // Button 3
  if (xb & BTN_Y)     hid |= (1 << 3);   // Button 4
  if (xb & BTN_LB)    hid |= (1 << 4);   // Button 5
  if (xb & BTN_RB)    hid |= (1 << 5);   // Button 6
  if (xb & BTN_BACK)  hid |= (1 << 6);   // Button 7
  if (xb & BTN_START) hid |= (1 << 7);   // Button 8
  if (xb & BTN_GUIDE) hid |= (1 << 8);   // Button 9
  if (xb & BTN_L3)    hid |= (1 << 9);   // Button 10
  if (xb & BTN_R3)    hid |= (1 << 10);  // Button 11
  return hid;
}

// ---- D-pad bits → Hat switch value ----
// 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 0x0F=center
uint8_t dpad_to_hat(uint16_t xb) {
  bool u = xb & BTN_DPAD_UP;
  bool d = xb & BTN_DPAD_DOWN;
  bool l = xb & BTN_DPAD_LEFT;
  bool r = xb & BTN_DPAD_RIGHT;
  if (u && r) return 1;
  if (d && r) return 3;
  if (d && l) return 5;
  if (u && l) return 7;
  if (u)      return 0;
  if (r)      return 2;
  if (d)      return 4;
  if (l)      return 6;
  return 0x0F;  // No direction (null state)
}

// ---- Radio ----

void radio_init() {
  // Disable any residual RADIO IRQ from SoftDevice
  NVIC_DisableIRQ(RADIO_IRQn);
  NVIC_ClearPendingIRQ(RADIO_IRQn);

  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  while (!NRF_CLOCK->EVENTS_HFCLKSTARTED);

  // Force-disable radio before reconfiguring
  NRF_RADIO->TASKS_DISABLE = 1;
  while (NRF_RADIO->STATE != 0);  // Wait for Disabled state

  NRF_RADIO->POWER = 1;

  NRF_RADIO->TXPOWER  = 0;
  NRF_RADIO->FREQUENCY = RADIO_CHANNEL;
  NRF_RADIO->MODE      = RADIO_MODE_MODE_Nrf_2Mbit;

  NRF_RADIO->PCNF0 = 0;

  NRF_RADIO->PCNF1 = (PAYLOAD_LEN << RADIO_PCNF1_STATLEN_Pos) |
                      (PAYLOAD_LEN << RADIO_PCNF1_MAXLEN_Pos)  |
                      (4 << RADIO_PCNF1_BALEN_Pos);

  NRF_RADIO->BASE0      = RADIO_BASE_ADDR;
  NRF_RADIO->PREFIX0    = RADIO_PREFIX_BYTE;
  NRF_RADIO->RXADDRESSES = 1;

  NRF_RADIO->CRCCNF  = RADIO_CRCCNF_LEN_Two;
  NRF_RADIO->CRCPOLY = 0x11021;
  NRF_RADIO->CRCINIT = 0xFFFF;

  NRF_RADIO->PACKETPTR = (uint32_t)radio_pkt;

  NRF_RADIO->SHORTS = RADIO_SHORTS_END_START_Msk;
}

void radio_start_rx() {
  NRF_RADIO->EVENTS_READY = 0;
  NRF_RADIO->EVENTS_END   = 0;

  NRF_RADIO->TASKS_RXEN = 1;
  while (!NRF_RADIO->EVENTS_READY);

  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->TASKS_START = 1;
}

// ---- Main ----

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // USB HID must be configured BEFORE Serial.begin()
  usb_hid.setPollInterval(8);   // 8ms = 125 Hz polling
  usb_hid.setReportDescriptor(hid_report_descriptor, sizeof(hid_report_descriptor));
  usb_hid.setStringDescriptor("PowerA Wireless Gamepad");
  usb_hid.begin();

  Serial.begin(115200);
  delay(3000);

  Serial.println("==========================================");
  Serial.println("  NRF52840 ESB Receiver + USB HID Gamepad");
  Serial.println("==========================================");

  // Disable SoftDevice to get direct RADIO access
  uint32_t sd_err = sd_softdevice_disable();
  Serial.printf("sd_softdevice_disable() = %lu\r\n", sd_err);

  radio_init();
  radio_start_rx();

  Serial.printf("Radio: 2Mbps, Ch2 (2402MHz), STATE=%lu\r\n", NRF_RADIO->STATE);
  Serial.printf("HID report: %d bytes, poll 8ms\r\n", (int)sizeof(gamepad_report_t));
  Serial.println("Listening + USB Gamepad active...");
  Serial.println();

  // Wait for USB to be ready
  while (!USBDevice.mounted()) delay(1);

  digitalWrite(LED_PIN, LOW);
}

void loop() {
  // Track ADDRESS events (fires when on-air address matches)
  if (NRF_RADIO->EVENTS_ADDRESS) {
    NRF_RADIO->EVENTS_ADDRESS = 0;
    addr_events++;
  }
  // Track unexpected DISABLED events
  if (NRF_RADIO->EVENTS_DISABLED) {
    NRF_RADIO->EVENTS_DISABLED = 0;
    disabled_events++;
    // Restart RX if radio was disabled unexpectedly
    if (NRF_RADIO->STATE == 0) {
      radio_start_rx();
    }
  }

  if (NRF_RADIO->EVENTS_END) {
    NRF_RADIO->EVENTS_END = 0;

    memcpy(local_pkt, radio_pkt, PAYLOAD_LEN);
    bool crc_ok = (NRF_RADIO->CRCSTATUS == 1);

    if (crc_ok) {
      rx_count++;
      digitalToggle(LED_PIN);

      // Decode radio payload
      uint16_t buttons = local_pkt[0] | (local_pkt[1] << 8);
      int16_t  lx = (int16_t)(local_pkt[2]  | (local_pkt[3]  << 8));
      int16_t  ly = (int16_t)(local_pkt[4]  | (local_pkt[5]  << 8));
      int16_t  rx = (int16_t)(local_pkt[6]  | (local_pkt[7]  << 8));
      int16_t  ry = (int16_t)(local_pkt[8]  | (local_pkt[9]  << 8));
      uint8_t  lt = local_pkt[10];
      uint8_t  rt = local_pkt[11];

      // ---- Send USB HID Gamepad report ----
      if (usb_hid.ready()) {
        gamepad_report_t report;
        report.buttons = map_buttons(buttons);
        report.hat     = dpad_to_hat(buttons);
        report.lx      = lx;
        report.ly      = ly;
        report.rx      = rx;
        report.ry      = ry;
        report.lt      = lt;
        report.rt      = rt;
        usb_hid.sendReport(1, &report, sizeof(report));
      }

      // ---- Serial debug (rate-limited) ----
      uint32_t now = millis();
      if (now - last_print_ms >= 500) {
        last_print_ms = now;
        Serial.printf("BTN:%04X hat:%d LX:%6d LY:%6d RX:%6d RY:%6d LT:%3d RT:%3d\r\n",
          buttons, dpad_to_hat(buttons), lx, ly, rx, ry, lt, rt);
      }
    } else {
      crc_errors++;
    }
  }

  // Status every 5 seconds
  uint32_t now = millis();
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    Serial.printf("[RX] up=%lus pkt=%lu crc_e=%lu addr=%lu dis=%lu st=%lu\r\n",
                  now / 1000, rx_count, crc_errors, addr_events, disabled_events, NRF_RADIO->STATE);
    Serial.printf("  FREQ=%lu BASE0=%08lX PFX=%02lX RXADDR=%lu HFCLK=%lu\r\n",
                  NRF_RADIO->FREQUENCY, NRF_RADIO->BASE0,
                  NRF_RADIO->PREFIX0 & 0xFF, NRF_RADIO->RXADDRESSES,
                  NRF_RADIO->PCNF1);
  }
}
