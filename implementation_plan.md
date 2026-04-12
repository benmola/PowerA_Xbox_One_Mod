# Fix Wireless Controller: Switch from GIP to XInput Protocol

## Problem Analysis

After extensive debugging and research, I've identified **the root cause** of why the wireless receiver doesn't work as a game controller on Windows:

> [!CAUTION]
> **Xbox One GIP protocol requires a hardware authentication chip.** Windows' `xusb22.sys` driver performs a cryptographic authentication handshake (cmd 0x06) that requires a dedicated security ASIC present inside genuine Microsoft/licensed controllers. Without this chip, Windows **never fully activates the controller** — explaining why:
> - The host retries IDENTIFY 4 times (waiting for auth to complete)
> - The host sends POWER mode=0x01 (SLEEP) instead of 0x00 (ON) 
> - Input reports are received by USB hardware but **ignored by the driver**
> - The controller shows in Device Manager but **doesn't work in games**

This is confirmed by GP2040-CE (the leading open-source gamepad firmware):
- **XInput mode** (Xbox 360): Works on PC **without any authentication**
- **Xbox One mode** (GIP): Requires auth passthrough hardware — a real Xbox controller plugged in to handle the crypto handshake

Your **wired** PowerA controller works because it has the auth chip built into its PCB. The **wireless receiver** (NRF52840) has no such chip.

## Proposed Solution: Xbox 360 XInput Protocol

> [!IMPORTANT]
> Switch from Xbox One S GIP (VID 0x045E / PID 0x02EA) to **Xbox 360 XInput** (VID 0x045E / PID 0x028E). The Xbox 360 protocol:
> - Requires **no authentication** on Windows PC
> - Is natively recognized by `xusb22.sys` as a gamepad / XInput device
> - Works in **all games** that support Xbox controllers
> - Shows as "Controller (Xbox 360 For Windows)" in Game Controllers
> - Has the exact same button/stick/trigger functionality

### Why Not Just Fix GIP?
There is no software-only bypass for GIP authentication on Windows. The only alternatives are:
1. **Auth passthrough** — wire a real controller's auth chip to the NRF52840 (requires hardware modification)
2. **Switch to Xbox 360 XInput** — no auth needed ← **this is the recommended approach**

## Proposed Changes

### ESB_Receiver (XInput version)

#### [NEW] [ESB_Receiver_XInput_Xbox](file:///g:/PowerA%20Controller%20Project/PowerA_Xbox_One_Mod/ESB_Receiver/ESB_Receiver_XInput_Xbox/ESB_Receiver_XInput_Xbox.ino)

New receiver sketch that presents as an Xbox 360 wired controller:

| Property | Xbox One S (current, broken) | Xbox 360 (proposed) |
|---|---|---|
| VID/PID | 0x045E / 0x02EA | 0x045E / 0x028E |
| Protocol | GIP (proprietary handshake) | XInput (simple report) |
| Auth required | **YES** (crypto chip) | **NO** |
| Interface class | 0xFF / 0x47 / 0xD0 | 0xFF / 0x5D / 0x01 |
| Endpoints | Interrupt IN+OUT | Interrupt IN+OUT |
| Report size | 18 bytes | 20 bytes |
| Handshake | ANNOUNCE→IDENTIFY→AUTH→POWER | None — just send reports |
| Windows driver | xusb22.sys (GIP mode) | xusb22.sys (XInput mode) |

**Xbox 360 XInput report format (20 bytes):**
```
Byte  0:    0x00 (report type)
Byte  1:    0x14 (report size = 20)
Byte  2:    button byte 1 (DPAD_UP|DOWN|LEFT|RIGHT|START|BACK|L3|R3)
Byte  3:    button byte 2 (LB|RB|GUIDE|A|B|X|Y)
Byte  4:    left trigger (0-255)
Byte  5:    right trigger (0-255)
Bytes 6-7:  left stick X (int16 LE)
Bytes 8-9:  left stick Y (int16 LE)
Bytes 10-11: right stick X (int16 LE)
Bytes 12-13: right stick Y (int16 LE)
Bytes 14-19: reserved (zeros)
```

**Key advantages:**
- No handshake needed — start sending input reports immediately after USB mount
- No AUTH chip required
- Identical game support to Xbox One controller on Windows
- Much simpler code (~300 lines vs ~1100 lines)

## Open Questions

> [!IMPORTANT]
> **Do you want to proceed with the Xbox 360 XInput approach?** This will:
> - Create a new sketch `ESB_Receiver_XInput_Xbox/` (your existing GIP code stays untouched)
> - Show as "Controller (Xbox 360 For Windows)" instead of "Controller (Xbox One For Windows)"
> - Work identically in all games — XInput is XInput regardless of 360 vs One branding

## Verification Plan

### Automated Tests
- Compile the new sketch successfully
- Verify USB descriptor bytes match Xbox 360 controller spec

### Manual Verification
- Flash to NRF52840 receiver
- Check Device Manager: should show under "Xbox 360 Peripherals"
- Check Game Controllers (`joy.cpl`): should show "Controller (Xbox 360 For Windows)" with status OK
- Test buttons/sticks/triggers in Game Controllers Properties dialog
- Test in a game
