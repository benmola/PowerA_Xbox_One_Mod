// ============================================================
//  ESB_Receiver_XInput_Xbox.ino
//  NRF52840 — ESB Radio Receiver + USB XInput (Xbox 360 Controller)
//
//  Receives gamepad state via 2.4GHz ESB radio and presents itself
//  to Windows as a wired Xbox 360 controller using XInput protocol.
//  Windows binds xusb22.sys natively — NO authentication chip needed.
//
//  Why Xbox 360 instead of Xbox One?
//    Xbox One controllers use the GIP protocol which requires a
//    hardware cryptographic authentication chip.  Without this chip,
//    Windows never fully activates the controller.  Xbox 360 XInput
//    has no such requirement — plug in and send reports.
//
//  Radio (unchanged from transmitter):
//    2 Mbps, Channel 2 (2402 MHz), CRC16
//    Address: E7:E7:E7:E7:E7  |  Payload: 14 bytes
//
//  USB:
//    VID 0x045E  PID 0x028E  (Microsoft / Xbox 360 Controller)
//    Interface Class 0xFF  SubClass 0x5D  Protocol 0x01
//    Interrupt endpoints (patched from Bulk via descriptor override)
//    XInput reports written via tud_vendor_write()
//
// ============================================================
//  REQUIRED LIBRARY PATCH (same as GIP version):
//  Uses the Adafruit_USBD_WebUSB vendor class but overrides
//  getInterfaceDescriptor() to patch SubClass/Protocol and
//  endpoint types from Bulk to Interrupt.
// ============================================================

#include "Adafruit_TinyUSB.h"
#include "arduino/webusb/Adafruit_USBD_WebUSB.h"
#include "nrf_sdm.h"

// ============================================================
//  XInput USB Interface — full Xbox 360 descriptor override
//
//  A real Xbox 360 controller has a 39-byte interface block:
//    Interface Descriptor        (9 bytes)  FF/5D/01
//    Vendor XInput Descriptor   (16 bytes)  type 0x21
//    Endpoint IN Descriptor      (7 bytes)  Interrupt
//    Endpoint OUT Descriptor     (7 bytes)  Interrupt
//
//  The 16-byte vendor descriptor is CRITICAL — without it
//  xusb22.sys recognises the device (VID/PID match) but
//  never creates the game controller input device node.
//  That is why the previous version showed in Device Manager
//  but Game Controllers (joy.cpl) was empty.
// ============================================================
#define XBOX360_DESC_LEN 39

class XInput_Vendor_Device : public Adafruit_USBD_WebUSB {
public:
  XInput_Vendor_Device() : Adafruit_USBD_WebUSB(NULL) {}

  uint16_t getInterfaceDescriptor(uint8_t itfnum, uint8_t *buf,
                                  uint16_t bufsize) override {
    // ---- Length query (buf == NULL) ----
    if (!buf) return XBOX360_DESC_LEN;

    // ---- Build descriptor ----
    // First, call base class to a temp buffer so that TinyUSB's
    // internal endpoint/interface bookkeeping runs normally.
    uint8_t temp[64];
    uint16_t base = Adafruit_USBD_WebUSB::getInterfaceDescriptor(
        itfnum, temp, sizeof(temp));
    if (base < 23 || bufsize < XBOX360_DESC_LEN) return 0;

    // Extract the endpoint addresses that TinyUSB allocated
    uint8_t ep_out = temp[11];   // e.g. 0x01
    uint8_t ep_in  = temp[18];   // e.g. 0x81

    // Build the complete Xbox 360 XInput descriptor (39 bytes)
    const uint8_t desc[XBOX360_DESC_LEN] = {
      // ---- Interface Descriptor (9 bytes) ----
      0x09, 0x04,
      (uint8_t)itfnum, // bInterfaceNumber
      0x00,            // bAlternateSetting
      0x02,            // bNumEndpoints
      0xFF,            // bInterfaceClass:    Vendor
      0x5D,            // bInterfaceSubClass: XInput
      0x01,            // bInterfaceProtocol: Gamepad
      0x00,            // iInterface

      // ---- Vendor-Specific XInput Descriptor (16 bytes) ----
      //  This tells xusb22.sys about the input pipeline:
      //  report size (0x14 = 20), endpoint addresses, etc.
      0x10,            // bLength = 16
      0x21,            // bDescriptorType (vendor/HID-like)
      0x10, 0x01,      // bcdXInput (1.10)
      0x01,            // bSubType (gamepad)
      0x25,            // bFlags — IN endpoint capabilities
      ep_in,           // bEndpointAddressIN
      0x14,            // bMaxInputReportSize (20 bytes)
      0x00, 0x00, 0x00, 0x00, // reserved
      0x13,            // bFlags — OUT endpoint capabilities
      ep_out,          // bEndpointAddressOUT
      0x08,            // bMaxOutputReportSize (8 bytes = rumble)
      0x00,            // reserved

      // ---- Endpoint IN Descriptor (7 bytes) ----
      0x07, 0x05,
      ep_in,           // bEndpointAddress (IN)
      0x03,            // bmAttributes: Interrupt
      0x20, 0x00,      // wMaxPacketSize: 32
      0x04,            // bInterval: 4ms

      // ---- Endpoint OUT Descriptor (7 bytes) ----
      0x07, 0x05,
      ep_out,          // bEndpointAddress (OUT)
      0x03,            // bmAttributes: Interrupt
      0x20, 0x00,      // wMaxPacketSize: 32
      0x08,            // bInterval: 8ms
    };

    memcpy(buf, desc, XBOX360_DESC_LEN);
    return XBOX360_DESC_LEN;
  }
};

