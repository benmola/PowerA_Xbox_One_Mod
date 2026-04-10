/*
 * tusb_config.h — TinyUSB configuration for Pico USB Host with XInput driver
 *
 * The Pico's native USB port acts as a USB HOST.
 * The controller plugs directly into the Pico's USB-C port.
 * Debug serial goes over UART (GP4/GP5), not USB.
 *
 * Power: The Pico must be powered externally (e.g. via VSYS pin)
 * since its USB-C port is in host mode providing 5V to the controller.
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Common
//--------------------------------------------------------------------
#define CFG_TUSB_MCU          OPT_MCU_RP2040
#define CFG_TUSB_OS           OPT_OS_PICO
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0  // 0 = off, 1 = basic debug (interleaves with printf!)
#endif

//--------------------------------------------------------------------
// Host mode only — native USB controller
//--------------------------------------------------------------------
#define CFG_TUH_ENABLED       1
#define CFG_TUD_ENABLED       0  // No device mode

#define CFG_TUH_MAX_SPEED     OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 512  // Must be >321 for Xbox 360 wireless

//--------------------------------------------------------------------
// Host class drivers
// Disable all standard classes — we only use xinput
//--------------------------------------------------------------------
#define CFG_TUH_HUB           0
#define CFG_TUH_CDC           0
#define CFG_TUH_HID           0
#define CFG_TUH_MSC           0
#define CFG_TUH_VENDOR        0

//--------------------------------------------------------------------
// XInput driver (Ryzee119/tusb_xinput)
//--------------------------------------------------------------------
#define CFG_TUH_XINPUT        4  // Support up to 4 XInput interfaces

//--------------------------------------------------------------------
// Endpoint buffer sizes
//--------------------------------------------------------------------
#define CFG_TUH_XINPUT_EPIN_BUFSIZE   64
#define CFG_TUH_XINPUT_EPOUT_BUFSIZE  64

#ifdef __cplusplus
}
#endif

#endif // TUSB_CONFIG_H
