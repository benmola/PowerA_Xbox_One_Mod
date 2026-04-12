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

// ============================================================
//  GIP USB Interface — guarantees correct descriptors
//
//  The Adafruit BSP compiles its own copy of Adafruit_USBD_WebUSB.cpp,
//  ignoring any patches to the local library.  By overriding
//  getInterfaceDescriptor() right here in the .ino, the patched bytes
//  are compiled as part of the sketch — guaranteed to be used.
//
//  Without this, the endpoints stay as Bulk (0x02) instead of
//  Interrupt (0x03), and xusb22.sys never polls our IN endpoint.
// ============================================================
class GIP_Vendor_Device : public Adafruit_USBD_WebUSB {
public:
  GIP_Vendor_Device() : Adafruit_USBD_WebUSB(NULL) {}

  uint16_t getInterfaceDescriptor(uint8_t itfnum_deprecated,
                                  uint8_t *buf, uint16_t bufsize) override {
    uint16_t len = Adafruit_USBD_WebUSB::getInterfaceDescriptor(
                       itfnum_deprecated, buf, bufsize);
    if (len > 0 && buf != NULL && len >= 23) {
      // Patch interface class/subclass/protocol for Xbox GIP
      buf[6]  = 0x47;   // bInterfaceSubClass: GIP
      buf[7]  = 0xD0;   // bInterfaceProtocol: GIP
      // Patch endpoints: Bulk → Interrupt, bInterval = 4ms
      buf[12] = 0x03;   // EP OUT bmAttributes: Interrupt
      buf[15] = 0x04;   // EP OUT bInterval: 4ms
      buf[19] = 0x03;   // EP IN  bmAttributes: Interrupt
      buf[22] = 0x04;   // EP IN  bInterval: 4ms
    }
    return len;
  }
};

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
//  Corrected from xone Linux driver (medusalix/xone)
// ============================================================
#define GIP_CMD_ACK          0x01
#define GIP_CMD_ANNOUNCE     0x02
#define GIP_CMD_STATUS       0x03
#define GIP_CMD_IDENTIFY     0x04   // host requests / device responds with capabilities
#define GIP_CMD_POWER        0x05   // host sends power mode (on/off/sleep/reset)
#define GIP_CMD_AUTHENTICATE 0x06   // crypto auth handshake (or skip via AUTH COMPLETE)
#define GIP_CMD_GUIDE_BTN    0x07   // virtual key (guide button)
#define GIP_CMD_RUMBLE       0x09
#define GIP_CMD_LED          0x0A
#define GIP_CMD_INPUT        0x20

// ============================================================
//  GIP header options flags
// ============================================================
#define GIP_OPT_ACKNOWLEDGE  0x10
#define GIP_OPT_INTERNAL     0x20
#define GIP_OPT_CHUNK_START  0x40
#define GIP_OPT_CHUNK        0x80

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
//  GIP 18-byte input state packet  (cmd = 0x20)
//
//  GIP header layout (from xone driver):
//    Byte 0  command   — message type (0x20 for input)
//    Byte 1  options   — flags (0x00 for client commands like INPUT)
//    Byte 2  sequence  — counter (0x00 for wired single-controller instance)
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
  uint8_t  command;    // GIP_CMD_INPUT = 0x20
  uint8_t  client;     // client/instance — ALWAYS 0x00 for wired single controller
                       // (NOT a sequence counter — xusb22.sys uses this byte to
                       //  look up the registered client; an incrementing value
                       //  makes each report appear to come from a different device)
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
//  USB object  — uses our GIP_Vendor_Device override, NOT the
//  BSP's Adafruit_USBD_WebUSB (which may have unpatched Bulk endpoints).
// ============================================================
GIP_Vendor_Device usb_webusb;

// GIP sequence counter — incremented for each device→host packet that uses
// a non-zero sequence.  Input reports use client=0x00 (not a sequence).
static uint8_t gip_seq = 0;

