#!/usr/bin/env python3
"""Parse a SpadFS superblock and emit its fields as key=value lines.

Shared by the round-trip matrix (to assert mkspadfs options actually landed on
disk) and the corruption harness (to locate every metadata target without
hand-coded offsets).

Field offsets are taken from `struct superblock` in struct.h. All on-disk values
are little/big-endian per the volume's own `byte_sex`, which we read first.
"""

import struct
import sys

SUPERBLOCK_SECTOR = 16384
SECTOR = 512

# (name, offset, format) within struct superblock
FIELDS = [
    ("sectors_per_block_bits",     20, "B"),
    ("sectors_per_fnodepage_bits", 21, "B"),
    ("sectors_per_page_bits",      22, "B"),
    ("sectors_per_cluster_bits",   24, "B"),
    ("sectors_per_group_bits",     25, "B"),
    ("cluster_threshold",          26, "H"),
    ("small_file_group",           28, "H"),
    ("large_file_group",           30, "H"),
    ("size",                      32, "Q"),
    ("reserve_sectors",           40, "Q"),
    ("txblock",                   48, "Q"),
    ("apage_index0",              56, "Q"),
    ("apage_index1",              64, "Q"),
    ("cct",                       72, "Q"),
    ("root",                      80, "Q"),
    ("cct_crc",                   88, "Q"),  # was pad2
    ("flags_compat_fsck",         96, "I"),
    ("flags_compat_rw",          100, "I"),
    ("flags_compat_ro",          104, "I"),
    ("flags_compat_none",        108, "I"),
]

# struct superblock is little-endian on disk only if byte_sex reads ascending.
BYTE_SEX_ASCENDING = 0x0123456789ABCDEF


def read_super(path):
    with open(path, "rb") as f:
        f.seek(SUPERBLOCK_SECTOR * SECTOR)
        sb = f.read(SECTOR)
    if len(sb) < SECTOR:
        raise ValueError("short read of superblock")

    sig = sb[0:8]
    if sig != b"FS SPAD2":
        raise ValueError(f"bad superblock signature: {sig!r}")

    (byte_sex,) = struct.unpack_from("<Q", sb, 8)
    endian = "<" if byte_sex == BYTE_SEX_ASCENDING else ">"

    out = {
        "signature": sig.rstrip(b"\x00").decode("latin1"),
        "byte_sex": "little" if endian == "<" else "big",
        "version": f"{sb[16]}.{sb[17]}.{sb[18]}",
        "checksum": sb[19],
    }
    for name, off, fmt in FIELDS:
        (val,) = struct.unpack_from(endian + fmt, sb, off)
        out[name] = val
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: sbinfo.py <image>")
    try:
        info = read_super(sys.argv[1])
    except (OSError, ValueError) as e:
        sys.stderr.write(f"sbinfo: {e}\n")
        return 1
    for k, v in info.items():
        if isinstance(v, int) and k.startswith("flags_"):
            print(f"{k}=0x{v:08x}")
        else:
            print(f"{k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
