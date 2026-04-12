/*
 * xinput_device.h — Xbox 360 XInput USB Device Driver for TinyUSB
 *
 * Emulates a Microsoft Xbox 360 Controller (VID=0x045E, PID=0x028E).
 * Windows automatically loads the xusb22.sys XInput driver.
 * Works with every XInput-compatible game, no extra drivers needed.
 *
 * Usage:
 *   1. Include this header in your main sketch
 *   2. TinyUSB discovers the driver via usbd_app_driver_get_cb()
 *   3. Call xinputd_send_report() to send gamepad data
 */

#ifndef XINPUT_DEVICE_H
#define XINPUT_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "device/usbd.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Xbox 360 XInput report (20 bytes) — sent on IN endpoint
//--------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint8_t  report_id;     // 0x00 always
    uint8_t  report_size;   // 0x14 (20 bytes) always
    uint16_t wButtons;      // Button bitfield (see defines below)
    uint8_t  bLeftTrigger;  // 0-255
    uint8_t  bRightTrigger; // 0-255
    int16_t  sThumbLX;      // -32768 to 32767
    int16_t  sThumbLY;      // -32768 to 32767
    int16_t  sThumbRX;      // -32768 to 32767
    int16_t  sThumbRY;      // -32768 to 32767
    uint8_t  reserved[6];   // Padding to 20 bytes
} xinput_report_t;

//--------------------------------------------------------------------
// Button defines (identical to standard XInput wButtons layout)
//--------------------------------------------------------------------
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_GUIDE          0x0400
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

//--------------------------------------------------------------------
// Public API
//--------------------------------------------------------------------

// Send a 20-byte XInput report to the host PC.
// Returns true if the report was queued successfully.
bool xinputd_send_report(xinput_report_t const *report);

// Check if the device is ready to send (host has configured us).
bool xinputd_ready(void);

// The TinyUSB class driver struct — registered via usbd_app_driver_get_cb()
extern usbd_class_driver_t const xinput_device_driver;

#ifdef __cplusplus
}
#endif

#endif // XINPUT_DEVICE_H