// ============================================================
//  GIP IDENTIFY response — binary capability descriptor
//
//  When the host sends cmd 0x04 (IDENTIFY request), the device must
//  respond with cmd 0x04 carrying this binary payload.  It contains:
//    - 16 unknown header bytes
//    - Offset table pointing to data sections
//    - Classes: "Windows.Xbox.Input.Gamepad"
//    - Interfaces: GUIDs (we provide one null GUID)
//    - Firmware versions, capabilities
//
//  Packet format derived from the xone Linux driver (medusalix/xone).
//  The class string is what tells xusb22.sys to register us as a gamepad.
//  Without this response, the controller loads but never appears in XInput.
// ============================================================
static const char kGipClass[] = "Windows.Xbox.Input.Gamepad";  // 26 chars
#define GIP_CLASS_LEN (sizeof(kGipClass) - 1)  // 26, no null

// Count of OUT packets received from host.  Used to determine if the
// OUT endpoint is delivering data.  If this stays at 0, the OUT endpoint
// is broken or xusb22.sys is not sending anything to our device.
static uint32_t gip_out_pkt_count = 0;

// ============================================================
//  GIP state machine  (corrected from xone Linux driver)
//
//  Protocol flow:
//    IDLE       → device not mounted
//    ANNOUNCED  → device sent ANNOUNCE (0x02), waiting for host 0x04
//    IDENTIFIED → device responded to host IDENTIFY request (0x04)
//    READY      → host sent POWER ON (0x05); input reports enabled
// ============================================================
typedef enum {
  GIP_STATE_IDLE,
  GIP_STATE_ANNOUNCED,
  GIP_STATE_IDENTIFIED,
  GIP_STATE_READY
} gip_state_t;
static gip_state_t gip_state       = GIP_STATE_IDLE;
static bool     gip_announced      = false;
static uint32_t gip_mounted_ms     = 0;
static bool     gip_ever_mounted   = false;

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

// "Send-on-change" shadow state.
// Initialised to impossible values so the very first packet always sends.
static uint16_t last_c_buttons = 0xFFFF;
static int16_t  last_c_lx = 1, last_c_ly = 1, last_c_rx = 1, last_c_ry = 1;
static uint8_t  last_c_lt = 0xFF, last_c_rt = 0xFF;
// Heartbeat: resend even when nothing changed, to keep the driver alive.
// 16ms matches the ~60Hz cadence Xbox drivers expect; 500ms is too slow
// and triggers a port reset.
static uint32_t last_heartbeat_ms   = 0;
#define GIP_HEARTBEAT_MS  16

// ============================================================
//  Weak-symbol overrides
// ============================================================

// Suppress WebUSB BOS descriptor so Windows does not try to bind WinUSB.
extern "C" const uint8_t* tud_descriptor_bos_cb(void) {
  return NULL;
}

// Low-level USB OUT-endpoint callback — fires in the TinyUSB task context
// the moment any data arrives on the vendor OUT endpoint, BEFORE it is put
// in the application FIFO.  In buffered mode (CFG_TUD_VENDOR_TXRX_BUFFERED=1)
// TinyUSB passes buffer=NULL / bufsize=0; use tud_vendor_read() to get data.
// If this never prints, the host is not sending to our OUT endpoint at all.
extern "C" void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize) {
  (void)idx; (void)buffer; (void)bufsize;
  // Use a flag rather than Serial.printf — this fires in USB task context and
  // Serial.printf may not be safe there; the flag is read in loop().
  // In practice on Adafruit nRF52, tud_vendor_rx_cb IS called from RTOS task,
  // so Serial is safe — but keep it brief.
  Serial.printf("[RX_CB] OUT data arrived: bufsize=%lu (idx=%d)\r\n", bufsize, idx);
}

