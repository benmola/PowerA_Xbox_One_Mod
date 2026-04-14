#!/usr/bin/env python3
"""Convert a flat .bin file to .uf2 for RP2040 (Raspberry Pi Pico)."""

import struct
import sys
import os

# UF2 format constants
UF2_MAGIC1     = 0x0A324655  # "UF2\n"
UF2_MAGIC2     = 0x9E5D5157
UF2_MAGIC_END  = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56

PAYLOAD_SIZE = 256  # bytes of firmware data per block
BLOCK_SIZE   = 512  # total bytes per UF2 block


def bin_to_uf2(bin_path: str, uf2_path: str, base_addr: int = 0x10000000) -> None:
    with open(bin_path, "rb") as f:
        data = f.read()

    # Pad to multiple of PAYLOAD_SIZE
    remainder = len(data) % PAYLOAD_SIZE
    if remainder:
        data += b"\x00" * (PAYLOAD_SIZE - remainder)

    num_blocks = len(data) // PAYLOAD_SIZE

    with open(uf2_path, "wb") as out:
        for block_no in range(num_blocks):
            addr   = base_addr + block_no * PAYLOAD_SIZE
            payload = data[block_no * PAYLOAD_SIZE : (block_no + 1) * PAYLOAD_SIZE]

            # 32-byte header
            header = struct.pack(
                "<IIIIIIII",
                UF2_MAGIC1,
                UF2_MAGIC2,
                UF2_FLAG_FAMILY_ID_PRESENT,
                addr,
                PAYLOAD_SIZE,
                block_no,
                num_blocks,
                RP2040_FAMILY_ID,
            )

            # 476-byte data region (256 payload + 220 zeros)
            data_region = payload + b"\x00" * (476 - PAYLOAD_SIZE)

            # 4-byte final magic
            footer = struct.pack("<I", UF2_MAGIC_END)

            out.write(header + data_region + footer)  # 32 + 476 + 4 = 512 bytes

    size_kb = os.path.getsize(uf2_path) / 1024
    print(f"Created {uf2_path}  ({num_blocks} blocks, {size_kb:.1f} KB)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.bin> <output.uf2> [base_addr_hex]")
        print("Default base address: 0x10000000 (RP2040 flash start)")
        sys.exit(1)

    bin_file  = sys.argv[1]
    uf2_file  = sys.argv[2]
    base      = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x10000000

    bin_to_uf2(bin_file, uf2_file, base)
