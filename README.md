# PowerA Xbox One Controller — Wireless Mod

Convert a **wired PowerA Xbox One controller** into a wireless gamepad using a
Raspberry Pi Pico (RP2040) as a USB host radio transmitter and an
NRF52840-based board as the radio receiver / USB XInput device.  No
modifications to the controller PCB are required.

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Architecture](#2-architecture)
3. [Hardware](#3-hardware)
   - 3.1 [Transmitter — Raspberry Pi Pico (RP2040)](#31-transmitter--raspberry-pi-pico-rp2040)
   - 3.2 [NRF24L01+ Radio Module (x2)](#32-nrf24l01-radio-module-x2)
   - 3.3 [Receiver — Nice!Nano (NRF52840)](#33-receiver--nicenano-nrf52840)
   - 3.4 [Optional: Battery & Power (TP5400)](#34-optional-battery--power-tp5400)
4. [Wiring](#4-wiring)
   - 4.1 [Transmitter Side](#41-transmitter-side)
   - 4.2 [Receiver Side](#42-receiver-side)
5. [Radio Protocol](#5-radio-protocol)
   - 5.1 [ESB Configuration](#51-esb-configuration)
   - 5.2 [Bit-Reversal (MSBit vs LSBit)](#52-bit-reversal-msbit-vs-lsbit)
   - 5.3 [Gamepad Payload (14 bytes)](#53-gamepad-payload-14-bytes)
   - 5.4 [Rumble Return Payload (14 bytes)](#54-rumble-return-payload-14-bytes)
   - 5.5 [Timing](#55-timing)
6. [USB: Controller to Pico (tusb_xinput)](#6-usb-controller-to-pico-tusb_xinput)
   - 6.1 [Driver Overview](#61-driver-overview)
   - 6.2 [xinput_gamepad_t Struct](#62-xinput_gamepad_t-struct)
   - 6.3 [Callback API](#63-callback-api)
   - 6.4 [TinyUSB Configuration](#64-tinyusb-configuration)
7. [USB: NRF52840 to PC (XInput)](#7-usb-nrf52840-to-pc-xinput)
   - 7.1 [Why Xbox 360 XInput](#71-why-xbox-360-xinput)
   - 7.2 [USB Descriptor Layout](#72-usb-descriptor-layout)
   - 7.3 [Input Report Format (20 bytes)](#73-input-report-format-20-bytes)
   - 7.4 [Rumble Output Report (8 bytes)](#74-rumble-output-report-8-bytes)
8. [Vibration / Rumble System](#8-vibration--rumble-system)
9. [Build System](#9-build-system)
10. [Flashing](#10-flashing)
11. [Debug Serial Output](#11-debug-serial-output)
12. [LED Behaviour](#12-led-behaviour)
13. [File Structure](#13-file-structure)
14. [Troubleshooting](#14-troubleshooting)
15. [References](#15-references)

---

## 1. System Overview

A PowerA wired Xbox One controller (GIP protocol) plugs into the Pico's USB-C
port in **host mode**.  The Pico reads its button/stick/trigger state at up to
62 Hz, packs it into a compact 14-byte payload, and transmits it wirelessly via
an NRF24L01+ module over a 2.4 GHz Enhanced ShockBurst (ESB) radio link.

On the PC side, a Nice!Nano (NRF52840) receives the radio packets and presents
itself to Windows as a **wired Xbox 360 controller** (VID `0x045E` /
PID `0x028E`).  Windows binds the built-in `xusb22.sys` driver — no additional
drivers or authentication hardware are required.  Vibration commands sent by
games travel in the reverse direction: NRF52840 → radio → Pico → controller.

```
  [PowerA Controller]
        USB-C (GIP)
             |
      [Raspberry Pi Pico]         2.4 GHz ESB       [Nice!Nano NRF52840]
      RP2040  USB host     <-- NRF24L01+ ~~~ NRF52840 built-in radio -->  USB
      tusb_xinput driver                                               XInput
      SPI → NRF24L01+                                        Xbox 360 emulation
                                                                       |
                                                                [Windows PC]
                                                             xusb22.sys / XInput
```

---

## 2. Architecture

```
TRANSMITTER SIDE (inside/attached to controller)
------------------------------------------------
PowerA Controller
  |  USB-C OTG
  |  GIP protocol (vendor class 0xFF / 0x47 / 0xD0)
  v
Raspberry Pi Pico (RP2040)
  |
  +-- TinyUSB Host (native USB hardware, no external PHY)
  |     tusb_xinput class driver (Ryzee119)
  |     Parses GIP/Xbox360 reports into xinput_gamepad_t
  |     Forwards rumble commands from NRF back to controller
  |
  +-- NRF radio link (SPI0, 2 MHz)
        NRF24L01+ module
        ESB TX: E7:E7:E7:E7:E7, ch 2, 2 Mbps, no Auto-ACK
        ESB RX pipe 1: 4B:4B:4B:4B:4B (rumble return channel)
        Heartbeat TX every 16 ms
        RX window 500 us after each TX
        ~~~~~~~ 2.4 GHz ~~~~~~~

RECEIVER SIDE (USB dongle, plugs into PC)
------------------------------------------
Nice!Nano (NRF52840)
  |
  +-- NRF52840 built-in RADIO peripheral
  |     Nrf_2Mbit mode (ESB compatible)
  |     RX address: E7:E7:E7:E7:E7, ch 2
  |     TX address: D2:D2:D2:D2:D2 (rumble return)
  |     Gamepad packets received, decoded into input cache
  |     Rumble payloads broadcast back at 20 Hz while active
  |
  +-- USB (Adafruit TinyUSB, XInput vendor class override)
        VID 0x045E / PID 0x028E  (Xbox 360 wired controller)
        Interface: Class 0xFF, SubClass 0x5D, Protocol 0x01
        IN EP  0x81  Interrupt  32 bytes  4 ms poll
        OUT EP 0x01  Interrupt  32 bytes  8 ms poll
        Windows xusb22.sys binds automatically
```

---

## 3. Hardware

### 3.1 Transmitter — Raspberry Pi Pico (RP2040)

| Property | Value |
|---|---|
| MCU | RP2040 dual-core ARM Cortex-M0+ @ 133 MHz |
| Flash | 2 MB |
| SRAM | 264 KB |
| USB | Native USB 1.1 full-speed (Host **or** Device — not both) |
| SPI | 2× hardware SPI (SPI0 used here) |
| UART | 2× hardware UART (UART0 on GP0/GP1 for debug) |
| Power input | VSYS pin (1.8 V – 5.5 V) — **required** when USB-C is in host mode |
| Board variant | USB-C model required; micro-USB Pico lacks the OTG ID pin |

> **Critical:** When the USB-C port operates as a USB host it provides 5 V to
> the controller on VBUS.  The Pico cannot be powered through that same port.
> External 5 V must be fed into **VSYS (pin 39)**.

**Pin assignments:**

| Pico Pin | GPIO | Function |
|---|---|---|
| 1 | GP0 | UART0 TX — debug serial output |
| 2 | GP1 | UART0 RX — debug serial input |
| 4 | GP2 | SPI0 SCK → NRF24L01+ SCK |
| 5 | GP3 | SPI0 MOSI → NRF24L01+ MOSI |
| 6 | GP4 | SPI0 MISO ← NRF24L01+ MISO |
| 21 | GP16 | NRF24L01+ CE (chip enable / RX-TX control) |
| 22 | GP17 | NRF24L01+ CSN (SPI chip select, active low) |
| 36 | 3V3 OUT | NRF24L01+ VCC (**3.3 V only — never 5 V**) |
| 38 | GND | Common ground |
| 39 | VSYS | External 5 V power input |
| — | USB-C | Controller (via USB-C to USB-A female OTG adapter) |
| 25 | GP25 | Onboard LED (status indicator) |

Reference pinout diagrams: [`docs/pico_pinout.png`](docs/pico_pinout.png) · [`docs/pico_rp2040.jfif`](docs/pico_rp2040.jfif)

---

### 3.2 NRF24L01+ Radio Module (x2)

One module sits on the transmitter (Pico side), one on the receiver (but the
Nice!Nano has the NRF52840's built-in radio, so no external module is needed
there — see §3.3).

| Property | Value |
|---|---|
| Band | 2.4 GHz ISM |
| Data rate | 250 kbps / 1 Mbps / **2 Mbps** (configured at 2 Mbps) |
| Channel | 0–125 (configured at **channel 2** = 2402 MHz) |
| Protocol | Enhanced ShockBurst (ESB) |
| CRC | 8-bit or **16-bit** (configured at 16-bit, 2 bytes) |
| Supply voltage | **3.3 V strict** — 5 V will destroy the module |
| SPI clock | Up to 10 MHz (configured at 2 MHz for compatibility) |
| Address width | 3–5 bytes (configured at **5 bytes**) |
| Payload size | Fixed **14 bytes** |

**Physical pin layout (top-view, 2×4 header):**

```
  (Antenna end)
  +---------+
  | IRQ  MISO |   IRQ  = not connected
  | MOSI SCK  |   MOSI = Pico GP3
  | CSN  CE   |   SCK  = Pico GP2
  | VCC  GND  |   CSN  = Pico GP17
  +---------+   CE   = Pico GP16
                VCC  = Pico 3V3 (pin 36)
                GND  = Pico GND
                MISO = Pico GP4
```

See [`docs/nrf24l01_pinout.png`](docs/nrf24l01_pinout.png) and
[`docs/nrf24l01_dip_pinout.png`](docs/nrf24l01_dip_pinout.png) for labelled
photographs.

> **Tip:** Place a 10–100 µF capacitor across VCC and GND as close to the
> module as possible.  The NRF24L01+ draws sharp current spikes during TX and
> power supply droop causes spurious resets.

---

### 3.3 Receiver — Nice!Nano (NRF52840)

The Nice!Nano is a small Pro-Micro-footprint board built around the Nordic
NRF52840 SoC ([`docs/nicenano_nrf52840.jfif`](docs/nicenano_nrf52840.jfif)).
It has a built-in 2.4 GHz radio (identical silicon to the
NRF52840 inside Microsoft's Xbox One S wireless dongle) so no external radio
module is required.

| Property | Value |
|---|---|
| MCU | NRF52840 ARM Cortex-M4F @ 64 MHz |
| Flash | 1 MB |
| RAM | 256 KB |
| Radio | Built-in 2.4 GHz (Bluetooth / proprietary ESB-compatible) |
| USB | Full-speed USB 2.0 (native, no external PHY) |
| Operating voltage | 3.3 V (on-board regulator from USB 5 V) |

**Connections (receiver dongle):**

| Nice!Nano Pin | NRF52840 Port | Function |
|---|---|---|
| USB | — | Data + power to PC (XInput gamepad) |
| D11 | P0.06 | UART1 TX — debug serial output (optional) |
| D30 | P0.22 | UART1 RX — debug serial input (optional) |
| LED_BUILTIN | P0.15 | Status LED |

The radio is configured entirely in firmware using the NRF52840 `RADIO`
peripheral registers directly (no SoftDevice, no BLE stack).

---

### 3.4 Optional: Battery & Power (TP5400)

For a fully wireless build with no USB cable, a LiPo battery and charge/boost
module can be added.

| Component | Role |
|---|---|
| 603040 LiPo (3.7 V, ~600 mAh) | Energy storage |
| TP5400 or 134N3P module | Charges from USB-C; steps 3.7 V up to 5 V |

```
 LiPo (+) ──► TP5400 B+          5V OUT ──► Pico VSYS
 LiPo (-) ──► TP5400 B-          GND    ──► Pico GND
              TP5400 USB-C IN  (charging input)
```

> **Current status:** The battery circuit is bypassed in the current
> prototyping phase.  The Pico is powered directly from a USB 5 V source on
> VSYS.  Re-wire the TP5400 for the final build.

---

## 4. Wiring

### 4.1 Transmitter Side

Full wiring for the Pico + NRF24L01+ assembly:

```
                     Raspberry Pi Pico
                  +--------------------+
  USB-TTL RX <---| GP0 (UART TX)   VSYS|---> 5V power in
                 | GP1 (UART RX)    GND|---> GND
                 | GP2 (SPI SCK)    3V3|---> NRF VCC
                 | GP3 (SPI MOSI)      |
                 | GP4 (SPI MISO)      |
                 |                     |
                 | GP16 (CE)           |
                 | GP17 (CSN)          |
                 | GND                 |
                 +--------------------+
                    |   |   |   |   |
                   SCK MOSI MISO CE CSN
                    |   |   |   |   |
                 +--------------------+
                 |  NRF24L01+ module  |
                 |  VCC=3.3V  GND=GND |
                 +--------------------+

  Controller ──USB-C OTG──► Pico USB-C port  (Pico provides 5V to controller)
```

### 4.2 Receiver Side

The Nice!Nano only needs a USB cable to the PC.  No external wiring is required
for normal operation.  For debug serial:

```
  Nice!Nano D11 (P0.06 TX) ──► USB-TTL RX
  Nice!Nano D30 (P0.22 RX) ──► USB-TTL TX
  Nice!Nano GND            ──► USB-TTL GND
  Nice!Nano USB            ──► PC  (XInput gamepad)
```

---

## 5. Radio Protocol

### 5.1 ESB Configuration

Both ends must use **identical** settings.  Any mismatch = no communication.

| Parameter | Value | Register / Note |
|---|---|---|
| Frequency | 2402 MHz | `REG_RF_CH = 2` |
| Data rate | 2 Mbps | `REG_RF_SETUP bit3 = 1` |
| TX power | 0 dBm | `REG_RF_SETUP bits[2:1] = 11` |
| CRC | 16-bit (2 bytes) | `NRF_RADIO->CRCCNF = Two` |
| CRC polynomial | 0x11021 (CRC-CCITT) | `NRF_RADIO->CRCPOLY` |
| CRC initial value | 0xFFFF | `NRF_RADIO->CRCINIT` |
| Auto-ACK | Disabled | `REG_EN_AA = 0x00` |
| Auto-retransmit | Disabled | `REG_SETUP_RETR = 0x00` |
| Address width | 5 bytes | `REG_SETUP_AW = 0x03` |
| Payload size | 14 bytes (fixed) | `REG_RX_PW_P0/P1 = 14` |
| Gamepad TX address | `E7:E7:E7:E7:E7` | Pico → NRF52840 |
| Rumble TX address | `D2:D2:D2:D2:D2` | NRF52840 → Pico |

**Address explanation:**

The NRF52840 `RADIO` peripheral in `Nrf_2Mbit` mode transmits bytes
**LSBit-first** on air.  The NRF24L01+ always transmits **MSBit-first**.  The
bit-reversal mapping is:

| On-air value | NRF52840 register | NRF24L01+ register | Byte (hex) |
|---|---|---|---|
| Gamepad channel | BASE0=`0xE7E7E7E7`, PREFIX=`0xE7` | TX/RX P0 = `E7:E7:E7:E7:E7` | 0xE7 is self-symmetric under bit-reversal |
| Rumble channel | BASE0=`0xD2D2D2D2`, PREFIX=`0xD2` | RX P1 = `4B:4B:4B:4B:4B` | reverse_bits(0xD2) = 0x4B |

0xE7 = `1110 0111` → reversed = `1110 0111` = 0xE7 (symmetric, so no correction needed for gamepad channel).
0xD2 = `1101 0010` → reversed = `0100 1011` = 0x4B (Pico must use 0x4B for pipe 1).

---

### 5.2 Bit-Reversal (MSBit vs LSBit)

The NRF52840 `RADIO` peripheral in `Nrf_2Mbit` mode clocks each byte out
**LSBit-first**.  The NRF24L01+ always clocks bytes **MSBit-first**.  This
means every payload byte is received with its bits reversed.

The Pico compensates by reversing each byte *before* transmitting:

```c
// main.c — applied to every TX byte
static inline uint8_t reverse_bits(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));  // swap nibbles
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));  // swap pairs
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));  // swap bits
    return b;
}
```

The NRF52840 sees the original un-reversed values after reception.

For the **rumble return** direction (NRF52840 → Pico), the NRF52840 sends raw
values (no reversal) and the Pico calls `reverse_bits()` on each received byte
after reading from the FIFO to recover the original values.

---

### 5.3 Gamepad Payload (14 bytes)

Sent from Pico → NRF52840 at every heartbeat (16 ms) and on any input change.

| Byte(s) | Field | Type | Range |
|---|---|---|---|
| 0 | Buttons low byte | uint8 | XInput bitmask (see below) |
| 1 | Buttons high byte | uint8 | XInput bitmask (see below) |
| 2–3 | Left stick X | int16 LE | −32768 .. +32767 |
| 4–5 | Left stick Y | int16 LE | −32768 .. +32767 |
| 6–7 | Right stick X | int16 LE | −32768 .. +32767 |
| 8–9 | Right stick Y | int16 LE | −32768 .. +32767 |
| 10 | Left trigger | uint8 | 0–255 |
| 11 | Right trigger | uint8 | 0–255 |
| 12–13 | Padding | — | 0x00 |

**Button bitmask (wButtons, 16-bit):**

| Bit | Mask | Button |
|---|---|---|
| 0 | 0x0001 | D-pad Up |
| 1 | 0x0002 | D-pad Down |
| 2 | 0x0004 | D-pad Left |
| 3 | 0x0008 | D-pad Right |
| 4 | 0x0010 | Start / Menu |
| 5 | 0x0020 | Back / View |
| 6 | 0x0040 | Left thumbstick click (L3) |
| 7 | 0x0080 | Right thumbstick click (R3) |
| 8 | 0x0100 | Left Bumper (LB) |
| 9 | 0x0200 | Right Bumper (RB) |
| 10 | 0x0400 | Guide / Xbox button |
| 11 | 0x0800 | Share (if present) |
| 12 | 0x1000 | A |
| 13 | 0x2000 | B |
| 14 | 0x4000 | X |
| 15 | 0x8000 | Y |

This layout matches the XInput `XINPUT_GAMEPAD` struct directly so the
NRF52840 receiver can split it into Xbox 360 report bytes 2–3 without any
remapping.

---

### 5.4 Rumble Return Payload (14 bytes)

Sent from NRF52840 → Pico whenever a game triggers vibration.

| Byte | Field | Notes |
|---|---|---|
| 0 | Left motor (big) intensity | 0 = off, 255 = full |
| 1 | Right motor (small) intensity | 0 = off, 255 = full |
| 2–13 | Padding | 0x00 |

The NRF52840 transmits 3 back-to-back copies per broadcast attempt (~400 µs
total) for redundancy.

---

### 5.5 Timing

```
Pico timeline (1 iteration = 16 ms):
  |<--------- 16 ms heartbeat -------->|
  TX gamepad packet (~250 µs on-air)
                    |<-- 500 µs RX window --|
                    Listen on pipe 1 (rumble addr)
                                            Return to TX mode

NRF52840 timeline:
  Continuous RX on E7 address
  On valid CRC packet received:
    If rumble_active && time since last rumble TX >= 50 ms:
      Disable RX
      Switch to D2 TX address
      Send rumble payload × 3  (~400 µs)
      Restore E7 RX address
      Restart RX

  (50 ms = 20 Hz broadcast rate while rumble is active)
  (300 ms drain: keeps sending 0,0 after game stops, then goes quiet)

Pico rumble safety timeout:
  If rumble_big/small != 0 and no rumble packet received for 1500 ms:
    Force set_rumble(0, 0) → stop motors
    (guards against radio loss mid-vibration)
```

---

## 6. USB: Controller to Pico (tusb_xinput)

### 6.1 Driver Overview

`lib/tusb_xinput` is Ryzee119's open-source TinyUSB class driver.  It handles:

- **Xbox One (GIP):** Full GIP handshake (ANNOUNCE / IDENTIFY / POWER ON),
  input report parsing, rumble via GIP CMD 0x09
- **Xbox 360 Wired:** Direct XInput report parsing, LED / rumble OUT reports
- **Xbox 360 Wireless:** Connection packet handshake, wireless-specific framing
- **Original Xbox:** Legacy XID protocol

The driver is registered via the required TinyUSB hook:

```c
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}
```

### 6.2 xinput_gamepad_t Struct

```c
typedef struct xinput_gamepad {
    uint16_t wButtons;      // button bitmask (see §5.3)
    uint8_t  bLeftTrigger;  // left  trigger 0–255
    uint8_t  bRightTrigger; // right trigger 0–255
    int16_t  sThumbLX;      // left  stick X −32768..+32767
    int16_t  sThumbLY;      // left  stick Y −32768..+32767
    int16_t  sThumbRX;      // right stick X −32768..+32767
    int16_t  sThumbRY;      // right stick Y −32768..+32767
} xinput_gamepad_t;
```

Available inside `xinputh_interface_t.pad` when `new_pad_data` is set.

### 6.3 Callback API

All callbacks are implemented in `main.c`:

| Callback | When called | Used for |
|---|---|---|
| `tuh_xinput_mount_cb` | Controller plugged in and enumerated | Store dev_addr/instance, init LED, start report flow |
| `tuh_xinput_umount_cb` | Controller unplugged | Clear state, update LED |
| `tuh_xinput_report_received_cb` | New input report from controller | Parse pad, transmit radio packet, re-arm receive |

**Utility functions:**

| Function | Description |
|---|---|
| `tuh_xinput_receive_report(addr, inst)` | Re-arm the IN endpoint to receive the next report |
| `tuh_xinput_send_report(addr, inst, buf, len)` | Send raw bytes to controller OUT endpoint |
| `tuh_xinput_set_rumble(addr, inst, left, right, block)` | Set motor intensities (0–255 each) |
| `tuh_xinput_set_led(addr, inst, quadrant, block)` | Set Xbox 360 LED ring position |

For **Xbox One** controllers, `tuh_xinput_set_rumble` internally constructs a
GIP rumble command (cmd `0x09`) with a built-in duration.  The NRF52840's
20 Hz periodic broadcast acts as the required keep-alive refresh so motors
stay spinning as long as the game requests it.

### 6.4 TinyUSB Configuration

`tusb_config.h` configures TinyUSB for **host-only** operation:

| Setting | Value | Meaning |
|---|---|---|
| `CFG_TUH_ENABLED` | 1 | Host mode on |
| `CFG_TUD_ENABLED` | 0 | Device mode off |
| `CFG_TUH_MAX_SPEED` | FULL_SPEED | USB 1.1 full-speed (12 Mbps) |
| `CFG_TUH_ENUMERATION_BUFSIZE` | 512 | Must be ≥321 for Xbox 360 Wireless |
| `CFG_TUH_HID/CDC/MSC/VENDOR` | 0 | All standard classes disabled |
| `CFG_TUH_XINPUT` | 4 | Up to 4 XInput interfaces (controllers) |
| `CFG_TUH_XINPUT_EPIN_BUFSIZE` | 64 | IN endpoint buffer (bytes) |
| `CFG_TUH_XINPUT_EPOUT_BUFSIZE` | 64 | OUT endpoint buffer (bytes) |

---

## 7. USB: NRF52840 to PC (XInput)

### 7.1 Why Xbox 360 XInput

Xbox One controllers use the **GIP (Game Input Protocol)** which requires a
hardware cryptographic authentication chip (`xusb22.sys` sends cmd `0x06`).
Without this chip, Windows never fully activates the controller — it appears in
Device Manager but produces no input in games.

Xbox 360 XInput has **no authentication requirement**.  Windows binds
`xusb22.sys` via VID/PID matching and the controller works immediately in every
XInput-compatible game.  Both protocols expose identical button/stick/trigger
semantics to games.

| | Xbox One GIP | Xbox 360 XInput |
|---|---|---|
| VID/PID | 0x045E / 0x02EA | **0x045E / 0x028E** |
| Interface class | 0xFF / 0x47 / 0xD0 | **0xFF / 0x5D / 0x01** |
| Handshake | ANNOUNCE → IDENTIFY → AUTH → POWER | **None** |
| Auth chip required | **YES** | **NO** |
| Input report size | 18 bytes | **20 bytes** |
| Windows driver | xusb22.sys (GIP mode) | xusb22.sys (XInput mode) |

### 7.2 USB Descriptor Layout

The NRF52840 uses a vendor class interface (subclassing
`Adafruit_USBD_WebUSB`) with a fully custom **39-byte interface block**.  The
16-byte Vendor XInput Descriptor at offset 9 is critical — without it
`xusb22.sys` matches the VID/PID but never creates the gamepad input node.

```
Interface block (39 bytes total):
  Offset  Size  Value     Field
  ------  ----  --------  -----
  [0]     9     —         Interface Descriptor
    0     1     0x09      bLength
    1     1     0x04      bDescriptorType (Interface)
    2     1     itfnum    bInterfaceNumber
    3     1     0x00      bAlternateSetting
    4     1     0x02      bNumEndpoints
    5     1     0xFF      bInterfaceClass    (Vendor)
    6     1     0x5D      bInterfaceSubClass (XInput)
    7     1     0x01      bInterfaceProtocol (Gamepad)
    8     1     0x00      iInterface

  [9]     16    —         Vendor XInput Descriptor  <-- CRITICAL
    9     1     0x10      bLength = 16
   10     1     0x21      bDescriptorType (vendor/HID-like)
   11     2     0x10,0x01 bcdXInput (version 1.10)
   13     1     0x01      bSubType (gamepad)
   14     1     0x25      bFlags (IN endpoint caps)
   15     1     ep_in     bEndpointAddressIN
   16     1     0x14      bMaxInputReportSize (20 bytes)
   17     4     0x00...   reserved
   21     1     0x13      bFlags (OUT endpoint caps)
   22     1     ep_out    bEndpointAddressOUT
   23     1     0x08      bMaxOutputReportSize (8 bytes)
   24     1     0x00      reserved

  [25]    7     —         Endpoint IN Descriptor
   25     1     0x07      bLength
   26     1     0x05      bDescriptorType (Endpoint)
   27     1     ep_in     bEndpointAddress (IN, e.g. 0x81)
   28     1     0x03      bmAttributes (Interrupt)
   29     2     0x20,0x00 wMaxPacketSize = 32
   31     1     0x04      bInterval = 4 ms

  [32]    7     —         Endpoint OUT Descriptor
   32     1     0x07      bLength
   33     1     0x05      bDescriptorType (Endpoint)
   34     1     ep_out    bEndpointAddress (OUT, e.g. 0x01)
   35     1     0x03      bmAttributes (Interrupt)
   36     2     0x20,0x00 wMaxPacketSize = 32
   38     1     0x08      bInterval = 8 ms
```

### 7.3 Input Report Format (20 bytes)

Sent to the PC at every radio packet reception and on a heartbeat when idle
(interval `XINPUT_HEARTBEAT_MS = 8 ms`).

| Byte(s) | Value | Field |
|---|---|---|
| 0 | 0x00 | Report type (input) |
| 1 | 0x14 | Report size (20) |
| 2 | buttons low | DPAD (bits 0–3), Start (4), Back (5), L3 (6), R3 (7) |
| 3 | buttons high | LB (0), RB (1), Guide (2), — (3), A (4), B (5), X (6), Y (7) |
| 4 | left trigger | 0–255 |
| 5 | right trigger | 0–255 |
| 6–7 | left stick X | int16 LE |
| 8–9 | left stick Y | int16 LE |
| 10–11 | right stick X | int16 LE |
| 12–13 | right stick Y | int16 LE |
| 14–19 | 0x00 | reserved |

Bytes 2–3 are a direct split of the radio payload's `wButtons` field — no
remapping needed because the Pico transmits in XInput layout.

### 7.4 Rumble Output Report (8 bytes)

Windows sends this to the OUT endpoint when a game calls `XInputSetState`:

| Byte | Value | Field |
|---|---|---|
| 0 | 0x00 | Report type |
| 1 | 0x08 | Report size (8) |
| 2 | 0x00 | Reserved |
| 3 | 0–255 | Left motor (big / low frequency) intensity |
| 4 | 0–255 | Right motor (small / high frequency) intensity |
| 5–7 | 0x00 | Reserved |

The NRF52840 reads bytes 3 and 4, stores them in `rumble_payload[0/1]`, sets
`rumble_active = true`, and begins broadcasting them back to the Pico at 20 Hz.

---

## 8. Vibration / Rumble System

Complete data flow from game to physical motor:

```
  Game (e.g. Forza)
    XInputSetState(left=200, right=100)
         |
         v (via xusb22.sys)
  USB OUT endpoint (8-byte report: 00 08 00 C8 64 00 00 00)
         |
         v
  NRF52840 loop() — tud_vendor_read()
    rumble_big_val   = 200   (buf[3])
    rumble_small_val = 100   (buf[4])
    rumble_payload[0..1] = {200, 100}
    rumble_active = true
         |
         v (next valid radio packet received, every ~50 ms)
  NRF52840 RADIO TX (3 redundant packets, ~400 µs total)
    Address: D2:D2:D2:D2:D2
    Payload: {200, 100, 0, 0, ...}
         |
         | 2.4 GHz  ~400 µs window
         v
  Pico NRF24L01+ RX (pipe 1, 500 µs listening window)
    reverse_bits(200) = big_motor
    reverse_bits(100) = small_motor
    rumble_big = big_motor
    rumble_small = small_motor
    tuh_xinput_set_rumble(addr, inst, big_motor, small_motor, false)
    last_rumble_rx_time = now
         |
         v
  tusb_xinput → GIP CMD 0x09 (Xbox One) or OUT report (Xbox 360)
         |
         v
  Controller left motor vibrates at intensity 200/255
  Controller right motor vibrates at intensity 100/255

  Game stops: XInputSetState(left=0, right=0)
    NRF52840 sets {0, 0} → broadcasts 0,0 for 300 ms → rumble_active = false
    Pico receives 0,0 → set_rumble(0, 0) → motors stop

  Fallback (radio packet lost):
    Pico safety timeout: 1500 ms without receiving any rumble packet
    → force set_rumble(0, 0) + clear rumble_big/small
```

**Key timing constants:**

| Constant | Value | File | Purpose |
|---|---|---|---|
| `RUMBLE_TX_INTERVAL_MS` | 50 ms | `.ino` | NRF52840 broadcast rate (20 Hz) |
| `RUMBLE_STOP_DRAIN_MS` | 300 ms | `.ino` | How long to keep sending 0,0 after stop |
| Safety timeout | 1500 ms | `main.c` | Pico forces stop if no packet received |

---

## 9. Build System

### Prerequisites

| Tool | Version | Source |
|---|---|---|
| ARM GNU Toolchain | 14.2 rel1 | `arm-none-eabi-g++`, `arm-none-eabi-objcopy` |
| CMake | 3.13+ | cmake.org |
| Ninja | any | ninja-build.org |
| Python | 3.8+ | python.org |
| Pico SDK | 2.x | github.com/raspberrypi/pico-sdk |

### Configuration (top of `build.py`)

```python
PICO_SDK_PATH = Path("G:/PowerA Controller Project/pico-sdk")
ARM_BIN       = Path("C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin")
TARGET        = "pico_gamepad_host"
FLASH_BASE    = 0x10000000   # RP2040 flash start
```

Edit `PICO_SDK_PATH` and `ARM_BIN` to match your installation.

### What `build.py` Does

| Step | Action |
|---|---|
| 1 | CMake configure with Ninja generator — skipped if `CMakeLists.txt` has not changed |
| 2 | `cmake --build` — compiles all source files; the linker step is expected to crash on Windows (Ninja wraps the linker in `cmd.exe` which triggers an access violation) |
| 3 | Manual ELF link — `arm-none-eabi-g++` invoked directly from Python, bypassing `cmd.exe`; flags parsed from `build.ninja` |
| 4 | `arm-none-eabi-objcopy -Obinary` → `.bin` flat binary |
| 5 | Python UF2 converter (inline) → `build/pico_gamepad_host.uf2`; copied to `releases/` |

### Usage

```bash
# Incremental build (recompiles only changed files, ~30 s)
python build.py

# Full clean build (deletes build/ and reconfigures, ~3 min)
python build.py --clean
```

Output: `releases/pico_gamepad_host.uf2`

---

## 10. Flashing

### Pico (Transmitter)

1. Hold the **BOOTSEL** button on the Pico
2. Plug the Pico into your PC via USB-C
3. Release BOOTSEL — a drive named **RPI-RP2** appears
4. Copy `releases/pico_gamepad_host.uf2` onto the drive
5. The drive disappears and the Pico reboots automatically

No tools required — the RP2040 bootrom handles UF2 flashing natively.

### Nice!Nano (Receiver)

The receiver firmware is an Arduino sketch.  Use the Arduino IDE (1.8 or 2.x):

1. Install board support: **Adafruit nRF52** via Board Manager
   - URL: `https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`
2. Install library: **Adafruit TinyUSB Library** via Library Manager
3. Select board: **Adafruit nRF52 → Adafruit Feather nRF52840 Express**
   (or the closest variant matching your board)
4. Open `ESB_Receiver/ESB_Receiver_XInput_Xbox/ESB_Receiver_XInput_Xbox.ino`
5. **Double-tap the reset button** on the Nice!Nano to enter bootloader
   (LED should pulse slowly)
6. Select the correct COM port and click **Upload**

> **Note:** The sketch calls `sd_softdevice_disable()` at startup to free the
> radio peripheral from the BLE SoftDevice.  If you see `sd_softdevice_disable()
> = 0` in the serial log the SoftDevice was successfully stopped.

---

## 11. Debug Serial Output

### Pico — UART0 (GP0 TX / GP1 RX, 115200 baud)

Connect a USB-to-TTL adapter: adapter RX → GP0, adapter GND → GND.

Expected output on controller connect:

```
RP2040 Xbox Wireless Transmitter starting...
Waiting for Xbox controller on USB host port...
TinyUSB Host initialized. Plug in controller.
Radio: NRF24L01+ ready. TX=E7, RX(rumble)=D2

========================================
  XINPUT CONTROLLER CONNECTED!
  dev_addr=1, instance=0
  Type: Xbox One
========================================

BTN:0000 LT:  0 RT:  0 LX:     0 LY:     0 RX:     0 RY:     0
[RUMBLE RX] big=200 small=100
```

The `[RUMBLE RX]` line is rate-limited to once per 500 ms to avoid flooding.

### NRF52840 — UART1 (D11 TX / D30 RX, 115200 baud)

```
============================================
  ESB_Receiver_XInput — NRF52840
  USB: Xbox 360 Controller  VID 0x045E  PID 0x028E
============================================
  VID=0x045E PID=0x028E  DevClass=0x00 SubClass=0x00 Proto=0x00
  XInput patch: OK (SubClass=5D, VendorDesc present, Interrupt EPs)
Radio: 2Mbps Ch2, STATE=2
USB mounted — XInput active, sending reports.

[USB] Rumble: big=200 small=100
BTN:1000 LX:     0 LY:     0 RX:     0 RY:     0 LT:  0 RT:  0
[STATUS] up=15s  pkt=938  crc_e=2  radio=2  rumble_tx=12
  mounted=1  fifo_free=128
```

`[STATUS]` prints every 5 s.  `pkt` = total valid packets received.
`crc_e` = CRC errors (a few are normal; hundreds indicate interference or
wrong channel/address).

---

## 12. LED Behaviour

### Pico (onboard LED, GP25)

| Pattern | Meaning |
|---|---|
| 3 quick blinks on boot | Firmware started |
| Slow blink (500 ms on/off) | Waiting for controller to be plugged in |
| Solid ON | Controller connected and sending data |

### Nice!Nano (LED_BUILTIN)

| Pattern | Meaning |
|---|---|
| ON at boot, OFF after USB mount | USB enumeration complete |
| Toggles on each valid radio packet | Radio link active |

---

## 13. File Structure

```
PowerA_Xbox_One_Mod/
|
+-- main.c                           Pico firmware
|     USB host + NRF24L01+ transmitter
|     Reads Xbox controller via tusb_xinput
|     Transmits 14-byte gamepad payloads over ESB
|     Receives 2-byte rumble return from NRF52840
|     Forwards vibration to controller via tuh_xinput_set_rumble
|
+-- tusb_config.h                    TinyUSB host configuration
|     Host-only, XInput class driver, 64-byte EP buffers
|
+-- CMakeLists.txt                   CMake build definition
|     Links pico_stdlib, hardware_spi, tinyusb_host, xinput_host
|     UART stdio on GP0/GP1 at 115200; USB stdio disabled
|
+-- pico_sdk_import.cmake            Pico SDK locator (from SDK)
|
+-- build.py                         Automated build pipeline
|     Runs cmake + manual link + objcopy + UF2 conversion
|     Outputs releases/pico_gamepad_host.uf2
|
+-- releases/
|   +-- pico_gamepad_host.uf2        Latest Pico firmware (drag onto RPI-RP2)
|
+-- ESB_Receiver/
|   +-- ESB_Receiver_XInput_Xbox/
|       +-- ESB_Receiver_XInput_Xbox.ino   NRF52840 firmware
|             ESB radio receiver (NRF52840 built-in RADIO)
|             USB XInput Xbox 360 emulation (Adafruit TinyUSB)
|             Rumble relay: USB → radio → Pico
|
+-- lib/
|   +-- tusb_xinput/                 XInput USB host class driver
|       +-- xinput_host.h            Public API (callbacks + utility functions)
|       +-- xinput_host.c            Driver implementation
|       +-- CMakeLists.txt           CMake interface library
|       +-- LICENSE                  MIT
|       +-- README.md                Upstream documentation
|
+-- docs/
|   +-- nrf24l01_pinout.png          NRF24L01+ module physical pin layout
|   +-- pico_pinout.png              Raspberry Pi Pico full pinout reference
|
+-- LICENSE
+-- README.md                        This file
```

---

## 14. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Controller LED stays off | No VBUS (5 V) on Pico USB-C | Power Pico via VSYS so it can supply 5 V on VBUS |
| Pico reboots when controller plugged in | VSYS voltage too low | Ensure VSYS is 5 V; check cable / supply current ≥ 500 mA |
| Serial shows no `CONTROLLER CONNECTED` | Driver not matched | Controller must be Xbox One, 360 wired/wireless, or OG Xbox |
| No radio link (`crc_e` only in NRF52840 log) | Channel or address mismatch | Both sides must use channel 2 and E7×5 address |
| NRF52840 `crc_e` count climbs fast | RF interference | Move away from 2.4 GHz Wi-Fi on ch 1/6/11 or switch channel |
| NRF24L01+ resets / erratic | Power supply noise | Add 100 µF capacitor across module VCC/GND |
| NRF24L01+ not responding to SPI | Wrong 3.3 V / 5 V | Module VCC **must** be 3.3 V; use Pico 3V3 OUT pin |
| Game controller not in `joy.cpl` | Wrong USB descriptor | Verify serial log shows `XInput patch: OK` |
| Device Manager shows "Xbox Peripherals" not "Xbox 360 Peripherals" | VID/PID mismatch | Ensure sketch sets VID `0x045E` PID `0x028E` |
| Motors never stop vibrating | Old Pico firmware (pre-fix) | Flash latest `releases/pico_gamepad_host.uf2` |
| Motors vibrate at wrong intensity | Missed stop command | Fixed in current code: 20 Hz broadcast + 1500 ms safety timeout |
| `sd_softdevice_disable() = 3` | SoftDevice not running | Normal if no BLE stack was started; radio will still work |
| Arduino upload fails | Board not in bootloader | Double-tap reset button; LED should pulse slowly |

---

## 15. References

| Resource | URL |
|---|---|
| tusb_xinput driver (Ryzee119) | https://github.com/Ryzee119/tusb_xinput |
| TinyUSB | https://github.com/hathach/tinyusb |
| Raspberry Pi Pico SDK | https://github.com/raspberrypi/pico-sdk |
| Adafruit nRF52 Arduino BSP | https://github.com/adafruit/Adafruit_nRF52_Arduino |
| Adafruit TinyUSB Library | https://github.com/adafruit/Adafruit_TinyUSB_Arduino |
| NRF24L01+ Datasheet | https://www.sparkfun.com/datasheets/Components/SMD/nRF24L01Pluss_Preliminary_Product_Specification_v1_0.pdf |
| NRF52840 Product Specification | https://infocenter.nordicsemi.com/pdf/nRF52840_PS_v1.7.pdf |
| XInput on MSDN | https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad |
| Gamepad tester | https://gamepad-tester.com/ |