// Route all Serial.xxx() to hardware UART (USB is XInput, not CDC).
// Connect a USB-to-TTL adapter: TX→board RX, RX→board TX, GND→GND.
#define Serial Serial1

// ============================================================
//  Radio config — must match ESB_Transmitter exactly
// ============================================================
#define RADIO_CHANNEL     2
#define PAYLOAD_LEN       14
#define RADIO_BASE_ADDR   0xE7E7E7E7UL
#define RADIO_PREFIX_BYTE 0xE7

// Rumble return path: Receiver → Pico (separate address)
#define RUMBLE_BASE_ADDR   0xD2D2D2D2UL
#define RUMBLE_PREFIX_BYTE 0xD2

#define LED_PIN LED_BUILTIN

// Report send timing
#define XINPUT_REPORT_INTERVAL_MS  4   // minimum ms between sends
#define XINPUT_HEARTBEAT_MS        8   // resend even when idle

// ============================================================
//  XInput button bitmasks (from transmitter's radio packet)
//
//  The transmitter already sends buttons in XInput bit layout!
//  Low byte (bits 0–7):  DPAD_UDLR | START | BACK | L3 | R3
//  High byte (bits 8–15): LB | RB | GUIDE | — | A | B | X | Y
//
//  This maps DIRECTLY to Xbox 360 report bytes 2–3.
//  No remapping needed — just split into two bytes.
// ============================================================
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

// ============================================================
//  XInput 20-byte report struct (Xbox 360 wired controller)
//
//  Byte  0:     0x00  (report type = input)
//  Byte  1:     0x14  (report size = 20)
//  Byte  2:     buttons low  (DPAD | START | BACK | L3 | R3)
//  Byte  3:     buttons high (LB | RB | GUIDE | — | A | B | X | Y)
//  Byte  4:     left trigger  (0–255)
//  Byte  5:     right trigger (0–255)
//  Bytes 6–7:   left stick X  (int16 LE, −32768 .. +32767)
//  Bytes 8–9:   left stick Y  (int16 LE)
//  Bytes 10–11: right stick X (int16 LE)
//  Bytes 12–13: right stick Y (int16 LE)
//  Bytes 14–19: reserved (zeros)
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  report_type;    // always 0x00
  uint8_t  report_size;    // always 0x14 (20)
  uint8_t  buttons_lo;     // DPAD + START + BACK + L3 + R3
  uint8_t  buttons_hi;     // LB + RB + GUIDE + A + B + X + Y
  uint8_t  lt;              // left trigger 0–255
  uint8_t  rt;              // right trigger 0–255
  int16_t  lx;              // left stick X
  int16_t  ly;              // left stick Y
  int16_t  rx;              // right stick X
  int16_t  ry;              // right stick Y
  uint8_t  reserved[6];     // must be zero
} xinput_report_t;

// Compile-time size check
static_assert(sizeof(xinput_report_t) == 20,
              "xinput_report_t must be exactly 20 bytes");

// ============================================================
//  USB object
// ============================================================
XInput_Vendor_Device usb_vendor;

// ============================================================
//  Weak-symbol overrides  (same technique as GIP version)
// ============================================================

