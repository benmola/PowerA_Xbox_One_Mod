# Walkthrough: Xbox 360 XInput Wireless Receiver

## Problem Solved

The original GIP (Xbox One) approach required a **hardware cryptographic authentication chip** that only exists in real licensed controllers. Without it, Windows' `xusb22.sys` driver:
- Retried the IDENTIFY handshake 4 times
- Sent POWER SLEEP (0x01) instead of POWER ON (0x00)
- **Ignored all input reports** — the controller appeared in Device Manager but didn't work in games

## Solution: Xbox 360 XInput Protocol

Switched from Xbox One GIP to **Xbox 360 XInput**, which requires **zero authentication** on Windows PC.

### New Sketch Created

**[ESB_Receiver_XInput_Xbox.ino](file:///g:/PowerA%20Controller%20Project/PowerA_Xbox_One_Mod/ESB_Receiver/ESB_Receiver_XInput_Xbox/ESB_Receiver_XInput_Xbox.ino)**

### Key Design Decisions

| Aspect | GIP (old, broken) | XInput (new) |
|--------|-------------------|--------------|
| VID/PID | 0x045E/0x02EA (Xbox One S) | 0x045E/0x028E (Xbox 360) |
| Protocol | GIP handshake (ANNOUNCE→IDENTIFY→AUTH→POWER) | None — just send reports |
| Auth required | YES (crypto chip) | NO |
| Interface class | 0xFF/0x47/0xD0 | 0xFF/0x5D/0x01 |
| Report size | 18 bytes | 20 bytes |
| Code complexity | ~1100 lines | ~340 lines |
| Button remapping | XInput → GIP bitmasks (complex) | **Direct passthrough** (1:1 match!) |

### Button Mapping — Zero Conversion Needed

The transmitter's 16-bit button field is already in XInput byte layout:

```
Low byte  (bits 0-7):  DPAD_UP|DOWN|LEFT|RIGHT|START|BACK|L3|R3
High byte (bits 8-15): LB|RB|GUIDE|—|A|B|X|Y
```

This maps **directly** to Xbox 360 report bytes 2-3. The code simply splits into two bytes:
```cpp
report.buttons_lo = c_buttons & 0xFF;
report.buttons_hi = (c_buttons >> 8) & 0xFF;
```

### USB Descriptor Patching

Same technique as GIP version — subclass `Adafruit_USBD_WebUSB` and override `getInterfaceDescriptor()`:
- SubClass: `0x5D` (XInput)
- Protocol: `0x01` (Gamepad)  
- Endpoints: Interrupt (not Bulk), 32-byte max, 4ms/8ms polling

## What to Test

1. **Compile & flash** the new sketch to the NRF52840 receiver
2. **Check serial output** — verify `XInput patch: OK` and `[XInput] First report sent`
3. **Check Device Manager** — should show under `Xbox 360 Peripherals` (not just `Xbox Peripherals`)
4. **Open Game Controllers** (`Win+R → joy.cpl`) — should show `Controller (Xbox 360 For Windows)`
5. **Click Properties** — verify buttons and sticks respond to wireless controller input
6. **Test in a game**

## Files Changed

| File | Change |
|------|--------|
| [ESB_Receiver_XInput_Xbox.ino](file:///g:/PowerA%20Controller%20Project/PowerA_Xbox_One_Mod/ESB_Receiver/ESB_Receiver_XInput_Xbox/ESB_Receiver_XInput_Xbox.ino) | **NEW** — Complete XInput receiver sketch |
| ESB_Receiver_GIP_Xbox.ino | Unchanged (kept for reference) |
