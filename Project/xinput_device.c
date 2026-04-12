/*
 * xinput_device.c — Xbox 360 XInput USB Device Class Driver for TinyUSB
 *
 * Implements a custom TinyUSB device class driver that makes the NRF52840
 * appear as a Microsoft Xbox 360 Controller to the host PC.
 *
 * Key details:
 *   - VID=0x045E (Microsoft), PID=0x028E (Xbox 360 Controller)
 *   - Interface class 0xFF, subclass 0x5D, protocol 0x01
 *   - Includes the 17-byte vendor descriptor Windows needs for xusb22.sys
 *   - IN endpoint: 32 bytes, sends 20-byte input reports
 *   - OUT endpoint: 32 bytes, receives rumble commands (ignored for now)
 *
 * Registered with TinyUSB via usbd_app_driver_get_cb() in the main sketch.
 */

#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "xinput_device.h"

//--------------------------------------------------------------------
// Internal state
//--------------------------------------------------------------------
typedef struct {
    uint8_t ep_in;       // IN endpoint address
    uint8_t ep_out;      // OUT endpoint address
    uint8_t itf_num;     // Interface number
    bool    mounted;     // Device is configured and ready
    uint8_t out_buf[32]; // Buffer for OUT endpoint (rumble data)
} xinputd_interface_t;

static xinputd_interface_t _xinputd;

//--------------------------------------------------------------------
// USB Descriptors
//--------------------------------------------------------------------

// Xbox 360 vendor-specific descriptor (17 bytes)
// This is the magic block that tells Windows to load xusb22.sys
static uint8_t const xinput_vendor_desc[] = {
    0x11,       // bLength = 17
    0x21,       // bDescriptorType = 0x21 (vendor specific)
    0x00, 0x01, // bcdXID = 0x0100
    0x01,       // bType = 1 (gamepad)
    0x25,       // bSubType = 0x25 (gamepad subtype)
    0x81,       // wMaxInputReportSize LSB (NOTE: this is bEndpointAddressIn disguised)
    0x14,       // wMaxInputReportSize MSB = 20 bytes
    0x03, 0x00, // wMaxOutputReportSize = 3 (unused, but must be present)
    0x03, 0x00, // wAlternate1
    0x03, 0x00, // wAlternate2
    0x03, 0x00, // wAlternate3
    0x03,       // wAlternate4 (partial)
};

// Full configuration descriptor for Xbox 360 controller
// Interface 0: XInput gamepad (class 0xFF, subclass 0x5D, protocol 0x01)
//   - IN endpoint (interrupt, 32 bytes)
//   - OUT endpoint (interrupt, 32 bytes)
// Interface 1: Headset (stubbed, required for Windows compatibility)
//   - No endpoints
// Interface 2: Unknown IF 2 (stubbed, required for Windows compatibility)  
//   - No endpoints
// Interface 3: Security (stubbed, Xbox 360 auth — not checked on PC)
//   - No endpoints

#define XINPUT_CONFIG_TOTAL_LEN (9 + 9 + 17 + 7 + 7 + 9 + 9 + 9)

// Endpoint numbers
#define EPNUM_XINPUT_IN   0x01
#define EPNUM_XINPUT_OUT  0x01

static uint8_t const xinput_config_descriptor[] = {
    // Configuration Descriptor (9 bytes)
    0x09,                   // bLength
    0x02,                   // bDescriptorType = Configuration
    XINPUT_CONFIG_TOTAL_LEN & 0xFF, (XINPUT_CONFIG_TOTAL_LEN >> 8) & 0xFF, // wTotalLength
    0x04,                   // bNumInterfaces = 4
    0x01,                   // bConfigurationValue
    0x00,                   // iConfiguration
    0x80,                   // bmAttributes = bus powered
    0xFA,                   // bMaxPower = 500mA

    // ---- Interface 0: XInput Gamepad ----
    // Interface Descriptor (9 bytes)
    0x09,                   // bLength
    0x04,                   // bDescriptorType = Interface
    0x00,                   // bInterfaceNumber = 0
    0x00,                   // bAlternateSetting
    0x02,                   // bNumEndpoints = 2
    0xFF,                   // bInterfaceClass = Vendor
    0x5D,                   // bInterfaceSubClass = XInput
    0x01,                   // bInterfaceProtocol = Gamepad
    0x00,                   // iInterface

    // Xbox 360 vendor descriptor (17 bytes)
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14,
    0x03, 0x00, 0x03, 0x00, 0x03, 0x00, 0x03, 0x00,
    0x03,

    // IN Endpoint (7 bytes)
    0x07,                   // bLength
    0x05,                   // bDescriptorType = Endpoint
    0x80 | EPNUM_XINPUT_IN, // bEndpointAddress = IN 1
    0x03,                   // bmAttributes = Interrupt
    0x20, 0x00,             // wMaxPacketSize = 32
    0x04,                   // bInterval = 4ms

    // OUT Endpoint (7 bytes)
    0x07,                   // bLength
    0x05,                   // bDescriptorType = Endpoint
    EPNUM_XINPUT_OUT,       // bEndpointAddress = OUT 1
    0x03,                   // bmAttributes = Interrupt
    0x20, 0x00,             // wMaxPacketSize = 32
    0x08,                   // bInterval = 8ms

    // ---- Interface 1: Headset (stub) ----
    0x09, 0x04, 0x01, 0x00, 0x00, 0xFF, 0x5D, 0x03, 0x00,

    // ---- Interface 2: Unknown (stub) ----
    0x09, 0x04, 0x02, 0x00, 0x00, 0xFF, 0x5D, 0x02, 0x00,

    // ---- Interface 3: Security (stub) ----
    0x09, 0x04, 0x03, 0x00, 0x00, 0xFF, 0xFD, 0x13, 0x00,
};

