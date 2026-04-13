# Wireless Xbox Controller Conversion — Complete Technical Guide (v2)

## 1. Project Overview

**Goal:** Convert a PowerA wired Xbox One controller (VID: 0x24C6, PID: 0x581A, GIP protocol) into a dual-mode (wired + wireless) gamepad with zero hard mods to the controller PCB. Wireless mode uses proprietary 2.4GHz ESB radio via nRF24L01+ modules for low latency. Wired mode preserves original plug-and-play behavior. Battery charges whenever a cable is connected.

**Controller Protocol:** Xbox One Game Input Protocol (GIP) — NOT standard HID, NOT classic XInput. Uses vendor-specific USB class (0xFF, SubClass 0x47, Protocol 0xD0). Requires a proprietary handshake to wake up. Handled automatically by Ryzee119's `tusb_xinput` TinyUSB driver.

**Architecture:**

> [!NOTE]
> **Temporary Test Setup:** Currently, the battery and charger are bypassed. The setup is temporarily powered via a direct USB-A 5V source. The battery and step-up charger circuitry are kept in the design below for the complete wireless conversion.

```text
WIRED MODE (DPDT switch position 1):
  Controller USB-A ──► External USB cable ──► PC (original behavior)
                                    ↓
                            Charge Module (charges battery)

WIRELESS MODE (DPDT switch position 2):
  Controller USB-A ──► OTG adapter ──► Pico USB-C (Native USB Host)
                                              │
                                         tusb_xinput
                                         parses GIP
                                              │
                                         SPI (GP2-GP4, GP16-17)
                                              │
                                       nRF24L01+ (ESB TX)
                                              │ 2.4GHz radio
                                              ▼
  PC ◄── USB HID gamepad ◄── Receiver MCU + nRF24L01+ (ESB RX dongle)
```

**Hardware (what you have):**
| Component | Role |
|---|---|
| Raspberry Pi Pico (green, USB-C) × 2 | USB Host reads controller / acts as USB PC Dongle |
| nRF24L01+ Transceiver Module × 2 | ESB radio communication (TX inside controller, RX on dongle) |
| TP5400 / 134N3P Charge+Boost Module | Battery charging and 5V step-up (temporarily bypassed for USB 5V testing) |
| USB-to-TTL adapter | Debug serial output during prototyping |

**Hardware (to buy):**
| Component | Est. Cost |
|---|---|
| USB-C to USB-A female OTG adapter | £1 |
| 603040 LiPo battery (3.7V, 600mAh) | £1–2 |
| DPDT slide switch (2-position) | £0.50 |
| **Total** | **£2.50–3.50** |

---

## 2. Hardware Deep-Dive

### 2.1 Raspberry Pi Pico (RP2040, USB-C variant)

**Key specs:**
- Dual-core ARM Cortex-M0+ @ 133MHz
- 264KB SRAM, 2MB Flash
- Native USB 1.1 controller (Host and Device modes)
- 30 GPIO, 2× SPI, 2× I2C, 2× UART
- Operating voltage: 1.8V–5.5V (on-board 3.3V regulator)

**USB Host Mode (Native USB-C port):**
- The RP2040's built-in USB controller runs in Host mode via TinyUSB
- The controller plugs directly into the Pico's USB-C port via a USB-C to USB-A female OTG adapter
- **Critical:** When USB-C is in host mode, the Pico CANNOT be powered through that port. Power must come in via the VSYS pin (1.8V–5.5V)
- The Pico provides 5V on VBUS to power the controller
- No external resistors, no PIO, no GPIO wiring for USB — it's all handled by the native USB hardware

**Pin Assignments:**
```text
USB-C port         → Controller (via OTG adapter) — USB Host
GP0 (pin 1)        → Debug UART0 TX (to USB-TTL adapter during prototyping)
GP1 (pin 2)        → Debug UART0 RX (from USB-TTL adapter)
GP2 (pin 4)        → nRF24L01+ SPI SCK
GP3 (pin 5)        → nRF24L01+ SPI MOSI
GP4 (pin 6)        → nRF24L01+ SPI MISO
GP16 (pin 21)      → nRF24L01+ SPI CE
GP17 (pin 22)      → nRF24L01+ SPI CSN
VSYS (pin 39)      → External power input (3.3V–5V)
3V3 (pin 36)       → Power to nRF24L01+ (DO NOT use 5V for nRF!)
GND (pin 38 or 3)  → Common ground
```

### 2.2 nRF24L01+ Wireless Transceiver Module

