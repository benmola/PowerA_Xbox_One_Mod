# Wireless Xbox Controller Conversion — Complete Technical Guide (v2)

## 1. Project Overview

**Goal:** Convert a PowerA wired Xbox One controller (VID: 0x24C6, PID: 0x581A, GIP protocol) into a dual-mode (wired + wireless) gamepad with zero hard mods to the controller PCB. Wireless mode uses proprietary 2.4GHz ESB radio for low latency. Wired mode preserves original plug-and-play behavior. Battery charges whenever a cable is connected.

**Controller Protocol:** Xbox One Game Input Protocol (GIP) — NOT standard HID, NOT classic XInput. Uses vendor-specific USB class (0xFF, SubClass 0x47, Protocol 0xD0). Requires a proprietary handshake to wake up. Handled automatically by Ryzee119's `tusb_xinput` TinyUSB driver.

**Architecture:**
```
WIRED MODE (DPDT switch position 1):
  Controller USB-A ──► External USB cable ──► PC (original behavior)
                                    ↓
                            NRF52840 RAW pin (charges battery)

WIRELESS MODE (DPDT switch position 2):
  Controller USB-A ──► OTG adapter ──► Pico USB-C (Native USB Host)
                                              │
                                         tusb_xinput
                                         parses GIP
                                              │
                                         UART (GP8/GP9)
                                              │
                                       NRF52840 #1 (ESB TX)
                                              │ 2.4GHz radio
                                              ▼
  PC ◄── USB HID gamepad ◄── NRF52840 #2 (ESB RX dongle)
```

**Hardware (what you have):**
| Component | Role |
|---|---|
| Raspberry Pi Pico (green, USB-C) | USB Host — reads controller via native USB + tusb_xinput |
| NRF52840 dev board #1 (Nice!Nano V2.0 compatible) | ESB transmitter + battery charging |
| NRF52840 dev board #2 (Nice!Nano V2.0 compatible) | ESB receiver + USB HID gamepad dongle |
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
```
USB-C port         → Controller (via OTG adapter) — USB Host
GP4 (pin 6)        → Debug UART0 TX (to USB-TTL adapter during prototyping)
GP5 (pin 7)        → Debug UART0 RX (from USB-TTL adapter)
GP8 (pin 11)       → NRF52840 UART1 TX (gamepad data to transmitter)
GP9 (pin 12)       → NRF52840 UART1 RX (from transmitter, future use)
VSYS (pin 39)      → External power input (3.3V–5V)
GND (pin 38 or 3)  → Common ground
LED (GP25)         → Status indicator (built-in)
```

### 2.2 NRF52840 Dev Board (Nice!Nano V2.0 Compatible) × 2

**Key specs:**
- Nordic nRF52840 SoC: ARM Cortex-M4F @ 64MHz
- 256KB SRAM, 1MB Flash
- 2.4GHz radio: Bluetooth 5.0 (BLE) + Enhanced ShockBurst (ESB)
- Native USB 2.0 Full-Speed (device only)
- Built-in LiPo charger via B+/B- pads
- RAW pin: unregulated voltage input — also triggers battery charging
- VCC pin: regulated 3.3V output

**Pin mapping (Nice!Nano V2.0 compatible):**
```
Left side (top to bottom):    Right side (top to bottom):
  B+  (battery positive)        B-  (battery negative)
  GND                           GND
  GND                           D3  → P0.06
  D2  → P0.17                   D4  → P0.08 (UART RX from Pico GP8)
  D3  → P0.20                   D5  → P0.09
  D4  → P0.22                   D6  → P0.10
  D5  → P0.24                   D7  → P1.11
  D6  → P1.00                   D8  → P1.13
  D7  → P0.11                   D9  → P1.15
  D8  → P1.04                   RAW (battery/USB voltage input)
  D9  → P1.06                   GND
  D10 → P0.09                   RST
  VCC (3.3V out)                VCC (3.3V out)
```

**Board #1 (Transmitter):** Inside controller. Receives gamepad data from Pico over UART, transmits via ESB. Manages LiPo charging.

**Board #2 (Receiver):** USB dongle plugged into PC. Receives ESB data, presents as USB HID gamepad.

### 2.3 PowerA Xbox One Controller (LBX-902-A-V1.2)

**USB Identity:**
- VID: 0x24C6 (Xbox 3rd Party Partners)
- PID: 0x581A
- Product: "XB1 Classic Controller"
- Manufacturer: "BDA"

