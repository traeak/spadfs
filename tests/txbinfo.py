#!/usr/bin/env python3
"""Report the state of a volume's two txblock copies.

    txbinfo.py IMAGE  ->  pri=<ok|bad> bak=<ok|bad> same=<0|1> cc=<n>

    ok    magic (stamped with its own sector) and CRC trailer both verify
    same  the payloads match, ignoring magic, the legacy checksum byte and
          the trailer -- the bytes that legitimately differ between copies
    cc    tx.cc of the primary

Used by tests/txb_backup.sh and tests/mount_smoke.sh.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sbinfo import read_super
from prime import crc32c_std, META_COVER, META_CRC_POS
TXBLOCK_MAGIC = 0x4C425854            # struct.h
p = sys.argv[1]
info = read_super(p)
dyn = info["flags_compat_none"] & 0x2

def unit(sec):
    with open(p, "rb") as f:
        f.seek(sec * 512)
        return f.read(512)

def good(u, sec):
    want = TXBLOCK_MAGIC ^ (sec & 0xffffffff) if dyn else TXBLOCK_MAGIC
    return (int.from_bytes(u[0:4], "little") == want and
            int.from_bytes(u[META_CRC_POS:META_CRC_POS + 4], "little") ==
            crc32c_std(u[:META_COVER]) & 0xffffffff)

pri, bak = info["txblock"], info["txblock_backup"]
a, b = unit(pri), unit(bak)
print("pri=%s bak=%s same=%d cc=%d" % (
    "ok" if good(a, pri) else "bad", "ok" if good(b, bak) else "bad",
    a[5:META_COVER] == b[5:META_COVER], int.from_bytes(a[6:8], "little")))
