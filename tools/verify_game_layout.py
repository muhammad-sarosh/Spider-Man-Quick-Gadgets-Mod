"""Offline verifier for the QuickGadgets 4.0630.0.0 native layout."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path


EXPECTED_SHA256 = "E297D4D94F1FFE4FEBF289745E79E7B6FA233A788E7A00F480FC77C55DB81AD1"
IMAGE_BASE = 0x140000000

EXPECTED_BYTES = {
    0x09A5FF0: bytes.fromhex("85 D2 0F 84 0C 01 00 00"),
    0x09A4110: bytes.fromhex("48 89 5C 24 08 57 48 81"),
    0x15A0560: bytes.fromhex("44 8B 01 41 8B D0 C1 EA"),
    0x16798F0: bytes.fromhex("8B 11 8B CA C1 E9 14 85"),
    0x09098C0: bytes.fromhex("85 D2 0F 84 A0 00 00 00"),
}

EXPECTED_VTABLE_ENTRIES = {
    (0x38B55C8, 37): 0x09A4110,
    (0x38B55C8, 39): 0x09A42B0,
    (0x38B55C8, 40): 0x09A43A0,
    (0x38B59B8, 37): 0x09A4110,
}

EXPECTED_ACTION_HASHES = {
    "kUseGadget": 0x9D43C80F,
    "kAttack": 0x2B24146B,
    "kDodge": 0x7CA907FC,
    "kJump": 0xD69724B0,
    "kWebStrike": 0x775E96E1,
    "kEquipGadgetLeft": 0x39BC2B52,
    "kEquipGadgetRight": 0xA08798E7,
    "kEquipGadgetUp": 0xBE2CCCA6,
}


def insomniac_crc32(text: str) -> int:
    value = 0xEDB88320
    for byte in text.encode("ascii"):
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xEDB88320 if value & 1 else 0)
    return value & 0xFFFFFFFF


class PortableExecutable:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE file (missing MZ header)")
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise ValueError("not a PE file (missing PE signature)")
        coff = pe_offset + 4
        section_count = struct.unpack_from("<H", data, coff + 2)[0]
        optional_size = struct.unpack_from("<H", data, coff + 16)[0]
        section_table = coff + 20 + optional_size
        self.sections: list[tuple[int, int, int, int]] = []
        for index in range(section_count):
            header = section_table + index * 40
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
                "<IIII", data, header + 8
            )
            self.sections.append((virtual_address, virtual_size, raw_offset, raw_size))

    def offset_for_rva(self, rva: int) -> int:
        for virtual_address, virtual_size, raw_offset, raw_size in self.sections:
            extent = max(virtual_size, raw_size)
            if virtual_address <= rva < virtual_address + extent:
                offset = raw_offset + rva - virtual_address
                if offset >= len(self.data):
                    break
                return offset
        raise ValueError(f"RVA 0x{rva:X} is outside mapped sections")

    def read_rva(self, rva: int, size: int) -> bytes:
        offset = self.offset_for_rva(rva)
        return self.data[offset : offset + size]


def verify(path: Path) -> list[str]:
    failures: list[str] = []
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest().upper()
    if digest != EXPECTED_SHA256:
        failures.append(f"SHA-256 mismatch: {digest}")

    pe = PortableExecutable(data)
    for rva, expected in EXPECTED_BYTES.items():
        actual = pe.read_rva(rva, len(expected))
        if actual != expected:
            failures.append(
                f"bytes at RVA 0x{rva:X}: expected {expected.hex(' ')}, got {actual.hex(' ')}"
            )

    for (vtable_rva, slot), expected_function_rva in EXPECTED_VTABLE_ENTRIES.items():
        entry = pe.read_rva(vtable_rva + slot * 8, 8)
        actual_va = struct.unpack("<Q", entry)[0]
        expected_va = IMAGE_BASE + expected_function_rva
        if actual_va != expected_va:
            failures.append(
                f"vtable 0x{vtable_rva:X}[{slot}]: expected 0x{expected_va:X}, got 0x{actual_va:X}"
            )

    for name, expected in EXPECTED_ACTION_HASHES.items():
        actual = insomniac_crc32(name)
        if actual != expected:
            failures.append(
                f"action hash {name}: expected 0x{expected:08X}, got 0x{actual:08X}"
            )
        if name.encode("ascii") not in data:
            failures.append(f"action name {name} is not embedded in the executable")

    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path, help="path to Spider-Man.exe")
    args = parser.parse_args()
    if not args.exe.is_file():
        print(f"error: file not found: {args.exe}", file=sys.stderr)
        return 2
    try:
        failures = verify(args.exe)
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if failures:
        print("QuickGadgets layout verification FAILED:")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print("QuickGadgets layout verification passed for Spider-Man.exe 4.0630.0.0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