**USB Descriptor Summary:**
- Device Class: 0xFF (Vendor Specific)
- Interface 0: Class 0xFF, SubClass 0x47, Protocol 0xD0 = GIP
- IN endpoint: 0x81 (Interrupt, 64 bytes, 4ms interval)
- OUT endpoint: 0x02 (Interrupt, 64 bytes, 4ms interval)
- Interface 1: Audio (isochronous, not used)
- Interface 2: Bulk (not used)
- Full-Speed USB only (12 Mbps)
- Demands 500mA

**GIP Protocol:** The controller will NOT send any data until the host sends a specific initialization sequence. The `tusb_xinput` library handles this automatically. Without it, the controller LED stays off and no reports are sent.

### 2.4 Software Stack

```
┌─────────────────────────────────┐
│         main.c (your code)      │
│  - Mount/unmount callbacks      │
│  - Report parsing               │
│  - UART output to NRF52840     │
├─────────────────────────────────┤
│    tusb_xinput (Ryzee119)       │
│  - Xbox One GIP init handshake  │
│  - Xbox 360 wired/wireless      │
│  - Xbox OG support              │
│  - Automatic type detection     │
├─────────────────────────────────┤
│      TinyUSB (Host mode)        │
│  - USB enumeration              │
│  - Endpoint management          │
│  - Transfer scheduling          │
├─────────────────────────────────┤
│    RP2040 Native USB Hardware   │
│  - Full-Speed USB 1.1 Host      │
│  - DMA transfers                │
└─────────────────────────────────┘
```

### 2.5 Enhanced ShockBurst (ESB) — Why Not BLE

| Feature | BLE HID | ESB (Proprietary) |
|---|---|---|
| Latency per hop | 7.5ms minimum | ~0.5ms |
| Pairing | OS-level Bluetooth | Hardcoded in firmware |
| Compatibility | Any BLE device | Custom receiver dongle |
| Encryption | AES-CCM built-in | Optional AES-128 |
| Power | Higher (BLE stack) | Lower (minimal overhead) |

### 2.6 DPDT Switch — Dual Mode

```
Position 1 (Wired):    Controller USB → External cable → PC
Position 2 (Wireless): Controller USB → OTG adapter → Pico USB-C Host
```

5V and GND are always connected to both paths for battery charging.

---

## 3. Communication Protocols

### 3.1 USB: Controller → Pico (handled by tusb_xinput)

The `tusb_xinput` driver:
1. Detects the controller during USB enumeration
2. Sends GIP init packets to wake it up
3. Parses raw GIP reports into a clean `xinput_gamepad_t` struct:

```c
typedef struct xinput_gamepad {
    uint16_t wButtons;      // Button bitfield (see defines below)
    uint8_t  bLeftTrigger;  // 0-255
    uint8_t  bRightTrigger; // 0-255
    int16_t  sThumbLX;      // -32768 to 32767
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
} xinput_gamepad_t;

// Button defines:
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
```

### 3.2 UART: Pico → NRF52840 Transmitter

**Settings:**
- UART1 on GP8 (TX) / GP9 (RX)
- Baud: 921600
- Format: 8N1, no flow control

**Packet format (15 bytes):**
```
Byte 0:    0xAA (sync header)
Byte 1:    Buttons low byte
Byte 2:    Buttons high byte
Byte 3-4:  Left stick X (16-bit LE)
Byte 5-6:  Left stick Y (16-bit LE)
Byte 7-8:  Right stick X (16-bit LE)
Byte 9-10: Right stick Y (16-bit LE)
Byte 11:   Left trigger (0-255)
Byte 12:   Right trigger (0-255)
Byte 13:   Reserved (0x00)
Byte 14:   Checksum (XOR of bytes 1-13)
```

At 921600 baud: 15 bytes = ~0.16ms transfer time.

### 3.3 ESB Radio: NRF52840 TX → NRF52840 RX

```
Frequency:   2402 MHz (channel 2)
Data rate:   2 Mbps
TX power:    0 dBm
Address:     0xE7E7E7E7E7 (5-byte, hardcoded)
Payload:     14 bytes (UART bytes 1-14, no header)
CRC:         16-bit (hardware)
Auto-ACK:    Enabled (5 retries, 500µs delay)
```

### 3.4 USB HID: NRF52840 RX → PC

