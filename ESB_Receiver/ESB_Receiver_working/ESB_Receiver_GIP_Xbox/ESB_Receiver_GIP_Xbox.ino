// ============================================================
//  ESB_Receiver_GIP_Xbox.ino
//  NRF52840 — ESB Radio Receiver + USB GIP (Xbox One S spoof)
//
//  Receives gamepad state via 2.4GHz ESB radio and presents itself
//  to Windows as a wired Xbox One S controller using Microsoft's
//  Game Input Protocol (GIP).  Windows binds xusb22.sys /
//  dc1-controller.sys natively — no x360ce or other emulators needed.
//
//  Radio (unchanged from original HID version):
//    2 Mbps, Channel 2 (2402 MHz), CRC16
//    Address: E7:E7:E7:E7:E7  |  Payload: 14 bytes
//
//  USB:
//    VID 0x045E  PID 0x02EA  (Microsoft / Xbox One S Controller)
//    Interface Class 0xFF  SubClass 0x47  Protocol 0xD0
//    Interrupt endpoints (patched from Bulk) via Adafruit_USBD_WebUSB
//    GIP data written directly via tud_vendor_write()
//
// ============================================================
//  REQUIRED LIBRARY PATCH (Adafruit_USBD_WebUSB.cpp)
// ============================================================
//  In getInterfaceDescriptor(), after memcpy(buf, desc, len), add:
//
//    buf[6]  = 0x47;  // Interface SubClass: GIP
//    buf[7]  = 0xD0;  // Interface Protocol: GIP
//    buf[12] = 0x03;  // EP OUT bmAttributes: Interrupt (was Bulk)
//    buf[15] = 0x04;  // EP OUT bInterval: 4ms
//    buf[19] = 0x03;  // EP IN  bmAttributes: Interrupt (was Bulk)
//    buf[22] = 0x04;  // EP IN  bInterval: 4ms
//
//  Buffer layout (TUD_VENDOR_DESCRIPTOR = 9+7+7 = 23 bytes):
//    [0-8]   Interface descriptor  (bSubClass=[6], bProtocol=[7])
//    [9-15]  Endpoint OUT          (bmAttributes=[12], bInterval=[15])
//    [16-22] Endpoint IN           (bmAttributes=[19], bInterval=[22])
//
//  If using the local library in this project it has already been patched.
// ============================================================

#include "Adafruit_TinyUSB.h"
#include "arduino/webusb/Adafruit_USBD_WebUSB.h"
#include "nrf_sdm.h"

// Route all Serial.xxx() calls to the hardware UART (TX/RX pins).
// The USB port is now the Xbox GIP interface — USB-CDC Serial is unavailable.
// Connect a USB-to-TTL adapter: adapter TX→board RX, adapter RX→board TX, GND→GND.
// Open the COM port at 115200 baud to see debug output.
#define Serial Serial1

// ============================================================
//  Radio config — must match ESB_Transmitter exactly
// ============================================================
#define RADIO_CHANNEL     2
#define PAYLOAD_LEN       14
#define RADIO_BASE_ADDR   0xE7E7E7E7UL
#define RADIO_PREFIX_BYTE 0xE7

#define LED_PIN LED_BUILTIN

// Minimum interval between GIP input reports sent to the host.
// 4ms matches the real Xbox One S wired polling rate.
// Without this limit the inner loop can flood the FIFO with 0x20
// packets, leaving no room for the 7-byte ACK that xusb22.sys
// expects when it fires a rumble command on first input — causing
// the driver to reset the port ("disconnect on input" crash).
#define GIP_REPORT_INTERVAL_MS  4

// ============================================================
//  XInput button bitmasks (from Pico / xinput_host.c)
// ============================================================
#define BTN_DPAD_UP    0x0001
#define BTN_DPAD_DOWN  0x0002
#define BTN_DPAD_LEFT  0x0004
#define BTN_DPAD_RIGHT 0x0008
#define BTN_START      0x0010   // Xbox One "Menu"
#define BTN_BACK       0x0020   // Xbox One "View"
#define BTN_L3         0x0040
#define BTN_R3         0x0080
#define BTN_LB         0x0100
#define BTN_RB         0x0200
#define BTN_GUIDE      0x0400   // Xbox / Nexus button
#define BTN_A          0x1000
#define BTN_B          0x2000
#define BTN_X          0x4000
#define BTN_Y          0x8000

