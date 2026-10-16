#!/usr/bin/env python3
"""Embed the standalone game assets. No original ROM is read or required."""
import hashlib
import json
from pathlib import Path
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: pack_assets.py ASSET_DIRECTORY OUTPUT_C")
    directory, destination = map(Path, sys.argv[1:])
    index = json.loads((directory / "index.json").read_text())
    payload = (directory / "game-data.bin").read_bytes()
    if (index.get("format") != "sml2-native-assets-v1"
            or index.get("address_space_size") != 0x80000
            or len(payload) != index.get("payload_size")
            or hashlib.sha256(payload).hexdigest() != index.get("payload_sha256")):
        raise SystemExit("invalid or damaged game asset bundle")
    previous_end = payload_end = 0
    lines = ['/* Generated from the standalone game asset bundle. */',
             '#include "../assets.h"',
             'const uint8_t game_assets[GAME_ASSET_SPACE_SIZE] = {']
    for segment in index["segments"]:
        address, offset, size = (segment[key] for key in ("address", "offset", "size"))
        if (any(type(value) is not int for value in (address, offset, size))
                or address < previous_end or offset != payload_end or size <= 0
                or address + size > 0x80000 or offset + size > len(payload)):
            raise SystemExit("invalid game asset segment")
        lines.append(f"    [0x{address:05x}] =")
        for cursor in range(offset, offset + size, 32):
            lines.append("    " + ",".join(f"0x{value:02x}" for value in
                         payload[cursor:min(cursor + 32, offset + size)]) + ",")
        previous_end, payload_end = address + size, offset + size
    if payload_end != len(payload):
        raise SystemExit("unmapped game asset bytes")
    lines.append("};")
    destination.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