The receiver presents as a standard USB HID gamepad with:
- 16 buttons, 4 axes (16-bit), 2 triggers (8-bit), 1 hat switch
- 1ms USB poll rate (1000Hz)
- No drivers needed on Windows/Linux/macOS

---

## 4. Dual Mode — Wired/Wireless Switching

### 4.1 DPDT Switch Wiring

The controller's USB cable has 4 wires. The DPDT switch routes the data lines (D+/D-) between two destinations. Power lines (5V/GND) are NOT switched.

```
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

*Note: In production, the DPDT switch sits between the controller PCB's USB pads and the rest of the circuit. During prototyping, just plug/unplug cables manually.*

### 4.2 Power Wiring (Always Connected)

```
Controller USB 5V (red)  ──┬── External USB cable 5V
                           ├── Pico VBUS (via OTG adapter, powers controller)
                           └── NRF52840 #1 RAW pin (charges battery)

Controller USB GND (black) ──┬── External USB cable GND
                             ├── Pico GND
                             └── NRF52840 #1 GND
```

### 4.3 Battery Charging

The NRF52840 #1's RAW pin is connected to the external USB cable's 5V line. Whenever ANY USB cable is plugged in (wired or wireless mode), the battery charges automatically through the NRF52840's built-in LiPo charger.

### 4.4 Battery Power (Wireless, No Cable)

```
LiPo (3.7V) → NRF52840 #1 B+/B- → NRF52840 #1 VCC (3.3V out)
                                          ↓
                                    Pico VSYS pin (powers Pico)
```

Note: The controller needs 5V but may work at 3.3V. Test first. If not, add a tiny MT3608 boost converter (~£0.50).

### 4.5 Mode Summary

| Condition | Data Path | Battery | Controller Power |
|---|---|---|---|
| Wired + cable | Controller → USB → PC | Charging via RAW | 5V from cable |
| Wireless + cable | Controller → Pico → NRF → ESB → PC | Charging via RAW | 5V from Pico VBUS |
| Wireless + no cable | Controller → Pico → NRF → ESB → PC | Discharging | 3.3V (test) or 5V (boost) |

---

## 5. Prototyping Phase (Breadboard)

### 5.1 Breadboard Wiring

```
    USB-TTL Adapter (debug)          Raspberry Pi Pico
    ┌──────────────┐                 ┌─────────────────────┐
    │ RX ──────────│─────────────────│ GP4 (pin 6) TX      │
    │ GND ─────────│──────┬──────────│ GND (pin 8)         │
    └──────────────┘      │          │                     │
                          │          │ USB-C port ─────────│──► Controller
    5V Power Source       │          │  (via OTG adapter)  │     (USB-A)
    ┌──────────────┐      │          │                     │
    │ 5V ──────────│──────│──────────│ VSYS (pin 39)       │
    │ GND ─────────│──────┘          │                     │
    └──────────────┘                 │ GP8 (pin 11) TX ────│──┐
                                     │ GP9 (pin 12) RX ────│──│──┐
                                     │ GND ────────────────│──│──│──┐
                                     └─────────────────────┘  │  │  │
                                                               │  │  │
                              NRF52840 #1 (Transmitter)        │  │  │
                              ┌─────────────────────┐         │  │  │
                   PC USB ───►│ USB-C (power+debug) │         │  │  │
                              │                     │         │  │  │
                              │ P0.08 (RX) ─────────│─────────┘  │  │
                              │ P0.06 (TX) ─────────│────────────┘  │
                              │ GND ────────────────│───────────────┘
                              │                     │
                              │ [2.4GHz radio] ─────│─ ─ ─ ─ ┐
                              └─────────────────────┘        │
                                                              │ ESB
                              NRF52840 #2 (Receiver)          │
                              ┌─────────────────────┐        │
                   PC USB ───►│ USB-C (USB HID)     │        │
                              │ [2.4GHz radio] ─────│─ ─ ─ ─ ┘
                              │ [Gamepad to PC] ────│──► PC sees controller
                              └─────────────────────┘