// ============================================================
//  GIP message types  (byte 0 of every GIP packet)
// ============================================================
#define GIP_TYPE_ACK       0x01
#define GIP_TYPE_ANNOUNCE  0x02
#define GIP_TYPE_IDENTIFY  0x05
#define GIP_TYPE_INPUT     0x20

// ============================================================
//  GIP Button Mask 1  (byte 4 of a 0x20 report)
// ============================================================
#define GIP_BTN1_SYNC  0x01   // Sync / Guide (Xbox button)
#define GIP_BTN1_MENU  0x04   // Menu  (was "Start")
#define GIP_BTN1_VIEW  0x08   // View  (was "Back")
#define GIP_BTN1_A     0x10
#define GIP_BTN1_B     0x20
#define GIP_BTN1_X     0x40
#define GIP_BTN1_Y     0x80

// ============================================================
//  GIP Button Mask 2  (byte 5 of a 0x20 report)
// ============================================================
#define GIP_BTN2_DPAD_U  0x01
#define GIP_BTN2_DPAD_D  0x02
#define GIP_BTN2_DPAD_L  0x04
#define GIP_BTN2_DPAD_R  0x08
#define GIP_BTN2_LB      0x10
#define GIP_BTN2_RB      0x20
#define GIP_BTN2_L3      0x40
#define GIP_BTN2_R3      0x80

// ============================================================
//  GIP 18-byte input state packet  (type = 0x20)
//
//  All GIP messages share the same 4-byte header:
//    Byte 0  command   — message type
//    Byte 1  sequence  — counter, wraps 255→0, shared across all messages
//    Byte 2  options   — flags, MUST be 0x00 for standard packets
//                        (previously mislabelled "len_lo" — that bug caused
//                         the driver to see invalid flags and an empty payload)
//    Byte 3  length    — payload size in bytes (0x0E = 14 for input)
//
//  Payload bytes 4–17 (14 bytes):
//    [4]    btn1      Sync|Menu|View|A|B|X|Y
//    [5]    btn2      DpadUDLR|LB|RB|L3|R3
//    [6-7]  lt        left  trigger, uint16 LE, range 0–1023
//    [8-9]  rt        right trigger, uint16 LE, range 0–1023
//    [10-11] lx       left  stick X, int16 LE  (-32768..32767)
//    [12-13] ly       left  stick Y, int16 LE  (up = +32767)
//    [14-15] rx       right stick X
//    [16-17] ry       right stick Y
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  command;    // GIP_TYPE_INPUT = 0x20
  uint8_t  sequence;   // global counter, increments each sent packet
  uint8_t  options;    // MUST be 0x00
  uint8_t  length;     // MUST be 0x0E  (14-byte payload)
  uint8_t  btn1;
  uint8_t  btn2;
  uint16_t lt;
  uint16_t rt;
  int16_t  lx;
  int16_t  ly;
  int16_t  rx;
  int16_t  ry;
} gip_report_t;        // must be exactly 18 bytes

// Compile-time guard: if the struct is not 18 bytes the driver will
// receive malformed packets and disconnect immediately.
static_assert(sizeof(gip_report_t) == 18,
              "gip_report_t is not 18 bytes — check for compiler padding");

// ============================================================
//  USB object
// ============================================================
Adafruit_USBD_WebUSB usb_webusb;

// GIP sequence counter — incremented on EVERY packet sent (announce,
// ack, and input reports all share one counter, matching real HW).
static uint8_t gip_seq = 0;

// ============================================================
//  GIP state machine
// ============================================================
static bool     gip_announced    = false;
static uint32_t gip_mounted_ms   = 0;
static bool     gip_ever_mounted = false;

// ============================================================
//  Cached input state (decoupled from radio receive rate)
//
//  The radio handler updates this cache on every valid packet.
//  The USB send fires independently on a 4ms timer, reading from
//  the cache.  This prevents FIFO starvation even if the radio runs
//  faster than the USB poll interval.
// ============================================================
static bool     input_cache_valid   = false;  // true after first radio packet
static uint32_t last_gip_send_ms    = 0;
static uint16_t c_buttons           = 0;
static int16_t  c_lx = 0, c_ly = 0, c_rx = 0, c_ry = 0;
static uint8_t  c_lt = 0, c_rt = 0;