// Suppress WebUSB BOS descriptor so Windows doesn't try WinUSB
extern "C" const uint8_t *tud_descriptor_bos_cb(void) { return NULL; }

// OUT endpoint callback — fires when host sends rumble/LED data
extern "C" void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer,
                                 uint32_t bufsize) {
  (void)idx;
  (void)buffer;
  (void)bufsize;
  // Data is drained in loop() via tud_vendor_read()
}

// EP0 vendor control requests — ACK instead of STALL
extern "C" bool
tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                           tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP)
    return true;

  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
    if (request->wLength == 0) {
      Serial.printf("[EP0] vendor SET req=0x%02X — ACK\r\n",
                    request->bRequest);
      return tud_control_status(rhport, request);
    } else {
      Serial.printf("[EP0] vendor GET req=0x%02X len=%u — zero reply\r\n",
                    request->bRequest, request->wLength);
      static uint8_t dummy[64] = {0};
      return tud_control_xfer(
          rhport, request, dummy,
          (uint16_t)tu_min16(request->wLength, sizeof(dummy)));
    }
  }
  return false;
}

// ============================================================
//  Radio buffers and counters
// ============================================================
static uint8_t  radio_pkt[PAYLOAD_LEN] __attribute__((aligned(4)));
static uint8_t  local_pkt[PAYLOAD_LEN];

static uint32_t rx_count       = 0;
static uint32_t crc_errors     = 0;
static uint32_t last_status_ms = 0;
static uint32_t last_print_ms  = 0;

// Rumble state (set by USB task, consumed by radio task)
static uint8_t  rumble_payload[PAYLOAD_LEN] __attribute__((aligned(4)));
static volatile bool rumble_pending = false;
static uint32_t rumble_tx_count = 0;

// ============================================================
//  Input cache  (decoupled from radio rate)
// ============================================================
static uint16_t c_buttons = 0;
static int16_t  c_lx = 0, c_ly = 0, c_rx = 0, c_ry = 0;
static uint8_t  c_lt = 0, c_rt = 0;

static uint32_t last_send_ms      = 0;
static uint32_t last_heartbeat_ms = 0;

// Shadow state for send-on-change detection.
// Initialised to impossible values so the very first packet sends.
static uint16_t last_buttons = 0xFFFF;
static int16_t  last_lx = 1, last_ly = 1, last_rx = 1, last_ry = 1;
static uint8_t  last_lt = 0xFF, last_rt = 0xFF;

// ============================================================
//  Radio: hardware init
// ============================================================
void radio_init() {
  NVIC_DisableIRQ(RADIO_IRQn);
  NVIC_ClearPendingIRQ(RADIO_IRQn);

  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  while (!NRF_CLOCK->EVENTS_HFCLKSTARTED)
    ;

  NRF_RADIO->TASKS_DISABLE = 1;
  while (NRF_RADIO->STATE != 0)
    ;

  NRF_RADIO->POWER     = 1;
  NRF_RADIO->TXPOWER   = 0;
  NRF_RADIO->FREQUENCY = RADIO_CHANNEL;
  NRF_RADIO->MODE      = RADIO_MODE_MODE_Nrf_2Mbit;

  NRF_RADIO->PCNF0 = 0;
  NRF_RADIO->PCNF1 = (PAYLOAD_LEN << RADIO_PCNF1_STATLEN_Pos) |
                      (PAYLOAD_LEN << RADIO_PCNF1_MAXLEN_Pos) |
                      (4 << RADIO_PCNF1_BALEN_Pos);

  NRF_RADIO->BASE0       = RADIO_BASE_ADDR;
  NRF_RADIO->PREFIX0     = RADIO_PREFIX_BYTE;
  NRF_RADIO->RXADDRESSES = 1;

  NRF_RADIO->CRCCNF  = RADIO_CRCCNF_LEN_Two;
  NRF_RADIO->CRCPOLY = 0x11021;
  NRF_RADIO->CRCINIT = 0xFFFF;

  NRF_RADIO->PACKETPTR = (uint32_t)radio_pkt;
  NRF_RADIO->SHORTS    = RADIO_SHORTS_END_START_Msk;
}