**Key specs:**
- 2.4GHz ISM band operation
- Enhanced ShockBurst (ESB) hardware protocol
- SPI interface to microcontroller
- Requires strictly 3.3V power on VCC (Pins are usually 5V tolerant, but VCC must be 3.3V)

**Physical Pin Layout (Top View):**
```text
(Top Left)               (Top Right)
  IRQ [Not used]           MISO ← to Pico GP4
  MOSI ← from Pico GP3     SCK  ← from Pico GP2
  CSN  ← from Pico GP17    CE   ← from Pico GP16
  VCC  ← Pico 3V3(OUT)     GND  ← Common GND
(Bottom Left)            (Bottom Right)
```

### 2.3 TP5400 / 134N3P Charge+Boost Module

**Key specs:**
- Accepts LiPo battery
- Charges via USB-C or RAW input
- Steps up 3.7V to 5V output (needed to power Pico and provide VBUS for controller)

**Note:** Currently bypassed. The setup is temporarily powered via direct USB 5V power source.

### 2.4 Software Stack

```text
┌─────────────────────────────────┐
│         main.c (your code)      │
│  - Mount/unmount callbacks      │
│  - Report parsing               │
│  - SPI output to nRF24L01+      │
├─────────────────────────────────┤
│    tusb_xinput (Ryzee119)       │
│  - Xbox One GIP init handshake  │
│  - Xbox 360 wired/wireless      │
├─────────────────────────────────┤
│      TinyUSB (Host mode)        │
│  - USB enumeration & transfers  │
└─────────────────────────────────┘
```

---

## 3. Communication Protocols

### 3.1 USB: Controller → Pico (handled by tusb_xinput)

The `tusb_xinput` driver detects the controller and parses raw GIP reports into a clean `xinput_gamepad_t` struct.

### 3.2 SPI: Pico → nRF24L01+ Transmitter

**Settings:**
- SPI0
- Payload format can be raw struct bytes.

**Packet format (14 bytes):**
```text
Byte 0-1:  Buttons (16-bit LE)
Byte 2-3:  Left stick X (16-bit LE)
Byte 4-5:  Left stick Y (16-bit LE)
Byte 6-7:  Right stick X (16-bit LE)
Byte 8-9:  Right stick Y (16-bit LE)
Byte 10:   Left trigger (0-255)
Byte 11:   Right trigger (0-255)
Byte 12-13: Checksum or padding
```

### 3.3 ESB Radio: nRF24L01+ TX → nRF24L01+ RX

Frequency, data rate (up to 2Mbps), TX power, and address limits apply natively via the nRF24L01+ configuration registers. Typical setup utilizes Auto-ACK for reliable delivery.

### 3.4 USB HID: nRF24L01+ RX → PC

The receiver Pico dongle presents as a standard USB HID gamepad with 1ms USB poll rate (1000Hz). No drivers needed on Windows.

---

## 4. Dual Mode — Wired/Wireless Switching

### 4.1 DPDT Switch Wiring

The controller's USB cable has 4 wires. The DPDT switch routes the data lines (D+/D-) between two destinations. Power lines (5V/GND) are NOT switched.

```text
                          DPDT Slide Switch
                    ┌─────────────────────────┐
                    │  1 ●────────● 2         │
                    │      ● 3                │
                    │  4 ●────────● 5         │
                    │      ● 6                │
                    └─────────────────────────┘

Pin 3 (common A) ← Controller USB D+ (green wire)
Pin 6 (common B) ← Controller USB D- (white wire)

Position 1 (Wired):
  Pin 1 → External USB cable D+
  Pin 4 → External USB cable D-

Position 2 (Wireless):
  Pin 2 → Pico USB-C D+ (via OTG adapter internally)
  Pin 5 → Pico USB-C D- (via OTG adapter internally)
```

### 4.2 Power Wiring (with Battery / Step-Up Module)

