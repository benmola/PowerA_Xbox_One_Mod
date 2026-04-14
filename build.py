#!/usr/bin/env python3
"""
build.py - Automated build pipeline for pico_gamepad_host

Usage:
    python build.py            # incremental build
    python build.py --clean    # delete build/ and start fresh

What it does:
    1. CMake configure  (Ninja generator, only when needed)
    2. Compile objects  (cmake --build; the final link step is expected to
                         crash via Ninja on Windows - that is handled below)
    3. Link ELF         (arm-none-eabi-g++ called directly, bypassing Ninja)
    4. Generate .bin    (arm-none-eabi-objcopy)
    5. Convert to .uf2  (Python - no external tool needed)
    6. Copy .uf2 ->     releases/pico_gamepad_host.uf2

Flash: hold BOOTSEL, plug in USB, drag the .uf2 onto the RPI-RP2 drive.
"""

import argparse
import os
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# ------------------------------------------------------------------------------
#  CONFIGURATION - edit these paths if your tools are installed elsewhere
# ------------------------------------------------------------------------------

PICO_SDK_PATH = Path("G:/PowerA Controller Project/pico-sdk")

ARM_BIN     = Path("C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin")
ARM_GXX     = ARM_BIN / "arm-none-eabi-g++.exe"
ARM_OBJCOPY = ARM_BIN / "arm-none-eabi-objcopy.exe"

TARGET     = "pico_gamepad_host"
FLASH_BASE = 0x10000000     # RP2040 flash start address

# ------------------------------------------------------------------------------
#  UF2 constants (RP2040)
# ------------------------------------------------------------------------------

UF2_MAGIC1      = 0x0A324655   # "UF2\n"
UF2_MAGIC2      = 0x9E5D5157
UF2_MAGIC_END   = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000   # familyID present
RP2040_FAMILY   = 0xE48BFF56
PAYLOAD_SIZE    = 256          # bytes of firmware data per 512-byte UF2 block

# ------------------------------------------------------------------------------

SCRIPT_DIR   = Path(__file__).parent.resolve()
BUILD_DIR    = SCRIPT_DIR / "build"
RELEASES_DIR = SCRIPT_DIR / "releases"


# -- Helpers -------------------------------------------------------------------

def die(msg: str) -> None:
    print(f"\n[ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


def banner(step: str) -> None:
    print(f"\n{'-'*60}")
    print(f"  {step}")
    print(f"{'-'*60}")


def run(cmd: list, cwd: Path = None, check: bool = True) -> subprocess.CompletedProcess:
    """Run a subprocess, printing the command first."""
    pretty = " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd)
    print(f"  $ {pretty}")
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None)
    if check and result.returncode != 0:
        die(f"Command exited with code {result.returncode}")
    return result


# -- Step 1: CMake configure ---------------------------------------------------

def cmake_configure(force: bool = False) -> None:
    banner("Step 1/5 - CMake configure")

    cmake_cache = BUILD_DIR / "CMakeCache.txt"
    ninja_file  = BUILD_DIR / "build.ninja"
    cmakelists  = SCRIPT_DIR / "CMakeLists.txt"

    if not force and cmake_cache.exists() and ninja_file.exists():
        if cmakelists.stat().st_mtime <= cmake_cache.stat().st_mtime:
            print("  CMake already configured and up to date - skipping.")
            return

    print("  Running CMake with Ninja generator ...")
    BUILD_DIR.mkdir(exist_ok=True)
    run([
        "cmake",
        "-S", SCRIPT_DIR,
        "-B", BUILD_DIR,
        "-G", "Ninja",
        f"-DPICO_SDK_PATH={PICO_SDK_PATH}",
    ])


# -- Step 2: Compile source files ----------------------------------------------

def compile_objects() -> None:
    banner("Step 2/5 - Compile source files")
    print("  (The final link step will crash via Ninja on Windows - that is expected.)\n")

    result = run(["cmake", "--build", str(BUILD_DIR)], check=False)

    rsp = BUILD_DIR / "CMakeFiles" / f"{TARGET}.rsp"

    if result.returncode == 0:
        # Ninja somehow succeeded (or nothing needed rebuilding).
        # We still run the manual link below to regenerate the ELF from
        # the latest objects, so this is fine.
        print("  cmake --build returned 0 (nothing rebuilt or link succeeded).")
    else:
        if not rsp.exists():
            die(
                "cmake --build failed and the linker response file was not created.\n"
                "This means compilation failed before reaching the link step.\n"
                "Fix the compiler errors shown above and try again."
            )
        print("\n  Link step failed as expected (Ninja/Windows crash).")
        print("  Linker response file found - proceeding with manual link.")


# -- Step 3: Link ELF ----------------------------------------------------------