// ============================================================
//  Radio: start RX
// ============================================================
void radio_start_rx() {
  NRF_RADIO->EVENTS_READY = 0;
  NRF_RADIO->EVENTS_END   = 0;
  NRF_RADIO->TASKS_RXEN   = 1;
  while (!NRF_RADIO->EVENTS_READY)
    ;
  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->TASKS_START = 1;
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // --- USB init: single vendor interface as Xbox 360 controller ---
  usb_vendor.begin();
  TinyUSBDevice.clearConfiguration();

  TinyUSBDevice.setID(0x045E, 0x028E);   // Microsoft Xbox 360 Controller
  TinyUSBDevice.setManufacturerDescriptor("\xA9Microsoft Corporation");
  TinyUSBDevice.setProductDescriptor("Controller");
  TinyUSBDevice.setVersion(0x0200);

  TinyUSBDevice.addInterface(usb_vendor);

  // Force re-enumeration
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  // UART debug output (USB port is XInput, no CDC serial available)
  // Nice!Nano pins: TX = P0.06 (D11), RX = P0.22 (D30)
  Serial1.setPins(30, 11);   // setPins(RX_pin, TX_pin)
  Serial1.begin(115200);
  delay(3000);

  Serial.println("============================================");
  Serial.println("  ESB_Receiver_XInput — NRF52840");
  Serial.println("  USB: Xbox 360 Controller  VID 0x045E  PID 0x028E");
  Serial.println("============================================");

  // Dump device descriptor
  {
    extern uint8_t const *tud_descriptor_device_cb(void);
    const uint8_t *dd = tud_descriptor_device_cb();
    uint16_t vid = (uint16_t)(dd[8] | (dd[9] << 8));
    uint16_t pid = (uint16_t)(dd[10] | (dd[11] << 8));
    Serial.printf("  VID=0x%04X PID=0x%04X  DevClass=0x%02X SubClass=0x%02X "
                  "Proto=0x%02X\r\n",
                  vid, pid, dd[4], dd[5], dd[6]);
    if (vid != 0x045E || pid != 0x028E) {
      Serial.println("  *** WARNING: VID/PID wrong! ***");
    }
  }

  // Dump interface descriptor to verify XInput patch
  //
  // Expected 39-byte layout (config header at cfg+0, interface at cfg+9):
  //   [0-8]   Interface Descriptor    (9 bytes)
  //   [9-24]  Vendor XInput Desc      (16 bytes) ← CRITICAL
  //   [25-31] Endpoint IN Descriptor  (7 bytes)
  //   [32-38] Endpoint OUT Descriptor (7 bytes)
  {
    extern uint8_t const *tud_descriptor_configuration_cb(uint8_t index);
    const uint8_t *cfg = tud_descriptor_configuration_cb(0);
    const uint8_t *d = cfg + 9;  // skip config header
    uint16_t cfg_total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    uint16_t d_len = (cfg_total > 9) ? (cfg_total - 9) : 0;

    Serial.printf("  Config total=%u  NumInterfaces=%d\r\n",
                  cfg_total, cfg[4]);
    Serial.printf("  Raw descriptor (%u bytes):\r\n", d_len);
    // Print all bytes in rows of 16
    for (uint16_t i = 0; i < d_len && i < 48; i++) {
      Serial.printf(" %02X", d[i]);
      if ((i % 16) == 15) Serial.println();
    }
    Serial.println();

    if (d_len >= XBOX360_DESC_LEN) {
      // Interface Descriptor (bytes 0-8)
      Serial.printf("  Itf#=%d  Class=0x%02X SubClass=0x%02X Proto=0x%02X\r\n",
                    d[2], d[5], d[6], d[7]);

      // Vendor XInput Descriptor (bytes 9-24)
      bool has_xinput = (d[9] == 0x10 && d[10] == 0x21);
      Serial.printf("  Vendor XInput desc: %s (type=0x%02X len=%d)\r\n",
                    has_xinput ? "PRESENT" : "MISSING!", d[10], d[9]);
      if (has_xinput) {
        Serial.printf("    EP_IN=0x%02X  InputReportSize=%d\r\n",
                      d[15], d[16]);
        Serial.printf("    EP_OUT=0x%02X OutputReportSize=%d\r\n",
                      d[22], d[23]);
      }

      // Endpoint IN (bytes 25-31)
      Serial.printf("  EP_IN =0x%02X (attr=%02X pkt=%d int=%dms)\r\n",
                    d[27], d[28], d[29] | (d[30] << 8), d[31]);
      // Endpoint OUT (bytes 32-38)
      Serial.printf("  EP_OUT=0x%02X (attr=%02X pkt=%d int=%dms)\r\n",
                    d[34], d[35], d[36] | (d[37] << 8), d[38]);

      bool ok = (d[6] == 0x5D && d[7] == 0x01 &&
                 has_xinput &&
                 d[28] == 0x03 && d[35] == 0x03);
      Serial.printf("  XInput patch: %s\r\n",
                    ok ? "OK (SubClass=5D, VendorDesc present, Interrupt EPs)"
                       : "FAILED!");
    } else {
      Serial.printf("  ERROR: descriptor too short (%u < %d)\r\n",
                    d_len, XBOX360_DESC_LEN);
    }
  }

  Serial.printf("xinput_report_t = %d bytes (must be 20)\r\n",
                (int)sizeof(xinput_report_t));

  // Disable SoftDevice for direct radio access
  uint32_t sd_err = sd_softdevice_disable();
  Serial.printf("sd_softdevice_disable() = %lu\r\n", sd_err);

  // Init radio
  radio_init();
  radio_start_rx();
  Serial.printf("Radio: 2Mbps Ch%d, STATE=%lu\r\n",
                RADIO_CHANNEL, NRF_RADIO->STATE);

  // Wait for USB host to mount
  while (!USBDevice.mounted())
    delay(1);
  Serial.println("USB mounted — XInput active, sending reports.");

  digitalWrite(LED_PIN, LOW);
}