// ============================================================
//  Weak-symbol overrides
// ============================================================

// Suppress WebUSB BOS descriptor so Windows does not try to bind WinUSB.
extern "C" const uint8_t* tud_descriptor_bos_cb(void) {
  return NULL;
}

// Stall vendor EP0 control requests — GIP traffic uses bulk/interrupt
// endpoints only, not EP0.
extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                            tusb_control_request_t const *request) {
  (void)rhport; (void)stage; (void)request;
  return false;
}

// ============================================================
//  Radio buffers and diagnostic counters
// ============================================================
static uint8_t  radio_pkt[PAYLOAD_LEN] __attribute__((aligned(4)));
static uint8_t  local_pkt[PAYLOAD_LEN];

static uint32_t rx_count        = 0;
static uint32_t crc_errors      = 0;
static uint32_t addr_events     = 0;
static uint32_t disabled_events = 0;
static uint32_t last_status_ms  = 0;
static uint32_t last_print_ms   = 0;

// ============================================================
//  XInput → GIP Button Mask 1
// ============================================================
uint8_t map_gip_btn1(uint16_t xb) {
  uint8_t b = 0;
  //if (xb & BTN_GUIDE) b |= GIP_BTN1_SYNC;
  if (xb & BTN_START) b |= GIP_BTN1_MENU;
  if (xb & BTN_BACK)  b |= GIP_BTN1_VIEW;
  if (xb & BTN_A)     b |= GIP_BTN1_A;
  if (xb & BTN_B)     b |= GIP_BTN1_B;
  if (xb & BTN_X)     b |= GIP_BTN1_X;
  if (xb & BTN_Y)     b |= GIP_BTN1_Y;
  return b;
}

// ============================================================
//  XInput → GIP Button Mask 2
// ============================================================
uint8_t map_gip_btn2(uint16_t xb) {
  uint8_t b = 0;
  if (xb & BTN_DPAD_UP)    b |= GIP_BTN2_DPAD_U;
  if (xb & BTN_DPAD_DOWN)  b |= GIP_BTN2_DPAD_D;
  if (xb & BTN_DPAD_LEFT)  b |= GIP_BTN2_DPAD_L;
  if (xb & BTN_DPAD_RIGHT) b |= GIP_BTN2_DPAD_R;
  if (xb & BTN_LB)         b |= GIP_BTN2_LB;
  if (xb & BTN_RB)         b |= GIP_BTN2_RB;
  if (xb & BTN_L3)         b |= GIP_BTN2_L3;
  if (xb & BTN_R3)         b |= GIP_BTN2_R3;
  return b;
}

// ============================================================
//  gip_safe_write() — wrapper that checks FIFO space first
//
//  tud_vendor_write() writes as many bytes as fit and returns the
//  count written.  If the FIFO is full it returns 0 and the caller
//  gets a silently dropped (or partial) packet — which the driver
//  treats as a disconnect.  Always use this wrapper instead of
//  calling tud_vendor_write() directly.
// ============================================================
static bool gip_safe_write(const uint8_t *buf, uint32_t len) {
  if (tud_vendor_write_available() < len) {
    Serial.printf("[GIP] FIFO full, dropped %lu bytes\r\n", len);
    return false;
  }
  uint32_t written = tud_vendor_write(buf, len);
  if (written != len) {
    Serial.printf("[GIP] short write: %lu/%lu bytes\r\n", written, len);
    return false;
  }
  tud_vendor_flush();
  return true;
}

// ============================================================
//  GIP Announce (type 0x02)
//
//  Header bytes: [0x02, seq=0x20, options=0x00, length=0x1E]
//  Payload (30 bytes): sub-command, padding, firmware/hardware version.
//
//  xusb22.sys validates the 4-byte header; payload content is
//  permissive.  If the driver still resets after this fix, capture a
//  real Xbox One S with USBPcap and replace bytes [4..33].
// ============================================================
void gip_send_announce() {
  static const uint8_t announce[34] = {
    GIP_TYPE_ANNOUNCE, 0x20, 0x00, 0x1e,  // header
    0x00,                                  // sub-command 0
    0x00, 0x00, 0x00,                      // padding
    0x06, 0x20, 0x00, 0x00,                // firmware 6.32.0.0
    0x00, 0x00, 0x01, 0x00,                // hardware 0.0.1.0
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
  };
  if (gip_safe_write(announce, sizeof(announce))) {
    gip_announced = true;
    Serial.println("[GIP] sent ANNOUNCE (0x02)");
  }
}