def _extract_link_flags() -> list:
    """Parse LINK_FLAGS (and CPU FLAGS) out of build.ninja for our target."""
    ninja_path = BUILD_DIR / "build.ninja"
    cpu_flags = ["-mcpu=cortex-m0plus", "-mthumb", "-g", "-O3", "-DNDEBUG"]
    link_flags_str = ""

    in_block = False
    with open(ninja_path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            # Detect the start of our target's build block
            if (not line.startswith(" ")) and f"build {TARGET}.elf " in line:
                in_block = True
            elif in_block:
                if line.startswith("  LINK_FLAGS = "):
                    link_flags_str = line[len("  LINK_FLAGS = "):]
                    break
                # A non-indented, non-empty line means a new block started
                if line and not line.startswith(" "):
                    break

    if not link_flags_str:
        die("Could not find LINK_FLAGS in build.ninja - was cmake configure run?")

    # shlex.split handles quoted tokens that contain spaces (e.g. paths)
    return cpu_flags + shlex.split(link_flags_str, posix=True)


def link_elf() -> Path:
    banner("Step 3/5 - Link ELF (arm-none-eabi-g++ direct)")

    rsp_file = BUILD_DIR / "CMakeFiles" / f"{TARGET}.rsp"
    elf_file = BUILD_DIR / f"{TARGET}.elf"

    if not rsp_file.exists():
        die(
            f"Response file not found: {rsp_file}\n"
            "Run without --clean first so objects are compiled."
        )

    link_flags = _extract_link_flags()

    run(
        [ARM_GXX] + link_flags + [f"@{rsp_file}", "-o", elf_file],
        cwd=BUILD_DIR,
    )

    if not elf_file.exists():
        die("Linker did not produce an ELF file.")

    kb = elf_file.stat().st_size / 1024
    print(f"\n  ELF created: {elf_file.name}  ({kb:.1f} KB)")
    return elf_file


# -- Step 4: Binary extraction -------------------------------------------------

def generate_bin(elf_file: Path) -> Path:
    banner("Step 4/5 - Generate flat binary (.bin)")

    bin_file = elf_file.with_suffix(".bin")
    run([ARM_OBJCOPY, "-Obinary", elf_file, bin_file])

    kb = bin_file.stat().st_size / 1024
    print(f"\n  BIN created: {bin_file.name}  ({kb:.1f} KB)")
    return bin_file


# -- Step 5: BIN -> UF2 conversion + release packaging ------------------------

def bin_to_uf2(bin_file: Path, uf2_file: Path, base: int = FLASH_BASE) -> None:
    """Convert a flat binary to UF2 format (RP2040)."""
    data = bin_file.read_bytes()

    # Pad to a multiple of PAYLOAD_SIZE
    pad = (-len(data)) % PAYLOAD_SIZE
    data += b"\x00" * pad

    num_blocks = len(data) // PAYLOAD_SIZE

    with open(uf2_file, "wb") as out:
        for blk in range(num_blocks):
            addr    = base + blk * PAYLOAD_SIZE
            payload = data[blk * PAYLOAD_SIZE : (blk + 1) * PAYLOAD_SIZE]

            header = struct.pack(
                "<IIIIIIII",
                UF2_MAGIC1,
                UF2_MAGIC2,
                UF2_FLAG_FAMILY,
                addr,
                PAYLOAD_SIZE,
                blk,
                num_blocks,
                RP2040_FAMILY,
            )
            # Each block = 32 header + 476 data region + 4 footer = 512 bytes
            data_region = payload + b"\x00" * (476 - PAYLOAD_SIZE)
            footer      = struct.pack("<I", UF2_MAGIC_END)

            out.write(header + data_region + footer)


def package_release(elf_file: Path) -> Path:
    banner("Step 5/5 - Convert to UF2 and package release")

    bin_file = generate_bin(elf_file)

    # Also keep a .uf2 in build/ (mirrors what cmake would have made)
    build_uf2 = BUILD_DIR / f"{TARGET}.uf2"
    bin_to_uf2(bin_file, build_uf2)

    # Copy to releases/
    RELEASES_DIR.mkdir(exist_ok=True)
    release_uf2 = RELEASES_DIR / f"{TARGET}.uf2"
    shutil.copy2(build_uf2, release_uf2)

    blocks = build_uf2.stat().st_size // 512
    kb     = build_uf2.stat().st_size / 1024
    print(f"\n  UF2 created: {build_uf2.name}  ({blocks} blocks, {kb:.1f} KB)")
    print(f"  Copied to:   {release_uf2}")
    return release_uf2


# -- Sanity checks -------------------------------------------------------------

def check_tools() -> None:
    if not ARM_GXX.exists():
        die(
            f"ARM GCC not found:\n  {ARM_GXX}\n"
            "Install the Arm GNU Toolchain or update ARM_BIN in build.py."
        )
    if not ARM_OBJCOPY.exists():
        die(
            f"arm-none-eabi-objcopy not found:\n  {ARM_OBJCOPY}\n"
            "Check that the ARM toolchain installation is complete."
        )
    if not PICO_SDK_PATH.exists():
        die(
            f"Pico SDK not found:\n  {PICO_SDK_PATH}\n"
            "Clone the SDK or update PICO_SDK_PATH in build.py."
        )
    if not shutil.which("cmake"):
        die("cmake not found on PATH. Install CMake and add it to PATH.")
    if not shutil.which("ninja"):
        die("ninja not found on PATH. Install Ninja and add it to PATH.")


# -- Entry point ---------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build pico_gamepad_host.uf2 for Raspberry Pi Pico"
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Delete the build/ directory and reconfigure from scratch",
    )
    args = parser.parse_args()

    print("=" * 60)
    print("  pico_gamepad_host build script")
    print("=" * 60)

    check_tools()

    if args.clean and BUILD_DIR.exists():
        print(f"\n  Removing {BUILD_DIR} ...")
        # Read-only files (e.g. git pack files in _deps) need the flag cleared first
        def _remove_readonly(func, path, _exc):
            os.chmod(path, 0o777)
            func(path)
        shutil.rmtree(BUILD_DIR, onerror=_remove_readonly)

    cmake_configure(force=args.clean)
    compile_objects()
    elf = link_elf()
    uf2 = package_release(elf)

    print("\n" + "=" * 60)
    print("  BUILD COMPLETE")
    print("=" * 60)
    print(f"\n  Flash file:  {uf2}")
    print("\n  To flash your Pico:")
    print("    1. Hold the BOOTSEL button")
    print("    2. Plug in the USB cable")
    print("    3. Drag the .uf2 file onto the RPI-RP2 drive")
    print()


if __name__ == "__main__":
    main()