// Handle vendor EP0 control requests — ACK instead of STALL.
//
// Previously this returned false unconditionally, which sent a USB STALL
// on EP0.  xusb22.sys sends at least one vendor control request during
// its initialization sequence; receiving a STALL caused it to silently
// stop polling our IN endpoint entirely (permanent FIFO full).
//
// Fix: ACK SET-type requests (wLength==0) and return zeros for GET-type
// requests.  GIP data travels on the interrupt endpoints, not EP0, so
// the exact EP0 response content does not matter — we just must not STALL.
extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                            tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP) return true;  // ACK data/status stages

  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
    if (request->wLength == 0) {
      // SET command (no data phase) — acknowledge it
      Serial.printf("[EP0] vendor SET req=0x%02X — ACK\r\n", request->bRequest);
      return tud_control_status(rhport, request);
    } else {
      // GET command (host wants data) — return zeros rather than stalling
      Serial.printf("[EP0] vendor GET req=0x%02X len=%u — zero reply\r\n",
                    request->bRequest, request->wLength);
      static uint8_t dummy[64] = {0};
      return tud_control_xfer(rhport, request, dummy,
                              (uint16_t)tu_min16(request->wLength, sizeof(dummy)));
    }
  }

  return false;  // stall anything that isn't a vendor request
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
//
//  wait_ms:  If > 0, spin-wait up to that many ms for FIFO space
//            (yields to FreeRTOS so the USB task can drain the FIFO).
//            Use for handshake packets (announce, metadata, ACK).
//            Pass 0 (default) for input reports to avoid blocking.
// ============================================================
static bool gip_safe_write(const uint8_t *buf, uint32_t len, uint32_t wait_ms = 0) {
  if (wait_ms > 0) {
    uint32_t t0 = millis();
    while (tud_vendor_write_available() < len) {
      if (millis() - t0 >= wait_ms) {
        Serial.printf("[GIP] FIFO write timeout (%lums), dropped %lu bytes  fifo_free=%lu\r\n",
                      wait_ms, len, tud_vendor_write_available());
        return false;
      }
      delay(1);  // yield to FreeRTOS — lets USB task complete IN transfers
    }
  } else {
    if (tud_vendor_write_available() < len) {
      Serial.printf("[GIP] FIFO full, dropped %lu bytes\r\n", len);
      return false;
    }
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
//  GIP Announce (cmd 0x02)
//
//  GIP header: [command, options, sequence, varint_length]
//  Payload (28 bytes): gip_pkt_announce struct from xone driver:
//    address[6]     — MAC (fake, all zeros)
//    unknown[2]     — 0x00
//    vendor_id[2]   — USB VID (LE), must match USB descriptor
//    product_id[2]  — USB PID (LE), must match USB descriptor
//    fw_version     — {major, minor, build, revision} 4×u16 LE
//    hw_version     — {major, minor, build, revision} 4×u16 LE
// ============================================================
void gip_send_announce() {
  static const uint8_t announce[32] = {
    GIP_CMD_ANNOUNCE, GIP_OPT_INTERNAL, 0x00, 0x1C,  // header: cmd=0x02, opts=0x20, seq=0, len=28
    // address[6] — fake MAC
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // unknown[2]
    0x00, 0x00,
    // vendor_id LE (0x045E = Microsoft)
    0x5E, 0x04,
    // product_id LE (0x02EA = Xbox One S Controller)
    0xEA, 0x02,
    // fw_version: major=3, minor=1, build=0, revision=0  (4×u16 LE)
    0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    // hw_version: major=1, minor=0, build=0, revision=0  (4×u16 LE)
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  if (gip_safe_write(announce, sizeof(announce), 500)) {
    gip_announced = true;
    gip_state = GIP_STATE_ANNOUNCED;
    Serial.println("[GIP] sent ANNOUNCE (0x02, VID=045E PID=02EA, fw=3.1)");
  }
}

// ============================================================
//  GIP IDENTIFY response (cmd 0x04)
//
//  Sent in response to host's IDENTIFY request (0x04 with no payload).
//  Contains a binary capability descriptor (gip_pkt_identify from xone):
//    unknown[16]
//    Offset table (8 × u16 LE) pointing to data sections
//    Data: firmware versions, capabilities, class string, interface GUIDs
//
//  The class "Windows.Xbox.Input.Gamepad" is what tells xusb22.sys
//  to register this device as an Xbox gamepad (XInput device).
//
//  Total payload = 87 bytes, which exceeds the 64-byte USB FIFO.
//  We write it in two FIFO-sized pieces, letting USB handle framing.
// ============================================================
void gip_send_identify() {
  // IDENTIFY payload — 87 bytes
  //
  // xone's gip_handle_pkt_identify() does:
  //   pkt = data;                          // pkt at byte 0
  //   data += sizeof(pkt->unknown);        // data now at byte 16
  //   len  -= sizeof(pkt->unknown);        // len = payload - 16
  //
  // Then gip_parse_info_element(data, len, offset, ...) reads data[offset].
  // So ALL offsets are relative to byte 16 of the payload:
  //   data[0..15]  = the offset table itself
  //   data[16]     = byte 32 of payload = first data section byte
  //   offset = 0   → "not present" (returns NULL)
  //   offset = 16  → first byte after offset table
  //
  // No padding byte needed since offset 0 is just the "not present" sentinel.
  static const uint8_t identify_payload[] = {
    // ---- gip_pkt_identify struct (32 bytes) ----
    // unknown[16]  (bytes 0–15)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Offset table (bytes 16–31, each u16 LE, relative to byte 16)
    0x00, 0x00,    // client_commands_offset   = 0  (not present)
    0x10, 0x00,    // firmware_versions_offset = 16 → data[16] = byte 32
    0x00, 0x00,    // audio_formats_offset     = 0  (not present)
    0x15, 0x00,    // capabilities_out_offset  = 21 → data[21] = byte 37
    0x17, 0x00,    // capabilities_in_offset   = 23 → data[23] = byte 39
    0x19, 0x00,    // classes_offset           = 25 → data[25] = byte 41
    0x36, 0x00,    // interfaces_offset        = 54 → data[54] = byte 70
    0x00, 0x00,    // hid_descriptor_offset    = 0  (not present)
    // ---- Data sections (byte 32+, data[16]+) ----
    // [data[16], byte 32] Firmware versions: count=1, {major=3, minor=1}
    0x01,
    0x03, 0x00,    // major = 3
    0x01, 0x00,    // minor = 1
    // [data[21], byte 37] Capabilities out: count=1, flags
    0x01,
    0x1F,          // standard gamepad output (buttons, triggers, sticks)
    // [data[23], byte 39] Capabilities in: count=1, flags
    0x01,
    0x0F,          // rumble motors (left, right, left trigger, right trigger)
    // [data[25], byte 41] Classes: count=1, strlen=26 (LE), class string
    0x01,
    0x1A, 0x00,    // string length = 26 (LE u16)
    'W','i','n','d','o','w','s','.','X','b','o','x','.','I','n','p','u','t','.','G','a','m','e','p','a','d',
    // [data[54], byte 70] Interfaces: count=1, one null GUID (16 bytes)
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  static_assert(sizeof(identify_payload) == 87,
                "identify payload must be 87 bytes");

  // GIP header: cmd=0x04, options=0x20 (INTERNAL), sequence, varint length
  uint8_t hdr[4] = {
    GIP_CMD_IDENTIFY,
    GIP_OPT_INTERNAL,    // options = 0x20 (internal command)
    ++gip_seq,            // sequence (non-zero for reliability)
    (uint8_t)sizeof(identify_payload),  // 87 < 128, fits in 1-byte varint
  };

  // Build full packet: header + payload = 4 + 87 = 91 bytes
  uint8_t pkt[4 + sizeof(identify_payload)];
  memcpy(pkt, hdr, 4);
  memcpy(pkt + 4, identify_payload, sizeof(identify_payload));

  // Write to FIFO in two pieces (FIFO is 64 bytes max)
  uint32_t first = 64;
  uint32_t second = sizeof(pkt) - first;  // 27 bytes

  if (!gip_safe_write(pkt, first, 500)) {
    Serial.println("[GIP] IDENTIFY part1 dropped");
    return;
  }
  // Wait for first piece to drain, then write remainder
  if (!gip_safe_write(pkt + first, second, 500)) {
    Serial.println("[GIP] IDENTIFY part2 dropped");
    return;
  }

  gip_state = GIP_STATE_IDENTIFIED;
  Serial.println("[GIP] sent IDENTIFY response (class=Windows.Xbox.Input.Gamepad)");
}

// ============================================================
//  GIP AUTH COMPLETE (cmd 0x06)
//
//  Skips the cryptographic authentication handshake by immediately
//  sending an AUTH COMPLETE control packet.  This technique is used
//  by Mad Catz drum kits (see xone madcatz_glam.c) and may work
//  for third-party controllers on Windows.
//
//  Payload: {context=0x01 (CONTROL), control=0x00 (COMPLETE)}
// ============================================================
void gip_send_auth_complete() {
  uint8_t pkt[6] = {
    GIP_CMD_AUTHENTICATE,   // command = 0x06
    GIP_OPT_INTERNAL,       // options = 0x20
    ++gip_seq,               // sequence
    0x02,                    // payload length = 2
    0x01,                    // context = AUTH_CTX_CONTROL
    0x00,                    // control = AUTH_CTRL_COMPLETE
  };
  if (gip_safe_write(pkt, sizeof(pkt), 500)) {
    Serial.println("[GIP] sent AUTH COMPLETE (skipping crypto handshake)");
  }
}

// ============================================================
//  GIP Receive handler  (corrected command assignments from xone)
//
//  GIP header: [command, options, sequence, varint_length, ...]
//  Note: byte 1 is OPTIONS (not sequence!), byte 2 is SEQUENCE.
//
//  ACK format (xone gip_pkt_acknowledge):
//    GIP header: [0x01, options, seq, payload_len]
//    Payload: [unknown, acked_cmd, acked_options, acked_len[2], pad[2], remaining[2]]
//    Total payload = 9 bytes
//
//  We send a simplified 7-byte ACK that xusb22.sys accepts.
// ============================================================
void gip_handle_rx() {
  while (tud_vendor_available()) {
    uint8_t  buf[64];
    uint32_t len = tud_vendor_read(buf, sizeof(buf));
    if (len < 1) break;

    uint8_t host_cmd = buf[0];
    // byte 1 = options, byte 2 = sequence (corrected from xone)
    uint8_t host_opts = (len > 1) ? buf[1] : 0x00;
    uint8_t host_seq  = (len > 2) ? buf[2] : 0x00;
    gip_out_pkt_count++;

    // Hex dump first 8 bytes for diagnostics
    Serial.printf("[GIP] RX cmd=0x%02X opts=0x%02X seq=0x%02X len=%lu  hex:",
                  host_cmd, host_opts, host_seq, len);
    for (uint32_t i = 0; i < len && i < 12; i++)
      Serial.printf(" %02X", buf[i]);
    Serial.printf("  (OUT#%lu)\r\n", gip_out_pkt_count);

    if (host_cmd == GIP_CMD_IDENTIFY) {
      // 0x04 — Host requests our device capabilities.
      // Respond with IDENTIFY data (class, interfaces, versions).
      Serial.println("[GIP] IDENTIFY REQUEST (0x04) — sending capability descriptor");
      gip_send_identify();

    } else if (host_cmd == GIP_CMD_POWER) {
      // 0x05 — Host sends power mode.  Payload[0] = mode:
      //   0x00 = ON, 0x01 = SLEEP, 0x04 = OFF, 0x07 = RESET
      // On receiving POWER ON, start sending input reports.
      uint8_t mode = (len > 3 && buf[3] > 0) ? buf[4] : 0xFF;
      Serial.printf("[GIP] POWER (0x05) mode=0x%02X — %s\r\n",
                    mode, (mode == 0x00) ? "ON" : "other");
      // ACK the power command
      uint8_t ack[7] = {
        GIP_CMD_ACK, GIP_OPT_INTERNAL, ++gip_seq, 0x03,
        host_seq, host_cmd, 0x00
      };
      gip_safe_write(ack, sizeof(ack), 50);
      if (mode == 0x00 || mode == 0xFF) {
        gip_state = GIP_STATE_READY;
        Serial.println("[GIP] state → READY (input enabled)");
      }

    } else if (host_cmd == GIP_CMD_AUTHENTICATE) {
      // 0x06 — Host initiates authentication handshake.
      // We skip auth by sending AUTH COMPLETE immediately.
      Serial.println("[GIP] AUTHENTICATE (0x06) — sending AUTH COMPLETE to skip");
      gip_send_auth_complete();

    } else if (host_cmd == GIP_CMD_ACK) {
      // 0x01 — Host ACK of one of our packets.
      uint8_t acked_cmd = (len > 5) ? buf[5] : (len > 4) ? buf[4] : 0x00;
      Serial.printf("[GIP] host ACK of cmd=0x%02X\r\n", acked_cmd);
      // If the host ACKed our IDENTIFY, the handshake is progressing
      if (acked_cmd == GIP_CMD_IDENTIFY) {
        Serial.println("[GIP] host ACKed IDENTIFY — waiting for POWER ON");
      }

    } else {
      // Unhandled command (rumble 0x09, LED 0x0A, etc.) — ACK it.
      uint8_t ack[7] = {
        GIP_CMD_ACK, GIP_OPT_INTERNAL, ++gip_seq, 0x03,
        host_seq, host_cmd, 0x00
      };
      gip_safe_write(ack, sizeof(ack), 50);
    }
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

  // --- Critical: remove CDC serial, keep only our GIP vendor interface ---
  //
  // TinyUSBDevice.begin() (called internally by usb_webusb.begin()) auto-
  // registers CDC serial on nRF52, which consumes interfaces 0-1 and
  // pushes our vendor interface to interface 2+.  xusb22.sys expects the
  // GIP interface at index 0 on a non-composite device.
  //
  // clearConfiguration() also resets VID/PID/strings to BSP defaults,
  // so setID() and string descriptors MUST come AFTER clearConfiguration().
  //
  usb_webusb.begin();                      // inits TinyUSB + adds CDC + adds vendor
  TinyUSBDevice.clearConfiguration();      // strip ALL (CDC + vendor + VID/PID/strings!)

  // Re-set identity AFTER clearConfiguration wiped them
  TinyUSBDevice.setID(0x045E, 0x02EA);
  TinyUSBDevice.setManufacturerDescriptor("Microsoft");
  TinyUSBDevice.setProductDescriptor("Xbox One S Controller");
  TinyUSBDevice.setVersion(0x0200);        // undo 0x0210 that begin() forces

  TinyUSBDevice.addInterface(usb_webusb);  // re-add vendor ONLY → interface 0

  // Force re-enumeration so the host sees the clean single-interface config
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  // Remap Serial1 to Nice!Nano physical pins:
  //   TX = P0.06 (Nice!Nano D1) → connect to TTL adapter RX
  //   RX = P0.22 (Nice!Nano D4) → connect to TTL adapter TX
  // Feather BSP: P0.06 = Arduino D11, P0.22 = Arduino D30
  Serial1.setPins(30, 11);  // setPins(RX_arduino_pin, TX_arduino_pin)
  Serial1.begin(115200);
  delay(3000);
  Serial.println("============================================");
  Serial.println("  ESB_Receiver_GIP_Xbox  —  NRF52840");
  Serial.println("  USB: Microsoft Xbox One S  VID 0x045E  PID 0x02EA");
  Serial.println("============================================");

  // Dump device descriptor class fields
  {
    extern uint8_t const *tud_descriptor_device_cb(void);
    const uint8_t *dd = tud_descriptor_device_cb();
    uint16_t vid = (uint16_t)(dd[8] | (dd[9] << 8));
    uint16_t pid = (uint16_t)(dd[10] | (dd[11] << 8));
    Serial.printf("  VID=0x%04X PID=0x%04X  DevClass=0x%02X SubClass=0x%02X Proto=0x%02X\r\n",
                  vid, pid, dd[4], dd[5], dd[6]);
    if (vid != 0x045E || pid != 0x02EA) {
      Serial.println("  *** WARNING: VID/PID wrong! xusb22.sys won't bind ***");
    }
  }

  // Dump the actual interface descriptor bytes to verify the GIP patch
  // is applied.  buf[12]=0x03 (Interrupt) is the critical byte — if it's
  // 0x02 (Bulk) the host will never poll our IN endpoint.
  //
  // Note: we read from the cached config descriptor (tud_descriptor_configuration_cb)
  // NOT from getInterfaceDescriptor() — calling that again would allocate
  // duplicate interface/endpoint numbers as a side effect.
  {
    extern uint8_t const *tud_descriptor_configuration_cb(uint8_t index);
    const uint8_t *cfg = tud_descriptor_configuration_cb(0);
    // Config descriptor header is 9 bytes; our interface descriptor starts at offset 9
    const uint8_t *desc_buf = cfg + 9;
    uint16_t cfg_total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    uint16_t desc_len = (cfg_total > 9) ? (cfg_total - 9) : 0;

    Serial.printf("  Config total=%u  Itf bytes=%u  NumInterfaces=%d\r\n",
                  cfg_total, desc_len, cfg[4]);
    Serial.printf("  Descriptor:");
    for (uint16_t i = 0; i < desc_len && i < 23; i++) {
      Serial.printf(" %02X", desc_buf[i]);
    }
    Serial.println();
    if (desc_len >= 23) {
      Serial.printf("  Itf#=%d  SubClass=0x%02X Proto=0x%02X  EP_OUT=0x%02X(attr=%02X int=%d)  EP_IN=0x%02X(attr=%02X int=%d)\r\n",
                    desc_buf[2], desc_buf[6], desc_buf[7],
                    desc_buf[11], desc_buf[12], desc_buf[15],
                    desc_buf[18], desc_buf[19], desc_buf[22]);
      bool ok = (desc_buf[2] == 0x00 && desc_buf[6] == 0x47 && desc_buf[7] == 0xD0 &&
                 desc_buf[12] == 0x03 && desc_buf[19] == 0x03);
      Serial.printf("  GIP patch: %s\r\n", ok ? "OK (Itf#0, Interrupt endpoints)" :
                    (desc_buf[2] != 0x00 ? "WRONG INTERFACE NUMBER (must be 0!)" :
                     "MISSING (Bulk endpoints — host won't poll!)"));
    }
  }
  Serial.printf("gip_report_t = %d bytes (must be 18)\r\n",
                (int)sizeof(gip_report_t));

  uint32_t sd_err = sd_softdevice_disable();
  Serial.printf("sd_softdevice_disable() = %lu\r\n", sd_err);

  radio_init();
  radio_start_rx();
  Serial.printf("Radio: 2Mbps Ch2, STATE=%lu\r\n", NRF_RADIO->STATE);

  while (!USBDevice.mounted()) delay(1);
  Serial.println("USB mounted. Sending GIP ANNOUNCE, then waiting for host IDENTIFY request (0x04)...");

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
  // 2. GIP handshake — send ANNOUNCE on mount
  //
  //    xusb22.sys expects the device to speak first with an ANNOUNCE.
  //    After announce, the host will send IDENTIFY request (0x04).
  //    We respond with IDENTIFY data in gip_handle_rx().
  //    Do NOT send metadata/identify unsolicited — wait for the host.
  // ──────────────────────────────────────────────────────────
  if (tud_vendor_mounted()) {
    if (!gip_ever_mounted) {
      gip_ever_mounted = true;
      gip_mounted_ms   = millis();
      Serial.printf("[GIP] vendor mounted — FIFO capacity: %lu bytes\r\n",
                    tud_vendor_write_available());
      Serial.println("[GIP] sending ANNOUNCE only — waiting for host IDENTIFY request");
      gip_send_announce();   // tell host we exist; capabilities sent on request
    }
  } else {
    if (gip_ever_mounted) {
      // USB disconnected / re-enumerated — reset everything
      gip_ever_mounted    = false;
      gip_announced       = false;
      gip_state           = GIP_STATE_IDLE;
      gip_mounted_ms      = 0;
      input_cache_valid   = false;
      last_gip_send_ms    = 0;
      last_heartbeat_ms   = 0;
      last_c_buttons      = 0xFFFF;  // force re-send on reconnect
      Serial.printf("[GIP] vendor unmounted — state reset (total OUT pkts this session: %lu)\r\n",
                    gip_out_pkt_count);
      gip_out_pkt_count   = 0;
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
  // 5. Send GIP input report — 4ms gate + send-on-change + heartbeat
  //
  //    a) 4ms timer updated BEFORE write — a failed write never causes
  //       an infinite spin that locks the MCU ("FIFO full" infinite loop).
  //    b) Send-on-change — skip if all axes/buttons match the last
  //       successfully transmitted report.
  //    c) Heartbeat — force a send every GIP_HEARTBEAT_MS (500ms) even
  //       when idle, so xusb22.sys does not consider the device stalled.
  //       Heartbeat timer resets only on successful write, so FIFO-full
  //       failures retry on the next loop iteration.
  // ──────────────────────────────────────────────────────────
  // gip_state == GIP_STATE_READY gates input reports.
  // State advances to READY when the host sends POWER ON (0x05).
  // Sending 0x20 before READY floods the FIFO and causes xusb22.sys to
  // stop polling the IN endpoint entirely.
  if (tud_vendor_mounted() && gip_state == GIP_STATE_READY) {
    uint32_t now_ms = millis();
    if (now_ms - last_gip_send_ms >= GIP_REPORT_INTERVAL_MS) {
      last_gip_send_ms = now_ms;   // always advance — prevents spin-lock

      bool changed   = (c_buttons != last_c_buttons ||
                        c_lx != last_c_lx || c_ly != last_c_ly ||
                        c_rx != last_c_rx || c_ry != last_c_ry ||
                        c_lt != last_c_lt || c_rt != last_c_rt);
      bool heartbeat = (now_ms - last_heartbeat_ms >= GIP_HEARTBEAT_MS);

      if (changed || heartbeat) {
        gip_report_t report;
        report.command  = GIP_CMD_INPUT;
        report.client   = 0x00;   // wired single controller = instance 0
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
          // Log the very first input report sent (one-time diagnostic)
          static bool first_report_logged = false;
          if (!first_report_logged) {
            first_report_logged = true;
            Serial.println("[GIP] first INPUT report (0x20) sent to host");
          }
          // Shadow state: only updated on success so we don't miss a change
          last_c_buttons = c_buttons;
          last_c_lx = c_lx;  last_c_ly = c_ly;
          last_c_rx = c_rx;  last_c_ry = c_ry;
          last_c_lt = c_lt;  last_c_rt = c_rt;
          last_heartbeat_ms = now_ms;  // reset heartbeat only on success
        }
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // 6. Fallback timer — if the host doesn't complete the handshake
  //    within 5 seconds, start input anyway.  Log state transitions
  //    while waiting so we can see what the host is doing.
  // ──────────────────────────────────────────────────────────
  #define GIP_FALLBACK_MS 5000
  if (gip_announced && gip_state != GIP_STATE_READY) {
    static uint32_t last_wait_log_ms = 0;
    uint32_t now_wait = millis();

    if (now_wait - last_wait_log_ms >= 1000) {
      last_wait_log_ms = now_wait;
      const char *st = (gip_state == GIP_STATE_ANNOUNCED) ? "ANNOUNCED" :
                       (gip_state == GIP_STATE_IDENTIFIED) ? "IDENTIFIED" : "IDLE";
      Serial.printf("[WAIT] %lums  state=%s  fifo_free=%lu  rx_avail=%d  out_pkts=%lu\r\n",
                    now_wait - gip_mounted_ms, st,
                    tud_vendor_write_available(),
                    (int)tud_vendor_available(),
                    gip_out_pkt_count);
    }

    if (now_wait - gip_mounted_ms >= GIP_FALLBACK_MS) {
      gip_state = GIP_STATE_READY;
      Serial.printf(
        "[GIP] %dms fallback — starting input (handshake incomplete)\r\n",
        GIP_FALLBACK_MS);
    }
  }

  // ──────────────────────────────────────────────────────────
  // 7. Periodic status dump every 5 s (section 7, was renumbered)
  // ──────────────────────────────────────────────────────────
  uint32_t now_ms = millis();
  if (now_ms - last_status_ms >= 5000) {
    last_status_ms = now_ms;
    Serial.printf(
      "[STATUS] up=%lus pkt=%lu crc_e=%lu radio_st=%lu\r\n",
      now_ms / 1000, rx_count, crc_errors, NRF_RADIO->STATE);
    const char *state_str = (gip_state == GIP_STATE_IDLE)       ? "IDLE"
                           : (gip_state == GIP_STATE_ANNOUNCED) ? "ANNOUNCED"
                           : (gip_state == GIP_STATE_IDENTIFIED)? "IDENTIFIED"
                           :                                       "READY";
    Serial.printf(
      "  vendor=%d announced=%d state=%s seq=%d\r\n",
      (int)tud_vendor_mounted(), (int)gip_announced,
      state_str, gip_seq);
    Serial.printf(
      "  fifo_free=%lu  rx_avail=%d  OUT_pkts=%lu\r\n",
      tud_vendor_write_available(), (int)tud_vendor_available(),
      gip_out_pkt_count);
  }
}