```

**Power during prototyping:** The Pico's USB-C port hosts the controller, so power the Pico via VSYS. Easiest method: cut a spare USB cable, connect its 5V wire to VSYS (pin 39) and GND wire to Pico GND. Plug that cable into your PC or a USB charger.

### 5.2 Testing Steps

**Step 1: Pico reads controller**
- Build and flash `pico_gamepad_host.uf2` (see Section 6)
- Connect USB-TTL adapter to GP4 (TX) and GND
- Power Pico via VSYS
- Plug controller into Pico's USB-C via OTG adapter
- Open PuTTY on the USB-TTL adapter's COM port at 115200
- Expected: controller LED lights up, serial shows button/stick data
- Press each button and verify correct output

**Step 2: UART to NRF52840**
- Flash NRF52840 #1 with UART bridge sketch (reads UART, prints to USB serial)
- Connect Pico GP8 → NRF52840 P0.08, GND → GND
- Open NRF52840's COM port — verify gamepad packets arrive

**Step 3: ESB radio link**
- Flash NRF52840 #1 with ESB transmitter firmware
- Flash NRF52840 #2 with ESB receiver firmware
- Verify data arrives on receiver's serial output

**Step 4: USB HID gamepad**
- Flash NRF52840 #2 with USB HID gamepad + ESB receiver firmware
- Open https://gamepad-tester.com/ — verify inputs appear
- Test all buttons, sticks, triggers

**Step 5: End-to-end**
- Full chain: Controller → Pico → UART → NRF52840 #1 → ESB → NRF52840 #2 → USB → PC
- Play a game to test latency and reliability

---

## 6. Building the Pico Firmware

### 6.1 Prerequisites

```bash
# Linux:
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git

# Windows:
# Install ARM GNU Toolchain from: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
# Install CMake from: https://cmake.org/download/
# Install Ninja from: https://ninja-build.org/ (recommended over MinGW Make)
# Install Git from: https://git-scm.com/
```

### 6.2 Project Setup

```bash
mkdir pico-workspace && cd pico-workspace

# Clone Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..
export PICO_SDK_PATH=$(pwd)/pico-sdk  # Or set in Windows env vars

# Create project
mkdir pico_gamepad_host && cd pico_gamepad_host

# Copy SDK import helper
cp ../pico-sdk/external/pico_sdk_import.cmake .

# Clone tusb_xinput driver
mkdir -p lib
git clone https://github.com/Ryzee119/tusb_xinput.git lib/tusb_xinput

# Place main.c, tusb_config.h, CMakeLists.txt in this folder
```

### 6.3 File Structure

```
pico_gamepad_host/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── tusb_config.h
├── main.c
└── lib/
    └── tusb_xinput/
        ├── xinput_host.c
        ├── xinput_host.h
        └── CMakeLists.txt
```

### 6.4 Build

```bash
mkdir build && cd build

# Linux/Mac:
cmake .. -DPICO_SDK_PATH=../../pico-sdk
make -j4

# Windows (Ninja — recommended):
cmake .. -G Ninja -DPICO_SDK_PATH=../../pico-sdk
ninja

# Windows (MinGW — if Ninja not available):
cmake .. -G "MinGW Makefiles" -DPICO_SDK_PATH=../../pico-sdk
mingw32-make -j1
```

### 6.5 Flash

1. Hold **BOOTSEL** on the Pico
2. Connect Pico to PC via USB-C (temporarily, just for flashing)
3. Release BOOTSEL — "RPI-RP2" drive appears
4. Drag `pico_gamepad_host.uf2` onto the drive
5. Pico reboots and runs firmware
6. Disconnect from PC, power via VSYS, plug controller into USB-C

---

## 7. Security

### 7.1 ESB Radio

No built-in encryption. For a gamepad, risk is minimal (attacker could inject button presses). Optional: add AES-128-CTR encryption with a pre-shared key and 4-byte nonce.

### 7.2 USB HID

The receiver dongle is trusted implicitly by the PC (standard for all USB HID).

### 7.3 Firmware Protection

Enable NRF52840 readback protection (APPROTECT) after final firmware is loaded. Irreversible without full chip erase.

---

## 8. Latency Analysis

### 8.1 Wireless Mode (ESB)

```
USB poll (controller → Pico):      ~4ms (bInterval=4)
tusb_xinput parsing:                ~0.01ms
UART transfer (15 bytes @ 921600):  ~0.16ms
ESB radio + ACK:                    ~0.5ms
USB HID (receiver → PC):           ~1ms (1000Hz)
OS input processing:                ~0.5ms
────────────────────────────────────────
Total estimated:                    ~6-7ms
```

### 8.2 Wired Mode

```
USB poll (controller → PC):        ~4ms (original)
OS input:                           ~0.5ms
────────────────────────────────────────
Total:                              ~4.5ms
```

### 8.3 Comparison

| Controller | Latency |
|---|---|
| This project (wired mode) | ~4.5ms |
| **This project (wireless ESB)** | **~6-7ms** |
| Xbox wireless (proprietary) | ~6-10ms |
| DualSense Bluetooth | ~15-25ms |
| Nintendo Switch Pro BLE | ~15-20ms |

---

## 9. Production (Inside Controller)

### 9.1 Components Inside Controller Shell

- Pico (51mm × 21mm) — alongside controller PCB
- NRF52840 #1 (34mm × 18mm) — behind controller PCB
- 603040 LiPo (40mm × 30mm × 6mm) — in grip area
- DPDT switch (12mm × 6mm) — accessible from outside
- OTG adapter internals — small PCB or direct wiring

### 9.2 Production Wiring

```
Controller PCB USB pads:
  D+ ──── DPDT pin 3 (common A)
  D- ──── DPDT pin 6 (common B)
  5V ──┬── DPDT wired-side → External USB port
       ├── Pico VSYS (power)
       └── NRF52840 #1 RAW (charging)
  GND ─┬── External USB port
       ├── Pico GND
       └── NRF52840 #1 GND

