#!/usr/bin/env python3
"""Report the live apage index copy and its CRC.

    apageidx.py IMAGE  ->  copy=<0|1> active=<n> stored=<hex> computed=<hex>

    copy      which index copy the txblock's a_cc/a_txc select
    active    entries with a non-zero end_sector (apages in use)
    stored    that copy's CRC in the txblock (apage_index_crc[copy])
    computed  CRC-32C of the copy's first n_apages entries on disk

Little-endian volumes only. Used by tests/apage_index_crc.sh and
tests/mount_smoke.sh.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sbinfo import read_super  # noqa: E402
from prime import crc32c_std  # noqa: E402
from corrupt import n_apages  # noqa: E402

p = sys.argv[1]
info = read_super(p)
with open(p, "rb") as f:
    f.seek(info["txblock"] * 512)
    tx = f.read(512)
    a_cc = int.from_bytes(tx[10:12], "little")
    a_txc = int.from_bytes(tx[12:16], "little")
    f.seek(info["cct"] * 512 + a_cc * 4)
    cct = int.from_bytes(f.read(4), "little")
    copy = 0 if ((cct - a_txc) & 0xFFFFFFFF) < 0x80000000 else 1
    f.seek(info["apage_index%d" % copy] * 512)
    entries = f.read(n_apages(info) * 16)
active = sum(1 for i in range(0, len(entries), 16)
             if entries[i + 8:i + 16] != bytes(8))
stored = int.from_bytes(tx[32 + 4 * copy:36 + 4 * copy], "little")
print("copy=%d active=%d stored=%08x computed=%08x" % (
    copy, active, stored, crc32c_std(entries) & 0xFFFFFFFF))