// Device Descriptor (18 bytes)
static uint8_t const xinput_device_descriptor[] = {
    0x12,                   // bLength
    0x01,                   // bDescriptorType = Device
    0x00, 0x02,             // bcdUSB = 2.00
    0xFF,                   // bDeviceClass = Vendor
    0xFF,                   // bDeviceSubClass
    0xFF,                   // bDeviceProtocol
    0x40,                   // bMaxPacketSize0 = 64
    0x5E, 0x04,             // idVendor = 0x045E (Microsoft)
    0x8E, 0x02,             // idProduct = 0x028E (Xbox 360 Controller)
    0x14, 0x01,             // bcdDevice = 1.14
    0x01,                   // iManufacturer
    0x02,                   // iProduct
    0x03,                   // iSerialNumber
    0x01,                   // bNumConfigurations
};

// String Descriptors
static uint16_t const string_desc_langid[] = { 0x0304, 0x0409 };
static uint16_t const string_desc_manufacturer[] = { 0x0310, 'X','b','o','x',' ','3','6','0' };
static uint16_t const string_desc_product[] = { 0x033C, 'C','o','n','t','r','o','l','l','e','r',' ',
    '(','X','b','o','x',' ','3','6','0',' ','W','i','r','e','l','e','s','s',' ','R','e','c','v',')' };

//--------------------------------------------------------------------
// TinyUSB Device Descriptor Callbacks
// (These override TinyUSB's default descriptor callbacks)
//--------------------------------------------------------------------

uint8_t const *tud_descriptor_device_cb(void) {
    return xinput_device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return xinput_config_descriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    switch (index) {
        case 0: return string_desc_langid;
        case 1: return string_desc_manufacturer;
        case 2: return string_desc_product;
        default: return NULL;
    }
}

//--------------------------------------------------------------------
// TinyUSB Class Driver Interface
//--------------------------------------------------------------------

// Called during TinyUSB init
static void xinputd_init(void) {
    memset(&_xinputd, 0, sizeof(_xinputd));
}

// Called during reset
static void xinputd_reset(uint8_t rhport) {
    (void)rhport;
    memset(&_xinputd, 0, sizeof(_xinputd));
}

// Called when host selects our configuration
// We need to open our endpoints here
static uint16_t xinputd_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    // Only claim interface 0 (the gamepad interface)
    if (itf_desc->bInterfaceClass != 0xFF ||
        itf_desc->bInterfaceSubClass != 0x5D ||
        itf_desc->bInterfaceProtocol != 0x01) {
        return 0; // Not our interface
    }

    uint16_t drv_len = 0;
    uint8_t const *p_desc = (uint8_t const *)itf_desc;

    // Interface descriptor
    _xinputd.itf_num = itf_desc->bInterfaceNumber;
    drv_len += tu_desc_len(p_desc);
    p_desc += tu_desc_len(p_desc);

    // Skip the vendor descriptor (17 bytes, type 0x21)
    if (tu_desc_type(p_desc) == 0x21) {
        drv_len += tu_desc_len(p_desc);
        p_desc += tu_desc_len(p_desc);
    }

    // Open endpoints
    uint8_t found_eps = 0;
    while (found_eps < 2 && drv_len < max_len) {
        if (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT) break;

        tusb_desc_endpoint_t const *ep_desc = (tusb_desc_endpoint_t const *)p_desc;
        TU_ASSERT(usbd_edpt_open(rhport, ep_desc), 0);

        if (tu_edpt_dir(ep_desc->bEndpointAddress) == TUSB_DIR_IN) {
            _xinputd.ep_in = ep_desc->bEndpointAddress;
        } else {
            _xinputd.ep_out = ep_desc->bEndpointAddress;
            // Start listening for OUT data (rumble)
            usbd_edpt_xfer(rhport, _xinputd.ep_out, _xinputd.out_buf, sizeof(_xinputd.out_buf));
        }

        found_eps++;
        drv_len += tu_desc_len(p_desc);
        p_desc += tu_desc_len(p_desc);
    }

    _xinputd.mounted = true;
    return drv_len;
}

// Called on SET_INTERFACE or device configured
static bool xinputd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void)rhport;
    (void)stage;
    (void)request;
    return true; // Accept all control transfers
}

// Called when an endpoint transfer completes
static bool xinputd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;
    (void)xferred_bytes;

    if (ep_addr == _xinputd.ep_out) {
        // Rumble data received — ignore for now, re-queue
        usbd_edpt_xfer(rhport, _xinputd.ep_out, _xinputd.out_buf, sizeof(_xinputd.out_buf));
    }

    return true;
}

//--------------------------------------------------------------------
// Class driver struct — registered with TinyUSB
//--------------------------------------------------------------------
usbd_class_driver_t const xinput_device_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "XINPUT",
#endif
    .init            = xinputd_init,
    .reset           = xinputd_reset,
    .open            = xinputd_open,
    .control_xfer_cb = xinputd_control_xfer_cb,
    .xfer_cb         = xinputd_xfer_cb,
    .sof             = NULL,
};

//--------------------------------------------------------------------
// Public API
//--------------------------------------------------------------------

bool xinputd_ready(void) {
    return _xinputd.mounted && _xinputd.ep_in != 0;
}

bool xinputd_send_report(xinput_report_t const *report) {
    if (!xinputd_ready()) return false;

    // Check if endpoint is free
    if (usbd_edpt_busy(0, _xinputd.ep_in)) return false;

    return usbd_edpt_xfer(0, _xinputd.ep_in, (uint8_t *)report, sizeof(xinput_report_t));
}