DPDT Switch:
  Pin 1 → External USB port D+
  Pin 2 → Pico USB-C D+ (internal wiring)
  Pin 3 → Controller D+ (common)
  Pin 4 → External USB port D-
  Pin 5 → Pico USB-C D- (internal wiring)
  Pin 6 → Controller D- (common)

Pico → NRF52840 #1 (UART):
  GP8 (TX) → NRF52840 P0.08 (RX)
  GP9 (RX) → NRF52840 P0.06 (TX)
  GND → GND

Battery:
  LiPo B+ → NRF52840 #1 B+
  LiPo B- → NRF52840 #1 B-

Power (wireless, no cable):
  NRF52840 #1 VCC (3.3V) → Pico VSYS
```

### 9.3 Power Budget

| Component | Current |
|---|---|
| NRF52840 #1 (ESB TX) | ~8mA |
| RP2040 (USB Host) | ~25mA |
| Controller PCB | ~15mA |
| **Total** | **~48mA** |

600mAh battery: **~12.5 hours** continuous play.

---

## 10. Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| Controller LED off | GIP init not sent | Ensure tusb_xinput is linked, check firmware |
| Controller LED off | No 5V on VBUS | Power Pico via VSYS so VBUS outputs 5V |
| No serial output | Wrong COM port | Check USB-TTL adapter COM port in Device Manager |
| No serial output | Wrong baud | Use 115200 for debug UART |
| UART garbled | Baud mismatch | Both sides must be 921600/8N1 |
| UART garbled | Missing GND | Connect GND between Pico and NRF52840 |
| ESB no data | Channel/address mismatch | Same channel + address on both boards |
| USB HID not recognized | Wrong descriptor | Verify HID report descriptor |
| Battery not charging | RAW not connected | Wire USB 5V to NRF52840 RAW pin |
| Controller won't start at 3.3V | Needs 5V | Add MT3608 boost converter |

---

## 11. Future Upgrades

1. **Rumble support:** Reverse ESB path (receiver → TX → UART → Pico → USB OUT → controller)
2. **Battery indicator:** ADC on NRF52840 RAW pin, transmitted in ESB packet
3. **Multi-controller:** ESB supports 8 pipes — one receiver, multiple controllers
4. **Custom PCB:** Single board with RP2040 + NRF52840 + charger + boost
5. **XInput emulation:** Receiver mimics Xbox 360 USB protocol for full XInput compatibility

---

## 12. References

- tusb_xinput driver: https://github.com/Ryzee119/tusb_xinput
- TinyUSB: https://github.com/hathach/tinyusb
- Pico SDK: https://github.com/raspberrypi/pico-sdk
- Adafruit nRF52 Arduino: https://github.com/adafruit/Adafruit_nRF52_Arduino
- nRF5 SDK ESB: https://infocenter.nordicsemi.com/topic/sdk_nrf5_v17.1.0/esb_user_guide.html
- USB HID Descriptors: https://eleccelerator.com/tutorial-about-usb-hid-report-descriptors/
- Gamepad Tester: https://gamepad-tester.com/
- Nordic nRF52840 Spec: https://infocenter.nordicsemi.com/pdf/nRF52840_PS_v1.7.pdf