// ============================================================
//  GIP Receive handler
//
//  Drains the ENTIRE OUT endpoint FIFO each call (not just one packet).
//  Critical: if two host commands arrive between loop iterations (e.g.
//  a rumble 0x09 and a follow-up LED 0x06) we must handle both before
//  the next input report, otherwise the second ACK is never sent and
//  the driver resets the port.
//
//  ACK format (7 bytes total, 3-byte payload):
//    [0] 0x01        GIP_TYPE_ACK
//    [1] ++gip_seq   next sequence counter value
//    [2] 0x00        options (must be 0x00)
//    [3] 0x03        payload length = 3
//    [4] host_seq    sequence byte from the host's command
//    [5] host_cmd    command byte from the host's command  (e.g. 0x09)
//    [6] 0x00        status = success
// ============================================================
void gip_handle_rx() {
  // Drain all pending packets — not just one
  while (tud_vendor_available()) {
    uint8_t  buf[64];
    uint32_t len = tud_vendor_read(buf, sizeof(buf));
    if (len < 1) break;

    uint8_t host_cmd = buf[0];
    uint8_t host_seq = (len > 1) ? buf[1] : 0x00;

    Serial.printf("[GIP] RX cmd=0x%02X seq=0x%02X len=%lu\r\n",
                  host_cmd, host_seq, len);

    if (host_cmd == GIP_TYPE_IDENTIFY) {
      // Primary handshake trigger from xusb22.sys
      gip_send_announce();

    } else if (host_cmd != GIP_TYPE_ACK) {
      // Any other non-ACK command (rumble 0x09, LED 0x0A, etc.) gets a
      // 7-byte ACK.  xusb22.sys WILL reset the port if it doesn't
      // receive this within its timeout window (~10ms).
      uint8_t ack[7] = {
        GIP_TYPE_ACK,   // [0] = 0x01
        ++gip_seq,      // [1] next seq
        0x00,           // [2] options = 0
        0x03,           // [3] payload length = 3
        host_seq,       // [4] acked sequence
        host_cmd,       // [5] acked command (e.g. 0x09 rumble)
        0x00            // [6] status = success
      };
      gip_safe_write(ack, sizeof(ack));
    }
    // GIP_TYPE_ACK from host → no reply
  }
}

// ============================================================
//  Radio: hardware init
// ============================================================
void radio_init() {
  NVIC_DisableIRQ(RADIO_IRQn);
  NVIC_ClearPendingIRQ(RADIO_IRQn);

  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART    = 1;
  while (!NRF_CLOCK->EVENTS_HFCLKSTARTED);

  NRF_RADIO->TASKS_DISABLE = 1;
  while (NRF_RADIO->STATE != 0);

  NRF_RADIO->POWER     = 1;
  NRF_RADIO->TXPOWER   = 0;
  NRF_RADIO->FREQUENCY = RADIO_CHANNEL;
  NRF_RADIO->MODE      = RADIO_MODE_MODE_Nrf_2Mbit;

  NRF_RADIO->PCNF0 = 0;
  NRF_RADIO->PCNF1 = (PAYLOAD_LEN << RADIO_PCNF1_STATLEN_Pos) |
                      (PAYLOAD_LEN << RADIO_PCNF1_MAXLEN_Pos)  |
                      (4           << RADIO_PCNF1_BALEN_Pos);

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
  while (!NRF_RADIO->EVENTS_READY);
  NRF_RADIO->EVENTS_END  = 0;
  NRF_RADIO->TASKS_START = 1;
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  TinyUSBDevice.setID(0x045E, 0x02EA);
  TinyUSBDevice.setManufacturerDescriptor("Microsoft");
  TinyUSBDevice.setProductDescriptor("Xbox One Controller");
  usb_webusb.begin();
  TinyUSBDevice.setVersion(0x0200);   // undo the 0x0210 that begin() forces

  // Remap Serial1 to Nice!Nano physical pins:
  //   TX = P0.06 (Nice!Nano D1) → connect to TTL adapter RX
  //   RX = P0.22 (Nice!Nano D4) → connect to TTL adapter TX
  // Feather BSP: P0.06 = Arduino D11, P0.22 = Arduino D30
  Serial1.setPins(30, 11);  // setPins(RX_arduino_pin, TX_arduino_pin)
  Serial1.begin(115200);
  delay(3000);
  Serial.println("============================================");
  Serial.println("  ESB_Receiver_GIP_Xbox  —  NRF52840");
  Serial.println("  USB: Xbox One S  VID 0x045E  PID 0x02EA");
  Serial.println("  Interface: 0xFF / 0x47 / 0xD0  (GIP)");
  Serial.println("  Endpoints: Interrupt, 4ms");
  Serial.println("============================================");
  Serial.printf("gip_report_t = %d bytes (must be 18)\r\n",
                (int)sizeof(gip_report_t));

  uint32_t sd_err = sd_softdevice_disable();
  Serial.printf("sd_softdevice_disable() = %lu\r\n", sd_err);

  radio_init();
  radio_start_rx();
  Serial.printf("Radio: 2Mbps Ch2, STATE=%lu\r\n", NRF_RADIO->STATE);

  while (!USBDevice.mounted()) delay(1);
  Serial.println("USB mounted. Waiting for GIP IDENTIFY (0x05) from host...");

  digitalWrite(LED_PIN, LOW);
}