// ============================================================
//  loop()
// ============================================================
void loop() {

  // ──────────────────────────────────────────────────────────
  // 1. Drain incoming USB data (rumble / LED commands)
  //    Xbox 360 rumble: 00 08 00 LL RR 00 00 00
  //    Xbox 360 LED:    01 03 PP
  //    We read and discard — keeps the OUT endpoint flowing.
  // ──────────────────────────────────────────────────────────
  while (tud_vendor_available()) {
    uint8_t buf[64];
    uint32_t len = tud_vendor_read(buf, sizeof(buf));
    if (len < 1)
      break;
    // Capture rumble commands for radio relay to controller
    if (len >= 5 && buf[0] == 0x00 && buf[1] == 0x08) {
      memset(rumble_payload, 0, PAYLOAD_LEN);
      rumble_payload[0] = buf[3]; // Big motor (left)
      rumble_payload[1] = buf[4]; // Small motor (right)
      rumble_pending = true;
      static bool rumble_logged = false;
      if (!rumble_logged) {
        rumble_logged = true;
        Serial.printf("[USB] Rumble: big=%d small=%d\r\n", buf[3], buf[4]);
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // 2. Radio event handling
  // ──────────────────────────────────────────────────────────
  if (NRF_RADIO->EVENTS_DISABLED) {
    NRF_RADIO->EVENTS_DISABLED = 0;
    if (NRF_RADIO->STATE == 0)
      radio_start_rx();
  }

  // ──────────────────────────────────────────────────────────
  // 3. Receive radio packet → update input cache
  // ──────────────────────────────────────────────────────────
  if (NRF_RADIO->EVENTS_END) {
    NRF_RADIO->EVENTS_END = 0;
    memcpy(local_pkt, radio_pkt, PAYLOAD_LEN);

    if (NRF_RADIO->CRCSTATUS == 1) {
      rx_count++;
      digitalToggle(LED_PIN);

      // Decode 14-byte ESB payload into input cache
      c_buttons = (uint16_t)(local_pkt[0] | (local_pkt[1] << 8));
      c_lx = (int16_t)(local_pkt[2] | (local_pkt[3] << 8));
      c_ly = (int16_t)(local_pkt[4] | (local_pkt[5] << 8));
      c_rx = (int16_t)(local_pkt[6] | (local_pkt[7] << 8));
      c_ry = (int16_t)(local_pkt[8] | (local_pkt[9] << 8));
      c_lt = local_pkt[10];
      c_rt = local_pkt[11];

      // Rate-limited serial debug (2 Hz)
      uint32_t now = millis();
      if (now - last_print_ms >= 500) {
        last_print_ms = now;
        Serial.printf("BTN:%04X LX:%6d LY:%6d RX:%6d RY:%6d LT:%3d RT:%3d\r\n",
                      c_buttons, c_lx, c_ly, c_rx, c_ry, c_lt, c_rt);
      }

      // ---- Synchronized rumble TX ----
      // Transmit rumble immediately after receiving a gamepad packet.
      // The Pico enters RX mode right after its TX, so this is the
      // only moment the Pico's 500µs listening window is open.
      if (rumble_pending) {
        rumble_pending = false;

        // Stop continuous RX
        NRF_RADIO->SHORTS = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->STATE != 0)
          ;

        // Swap to rumble address (D2) and point to rumble buffer
        NRF_RADIO->BASE0      = RUMBLE_BASE_ADDR;
        NRF_RADIO->PREFIX0    = RUMBLE_PREFIX_BYTE;
        NRF_RADIO->TXADDRESS  = 0;
        NRF_RADIO->PACKETPTR  = (uint32_t)rumble_payload;
        NRF_RADIO->SHORTS     = RADIO_SHORTS_END_DISABLE_Msk;

        // 3x redundancy blast (~400µs total, fits in Pico's 500µs window)
        for (int i = 0; i < 3; i++) {
          NRF_RADIO->EVENTS_READY    = 0;
          NRF_RADIO->EVENTS_END      = 0;
          NRF_RADIO->EVENTS_DISABLED = 0;
          NRF_RADIO->TASKS_TXEN      = 1;
          while (!NRF_RADIO->EVENTS_READY)
            ;
          NRF_RADIO->TASKS_START = 1;
          while (!NRF_RADIO->EVENTS_DISABLED)
            ;
        }
        rumble_tx_count++;

        // Restore gamepad RX address (E7) and restart RX
        NRF_RADIO->BASE0      = RADIO_BASE_ADDR;
        NRF_RADIO->PREFIX0    = RADIO_PREFIX_BYTE;
        NRF_RADIO->RXADDRESSES = 1;
        NRF_RADIO->PACKETPTR  = (uint32_t)radio_pkt;
        NRF_RADIO->SHORTS     = RADIO_SHORTS_END_START_Msk;
        radio_start_rx();
      }
    } else {
      crc_errors++;
    }
  }

  // ──────────────────────────────────────────────────────────
  // 4. Send XInput report to USB
  //
  //    No handshake needed!  Xbox 360 XInput protocol:
  //    just keep the IN endpoint fed with 20-byte reports.
  //    The xusb22.sys driver reads them on every 4ms poll.
  //
  //    Send on change + periodic heartbeat to stay alive.
  // ──────────────────────────────────────────────────────────
  if (tud_vendor_mounted()) {
    uint32_t now = millis();
    if (now - last_send_ms >= XINPUT_REPORT_INTERVAL_MS) {
      last_send_ms = now;   // always advance — prevents spin

      bool changed =
          (c_buttons != last_buttons || c_lx != last_lx || c_ly != last_ly ||
           c_rx != last_rx || c_ry != last_ry || c_lt != last_lt ||
           c_rt != last_rt);
      bool heartbeat = (now - last_heartbeat_ms >= XINPUT_HEARTBEAT_MS);

      if (changed || heartbeat) {
        xinput_report_t report;
        memset(&report, 0, sizeof(report));
        report.report_type = 0x00;
        report.report_size = 0x14;
        // Direct mapping — transmitter format IS XInput byte layout
        report.buttons_lo = (uint8_t)(c_buttons & 0xFF);
        report.buttons_hi = (uint8_t)((c_buttons >> 8) & 0xFF);
        report.lt = c_lt;
        report.rt = c_rt;
        report.lx = c_lx;
        report.ly = c_ly;
        report.rx = c_rx;
        report.ry = c_ry;

        if (tud_vendor_write_available() >= sizeof(report)) {
          uint32_t written =
              tud_vendor_write((const uint8_t *)&report, sizeof(report));
          if (written == sizeof(report)) {
            tud_vendor_flush();
            // Update shadow state (only on success)
            last_buttons = c_buttons;
            last_lx = c_lx;
            last_ly = c_ly;
            last_rx = c_rx;
            last_ry = c_ry;
            last_lt = c_lt;
            last_rt = c_rt;
            last_heartbeat_ms = now;

            // Log first report (one-time)
            static bool first = false;
            if (!first) {
              first = true;
              Serial.println("[XInput] First report sent to host");
            }
          }
        }
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // 5. Status dump every 5 seconds
  // ──────────────────────────────────────────────────────────
  uint32_t now = millis();
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    Serial.printf("[STATUS] up=%lus  pkt=%lu  crc_e=%lu  radio=%lu  rumble_tx=%lu\r\n",
                  now / 1000, rx_count, crc_errors, NRF_RADIO->STATE, rumble_tx_count);
    Serial.printf("  mounted=%d  fifo_free=%lu\r\n",
                  (int)tud_vendor_mounted(),
                  tud_vendor_write_available());
  }
}
