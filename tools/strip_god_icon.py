#!/usr/bin/env python3
"""
Strip the embedded icon from a GOD package header so the icon-sync code
in Omni360 will re-fetch and patch it on next launch.

Usage:
    python strip_god_icon.py <header-file>

The header file is the single file inside TitleID\00007000\ (not the .data
folder), e.g. Hdd1:\Content\0000000000000000\4D530910\00007000\AB12CD34
"""

import hashlib
import struct
import sys

HEADER_SIZE   = 0xB000
SHA1_OFFSET   = 0x032C  # where the SHA-1 lives
SHA1_DATA_OFF = 0x0344  # SHA-1 covers header[0x344:]
THUMB_SZ_OFF  = 0x1712  # BE32 content-thumbnail size
TITLE_SZ_OFF  = 0x1716  # BE32 title-thumbnail size
THUMB_DATA    = 0x171A  # content-thumbnail PNG bytes
TITLE_DATA    = 0x571A  # title-thumbnail PNG bytes
ICON_MAX      = 0x3D00


def strip(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < HEADER_SIZE:
        sys.exit(f"File is only {len(data)} bytes, expected {HEADER_SIZE:#x}")

    # Read current sizes so we can report them.
    thumb_sz = struct.unpack_from(">I", data, THUMB_SZ_OFF)[0]
    title_sz = struct.unpack_from(">I", data, TITLE_SZ_OFF)[0]

    if thumb_sz == 0 and title_sz == 0:
        print("No icon present — nothing to strip.")
        return

    print(f"Content-thumbnail: {thumb_sz} bytes  Title-thumbnail: {title_sz} bytes")

    # Zero the size fields and both data regions.
    struct.pack_into(">I", data, THUMB_SZ_OFF, 0)
    struct.pack_into(">I", data, TITLE_SZ_OFF, 0)
    data[THUMB_DATA : THUMB_DATA + ICON_MAX] = bytes(ICON_MAX)
    data[TITLE_DATA : TITLE_DATA + ICON_MAX] = bytes(ICON_MAX)

    # Recompute SHA-1 over header[0x344:].
    sha1 = hashlib.sha1(data[SHA1_DATA_OFF:HEADER_SIZE]).digest()
    data[SHA1_OFFSET : SHA1_OFFSET + 20] = sha1

    with open(path, "wb") as f:
        f.write(data)

    print(f"Icon stripped and SHA-1 updated. Run Omni360 to re-fetch it.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    strip(sys.argv[1])