```text
3.7V LiPo Battery            TP5400 / 134N3P Charge+Boost Module
    ┌──────────────┐             ┌─────────────────────────────┐
    │ RED Wire (+) │────────────►│ B+               5V OUT (+) │──┐
    │              │             │                             │  │
    │ BLK Wire (-) │────────────►│ B-               5V OUT (-) │──┼─┐
    └──────────────┘             │                             │  │ │
                                 │ USB-C IN (For Charging)     │  │ │
                                 └─────────────────────────────┘  │ │
                                                                  │ │
                                Pico USB-C RP2040 (Host)          │ │
                                 ┌─────────────────────┐          │ │
                                 │ 5V / VSYS           │◄─────────┘ │
                                 │ GND                 │◄───────────┘
                                 │                     │
      (Gets 5V via OTG) ◄────────│ USB-C Port          │
                                 │                     │
                                 │ 3V3(OUT)            │──┐ (Powers nRF24L01+)
                                 │ SPI SCK  (GP2)      │  │
                                 │ SPI MOSI (GP3)      │  │
                                 │ SPI MISO (GP4)      │  │
                                 │ SPI CSN  (GP17)     │  │
                                 │ SPI CE   (GP16)     │  │
                                 └─────────────────────┘  │
                                                          │
                                 nRF24L01+ Transceiver    │
                                 ┌─────────────────────┐  │
                                 │ VCC (3.3V ONLY!)    │◄─┘
                                 │ GND                 │◄── (From Common GND)
                                 │ SCK                 │◄── (From RP2040 GP2)
                                 │ MOSI                │◄── (From RP2040 GP3)
                                 │ MISO                │◄── (From RP2040 GP4)
                                 │ CSN                 │◄── (From RP2040 GP17)
                                 │ CE                  │◄── (From RP2040 GP16)
                                 └─────────────────────┘
```

> [!CAUTION]
> The nRF24L01+ module MUST receive 3.3V power. Connecting VCC to 5V will permanently damage it. Use the Pico's `3V3(OUT)` pin.

### 4.3 Mode Summary

| Condition | Data Path | Battery | Controller Power |
|---|---|---|---|
| Wired + cable | Controller → USB → PC | Charging via Module | 5V from cable |
| Wireless + cable | Controller → Pico → nRF24 → ESB → PC | Charging via Module | 5V from Pico VBUS |
| Wireless + no cable | Controller → Pico → nRF24 → ESB → PC | Discharging | 5V from Boost Module |

---

## 5. Prototyping Phase (Breadboard)

### 5.1 Testing Steps

**Step 1: Pico reads controller**
- Build and flash `pico_gamepad_host.uf2`
- Connect USB-TTL adapter to GP0 (TX) and GND
- Power Pico via VSYS (currently using USB 5V directly)
- Plug controller into Pico's USB-C via OTG adapter
- Expected: controller LED lights up, serial shows button/stick data

**Step 2: SPI to nRF24L01+**
- Connect Pico to nRF24L01+ as per schematic.
- Initialize SPI and configure nRF24 registers (Channel, TX Address).
- Start sending packets.

**Step 3: ESB radio link & Dongle**
- Wire up a second Pico (or other MCU) to an nRF24L01+ as the receiver dongle.
- Configure as ESB receiver.
- Flash USB HID gamepad firmware on the dongle.
- Open https://gamepad-tester.com/ — verify inputs appear.

---

## 6. Building the Pico Firmware

### 6.1 Prerequisites

```bash
# Linux:
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git

# Windows:
# Install ARM GNU Toolchain, CMake, Ninja, Git
```

### 6.2 Project Setup

```bash
mkdir pico-workspace && cd pico-workspace

# Clone Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..
export PICO_SDK_PATH=$(pwd)/pico-sdk

# Create project
mkdir pico_gamepad_host && cd pico_gamepad_host

# Copy SDK import helper
cp ../pico-sdk/external/pico_sdk_import.cmake .

# Clone tusb_xinput driver
mkdir -p lib
git clone https://github.com/Ryzee119/tusb_xinput.git lib/tusb_xinput
```

### 6.3 Build & Flash

```bash
mkdir build && cd build
cmake .. -G Ninja -DPICO_SDK_PATH=../../pico-sdk
ninja
```
1. Hold **BOOTSEL** on the Pico
2. Connect Pico to PC via USB-C (temporarily, just for flashing)
3. Release BOOTSEL — "RPI-RP2" drive appears
4. Drag `pico_gamepad_host.uf2` onto the drive

---

## 7. Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| Controller LED off | No 5V on VBUS | Power Pico via VSYS so VBUS outputs 5V |
| No serial output | Wrong baud | Use 115200 for debug UART on GP0 |
| ESB no data | Channel/address mismatch | Both nRF24 modules need exact same channel & pipe |
| nRF24 unstable | Power drop | Add small capacitor (10uF-100uF) across nRF24 VCC/GND |
| Battery not charging | Module bypassed | Remember to wire charge module back in for final build |

---

## 8. References

- tusb_xinput driver: https://github.com/Ryzee119/tusb_xinput
- TinyUSB: https://github.com/hathach/tinyusb
- Pico SDK: https://github.com/raspberrypi/pico-sdk
- nRF24L01+ Datasheet: https://www.sparkfun.com/datasheets/Components/SMD/nRF24L01Pluss_Preliminary_Product_Specification_v1_0.pdf
- Gamepad Tester: https://gamepad-tester.com/
