#!/usr/bin/env python3
"""Embed the exact selected ROM as read-only asset data; no CPU code generation."""
import hashlib
import pathlib
import sys

source, destination = map(pathlib.Path, sys.argv[1:])
data = source.read_bytes()
expected = "5450dce1bd0c073964c374b5b5b5729dce8d00f2e807892c34af32b8bce1392e"
if len(data) != 524288 or hashlib.sha256(data).hexdigest() != expected:
    raise SystemExit("Refusing unknown ROM revision: expected original SML2 512 KiB input")
with destination.open("w") as out:
    out.write("/* Original ROM bytes, used only as asset/table data. */\n")
    out.write("#include <stdint.h>\nconst uint8_t game_rom[524288] = {\n")
    for offset in range(0, len(data), 32):
        out.write(",".join(f"0x{b:02x}" for b in data[offset:offset+32]) + ",\n")
    out.write("};\n")