// ============================================================
//  loop()
// ============================================================
void loop() {

  // ──────────────────────────────────────────────────────────
  // 1. Drain all pending host commands FIRST, before anything else.
  //    This ensures ACKs for rumble/LED are handled before the next
  //    input report is sent.
  // ──────────────────────────────────────────────────────────
  gip_handle_rx();

  // ──────────────────────────────────────────────────────────
  // 2. GIP handshake state machine
  //    Primary path: 0x05 IDENTIFY handled in gip_handle_rx() above.
  //    Fallback: send announce 250ms after vendor endpoint mounts, in
  //    case this Windows build skips the IDENTIFY step.
  // ──────────────────────────────────────────────────────────
  if (tud_vendor_mounted()) {
    if (!gip_ever_mounted) {
      gip_ever_mounted = true;
      gip_mounted_ms   = millis();
      Serial.println("[GIP] vendor mounted");
    }
    if (!gip_announced && (millis() - gip_mounted_ms >= 250)) {
      Serial.println("[GIP] fallback announce after 250ms");
      gip_send_announce();
    }
  } else {
    if (gip_ever_mounted) {
      // USB disconnected / re-enumerated — reset everything
      gip_ever_mounted    = false;
      gip_announced       = false;
      gip_mounted_ms      = 0;
      input_cache_valid   = false;
      last_gip_send_ms    = 0;
      Serial.println("[GIP] vendor unmounted — state reset");
    }
  }

  // ──────────────────────────────────────────────────────────
  // 3. Radio event bookkeeping
  // ──────────────────────────────────────────────────────────
  if (NRF_RADIO->EVENTS_ADDRESS) {
    NRF_RADIO->EVENTS_ADDRESS = 0;
    addr_events++;
  }
  if (NRF_RADIO->EVENTS_DISABLED) {
    NRF_RADIO->EVENTS_DISABLED = 0;
    disabled_events++;
    if (NRF_RADIO->STATE == 0) radio_start_rx();
  }

  // ──────────────────────────────────────────────────────────
  // 4. Receive radio packet — update input cache ONLY.
  //    Do NOT send the USB report here.  Sending is decoupled to
  //    step 5 below so the send rate is governed by a wall-clock
  //    timer, not by the radio receive rate.
  // ──────────────────────────────────────────────────────────
  if (NRF_RADIO->EVENTS_END) {
    NRF_RADIO->EVENTS_END = 0;
    memcpy(local_pkt, radio_pkt, PAYLOAD_LEN);

    if (NRF_RADIO->CRCSTATUS == 1) {
      rx_count++;
      digitalToggle(LED_PIN);

      // Decode 14-byte ESB payload into the input cache
      c_buttons = (uint16_t)(local_pkt[0] | (local_pkt[1] << 8));
      c_lx      = (int16_t) (local_pkt[2] | (local_pkt[3] << 8));
      c_ly      = (int16_t) (local_pkt[4] | (local_pkt[5] << 8));
      c_rx      = (int16_t) (local_pkt[6] | (local_pkt[7] << 8));
      c_ry      = (int16_t) (local_pkt[8] | (local_pkt[9] << 8));
      c_lt      = local_pkt[10];
      c_rt      = local_pkt[11];
      input_cache_valid = true;

      // Rate-limited serial debug (2 Hz)
      uint32_t now_ms = millis();
      if (now_ms - last_print_ms >= 500) {
        last_print_ms = now_ms;
        Serial.printf(
          "BTN:%04X b1:%02X b2:%02X LX:%6d LY:%6d RX:%6d RY:%6d LT:%3d RT:%3d\r\n",
          c_buttons, map_gip_btn1(c_buttons), map_gip_btn2(c_buttons),
          c_lx, c_ly, c_rx, c_ry, c_lt, c_rt);
      }
    } else {
      crc_errors++;
    }
  }

  // ──────────────────────────────────────────────────────────
  // 5. Send GIP input report at most once per GIP_REPORT_INTERVAL_MS.
  //
  //    Decoupled from radio step above.  The 4ms timer ensures:
  //      a) We never flood the FIFO faster than the host can drain it.
  //      b) There is always space in the FIFO for a 7-byte ACK when
  //         xusb22.sys fires a rumble command on first input.
  //      c) The report rate matches what a real Xbox One S sends.
  // ──────────────────────────────────────────────────────────
  // 5. Send GIP input report (Fixed Timer & Send-on-Change)
  // ──────────────────────────────────────────────────────────
  if (tud_vendor_mounted() && gip_announced && input_cache_valid) {
    uint32_t now_ms = millis();

    if (now_ms - last_gip_send_ms >= GIP_REPORT_INTERVAL_MS) {
      last_gip_send_ms = now_ms; // CRITICAL FIX: Timer updates OUTSIDE the write check!

      // Only send if inputs changed, or send a heartbeat every 500ms
      static uint16_t last_btn = 0;
      static int16_t last_lx = 0, last_ly = 0, last_rx = 0, last_ry = 0;
      static uint8_t last_lt = 0, last_rt = 0;
      static uint32_t last_heartbeat = 0;

      bool changed = (c_buttons != last_btn) || (c_lx != last_lx) || (c_ly != last_ly) ||
                     (c_rx != last_rx) || (c_ry != last_ry) || (c_lt != last_lt) || (c_rt != last_rt);

      if (changed || (now_ms - last_heartbeat >= 500)) {
        last_heartbeat = now_ms;

        gip_report_t report;
        report.command  = GIP_TYPE_INPUT;
        report.sequence = ++gip_seq;
        report.options  = 0x00;
        report.length   = 0x0E;
        report.btn1     = map_gip_btn1(c_buttons);
        report.btn2     = map_gip_btn2(c_buttons);
        report.lt       = (uint16_t)c_lt * 4;
        report.rt       = (uint16_t)c_rt * 4;
        report.lx       = c_lx;
        report.ly       = c_ly;
        report.rx       = c_rx;
        report.ry       = c_ry;

        if (gip_safe_write((const uint8_t*)&report, sizeof(report))) {
          // Update tracking variables only if sent successfully
          last_btn = c_buttons;
          last_lx = c_lx; last_ly = c_ly;
          last_rx = c_rx; last_ry = c_ry;
          last_lt = c_lt; last_rt = c_rt;
        }
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // 6. Periodic status dump every 5 s
  // ──────────────────────────────────────────────────────────
  uint32_t now_ms = millis();
  if (now_ms - last_status_ms >= 5000) {
    last_status_ms = now_ms;
    Serial.printf(
      "[STATUS] up=%lus pkt=%lu crc_e=%lu radio_st=%lu\r\n",
      now_ms / 1000, rx_count, crc_errors, NRF_RADIO->STATE);
    Serial.printf(
      "  vendor=%d announced=%d seq=%d fifo_free=%lu\r\n",
      (int)tud_vendor_mounted(), (int)gip_announced,
      gip_seq, tud_vendor_write_available());
  }
}
