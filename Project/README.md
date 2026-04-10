# Pico Gamepad Host — Build Instructions

## Overview
Reads an Xbox One / 360 controller using the Pico's native USB port in Host mode.
Uses Ryzee119's `tusb_xinput` TinyUSB extension which handles all proprietary
Xbox initialization (GIP magic packets, etc.) automatically.

## Hardware Setup

### Power (CRITICAL)
The Pico's USB-C port is in HOST mode — it provides 5V to the controller.
Therefore, the Pico itself must be powered from somewhere else:

**Option A (Prototyping):** Feed 5V into the VSYS pin (pin 39) from a bench 
supply, a second USB cable's 5V, or a battery.

**Option B (Quick hack):** Cut a USB cable, connect its 5V/GND to VSYS/GND 
on the Pico. Plug that into your PC for power. Then plug the controller into 
the Pico's USB-C port.

### Wiring
```
Pico USB-C port  →  Controller (via micro-USB cable + USB-A OTG adapter)
Pico GP4 (pin 6) →  USB-TTL adapter RX (debug serial output)
Pico GND (pin 8) →  USB-TTL adapter GND
Pico VSYS (pin 39) → 5V power source
Pico GP8 (pin 11) →  NRF52840 RX (future: gamepad data)
Pico GP9 (pin 12) → NRF52840 TX (future: gamepad data)
Pico GND         →  NRF52840 GND
```

## Software Setup

### 1. Prerequisites
```bash
# Windows: Install from ARM website or use package manager
# Linux:
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git
```

### 2. Clone and Setup
```bash
mkdir pico-workspace && cd pico-workspace

# Clone Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..
export PICO_SDK_PATH=$(pwd)/pico-sdk

# Create project directory with our files
mkdir pico_gamepad_host && cd pico_gamepad_host

# Copy main.c, tusb_config.h, CMakeLists.txt into this folder
# Copy pico_sdk_import.cmake from the SDK:
cp ../pico-sdk/external/pico_sdk_import.cmake .

# Clone tusb_xinput as a submodule
mkdir -p lib
git clone https://github.com/Ryzee119/tusb_xinput.git lib/tusb_xinput
```

### 3. Build
```bash
mkdir build && cd build

# Linux/Mac:
cmake .. -DPICO_SDK_PATH=../../pico-sdk
make -j4

# Windows (MinGW):
cmake .. -G "MinGW Makefiles" -DPICO_SDK_PATH=../../pico-sdk
mingw32-make -j4

# Windows (Ninja — recommended if mingw32-make crashes):
cmake .. -G Ninja -DPICO_SDK_PATH=../../pico-sdk
ninja
```

### 4. Flash
1. Hold **BOOTSEL** on the Pico
2. Plug Pico into PC via USB-C (or power cycle if already connected)
3. Release BOOTSEL — "RPI-RP2" drive appears
4. Drag `pico_gamepad_host.uf2` onto the drive
5. Pico reboots automatically

### 5. Test
1. Power the Pico via VSYS (since USB-C is now in host mode)
2. Connect USB-TTL adapter to GP4 (TX) and GND
3. Open PuTTY/serial terminal on the adapter's COM port at 115200
4. Plug the Xbox controller into the Pico's USB-C port (via OTG adapter)
5. You should see:
   ```
   XINPUT CONTROLLER CONNECTED!
   Type: Xbox One
   BTN:0000  A:0 B:0 X:0 Y:0  LB:0 RB:0  LT:  0 RT:  0  LX:     0 LY:     0
   ```
6. Press buttons and move sticks — values update in real time

## File Structure
```
pico_gamepad_host/
├── CMakeLists.txt          ← Build configuration
├── pico_sdk_import.cmake   ← Copied from Pico SDK
├── tusb_config.h           ← TinyUSB configuration
├── main.c                  ← Application firmware
├── lib/
│   └── tusb_xinput/        ← Ryzee119's XInput driver (git clone)
│       ├── xinput_host.c
│       ├── xinput_host.h
│       └── CMakeLists.txt
└── build/                  ← Build output (created by cmake)
    └── pico_gamepad_host.uf2
```

## LED Behavior
- **5 fast blinks** → Firmware is alive
- **Slow blink (0.5s)** → Waiting for controller
- **Solid ON** → Controller connected and sending data
- **Off** → Controller disconnected

## Next Steps
Once this works, the NRF52840 firmware (ESB transmitter + USB receiver) 
is the next phase. The gamepad binary packets are already being sent on 
UART1 (GP8/GP9) at 921600 baud in the format documented in the guide.
