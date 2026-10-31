#!/usr/bin/env python3
"""Inspect and tamper with the cct backup epoch stamp (Task 3.3).

The stamp lives in the backup CRC region's last unit, after its CRC slots and
inside the sealed cover (struct.h, CCT_BACKUP_STAMP_*):

    unit CCT_CRC_UNITS - 1, byte offset 16:
        u32 magic  "CCTS" = 0x53544343
        u32 cc     crash count the mirror was written in
        u32 txc    cct[cc] as written
        u32 reserved (0)

Commands (all print key=value lines):
    show IMG             stamp fields, tx.cc, and whether primary == mirror
    set  IMG FIELD VALUE overwrite one stamp field and reseal the unit

Little-endian volumes only, which is what the test fixtures create.
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sbinfo import read_super, SECTOR  # noqa: E402
from prime import crc32c_std  # noqa: E402

CCT_SECTORS = 512
CRC_PER_UNIT = 127
CRC_UNITS = (CCT_SECTORS + CRC_PER_UNIT - 1) // CRC_PER_UNIT
STAMP_UNIT = CRC_UNITS - 1
STAMP_OFF = (CCT_SECTORS - STAMP_UNIT * CRC_PER_UNIT) * 4
STAMP_MAGIC = 0x53544343
FIELDS = ("magic", "cc", "txc", "reserved")
COVER = 508


def _unit_pos(img):
    return (read_super(img)["cct_backup_crc"] + STAMP_UNIT) * SECTOR


def read_stamp(img):
    with open(img, "rb") as f:
        f.seek(_unit_pos(img))
        unit = f.read(SECTOR)
    vals = struct.unpack_from("<4I", unit, STAMP_OFF)
    sealed = crc32c_std(unit[:COVER]) == struct.unpack_from("<I", unit, COVER)[0]
    return dict(zip(FIELDS, vals)), sealed


def set_stamp(img, field, value):
    pos = _unit_pos(img)
    with open(img, "r+b") as f:
        f.seek(pos)
        unit = bytearray(f.read(SECTOR))
        struct.pack_into("<I", unit, STAMP_OFF + 4 * FIELDS.index(field), value)
        struct.pack_into("<I", unit, COVER, crc32c_std(bytes(unit[:COVER])))
        f.seek(pos)
        f.write(unit)


def show(img):
    d = read_super(img)
    with open(img, "rb") as f:
        f.seek(d["txblock"] * SECTOR + 6)
        tx_cc = struct.unpack("<H", f.read(2))[0]
        f.seek(d["cct"] * SECTOR)
        prim = f.read(CCT_SECTORS * SECTOR)
        f.seek(d["cct_backup"] * SECTOR)
        bak = f.read(CCT_SECTORS * SECTOR)
    st, sealed = read_stamp(img)
    print("magic=%s" % ("ok" if st["magic"] == STAMP_MAGIC else "%08x" % st["magic"]))
    print("stamp_cc=%d" % st["cc"])
    print("stamp_txc=%d" % st["txc"])
    print("sealed=%s" % ("yes" if sealed else "no"))
    print("tx_cc=%d" % tx_cc)
    print("primary_txc=%d" % struct.unpack_from("<I", prim, 4 * tx_cc)[0])
    print("mirror=%s" % ("same" if prim == bak else "differs"))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "show":
        show(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "set" and sys.argv[3] in FIELDS:
        set_stamp(sys.argv[2], sys.argv[3], int(sys.argv[4], 0))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
