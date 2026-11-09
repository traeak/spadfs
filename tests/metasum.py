#!/usr/bin/env python3
"""Report the 8-bit checksums of every live apage half and dnode in an image.

    metasum.py IMAGE            summary lines, key=value
    metasum.py IMAGE --list     one line per structure as well

For each ACTIVE apage half (picked by its cc/txc, as the kernel and spadfsck
do) and each dnode's ACTIVE version:

    sealed    the CHECKSUM_VALID flag is set
    good      sealed and the checksum matches
    bad       sealed and the checksum does NOT match
    unsealed  flag clear -- nothing checks it

The formulas are the ones spadfsck verifies with (sckapage.c, sckdir.c):

    apage half   xor(half[8:]) == CHECKSUM_BASE
    dnode        xor(entries[version]) == CHECKSUM_BASE ^ checksum[version]

Inactive halves and versions are not judged: a crash may leave them torn.
Used by tests/meta_csum_mount.sh to show the kernel seals what it writes.
Little-endian volumes only, like the rest of the fixtures.
"""

import struct
import sys

import sbinfo

CHECKSUM_BASE = 0x43
APAGE_MAGIC = 0x5041
APAGE_CHECKSUM_VALID = 0x40
DNODE_PAGE_MAGIC = 0x444F4E44
DNODE_CHECKSUM_VALID = 0x01
DNODE_ENTRY_OFFSET = 24
FLAG_COMPAT_NONE_DYNAMIC_MAGIC = 0x2


def xor(b):
    s = 0
    # fold 8 bytes at a time, then the byte lanes
    n = len(b) // 8 * 8
    for (w,) in struct.iter_unpack("<Q", b[:n]):
        s ^= w
    for x in b[n:]:
        s ^= x
    s ^= s >> 32
    s ^= s >> 16
    s ^= s >> 8
    return s & 0xff


class Volume:
    def __init__(self, path):
        self.sb = sbinfo.read_super(path)
        if self.sb["byte_sex"] != "little":
            raise ValueError("little-endian volumes only")
        with open(path, "rb") as f:
            self.img = f.read()
        tx = self.sec(self.sb["txblock"])
        self.a_cc, self.a_txc = struct.unpack_from("<Hi", tx, 10)
        self.page = 512 << self.sb["sectors_per_page_bits"]

    def sec(self, n, count=1):
        return self.img[n * 512:(n + count) * 512]

    def cct(self, cc):
        return struct.unpack_from("<I", self.img, self.sb["cct"] * 512 + cc * 4)[0]

    def cc_valid(self, cc, txc):
        d = (self.cct(cc) - txc) & 0xffffffff
        return d < 0x80000000           # (s32)(cct[cc] - txc) >= 0

    def apages(self):
        """(index, sector, active half bytes, half number) for used apages."""
        which = 0 if self.cc_valid(self.a_cc, self.a_txc) else 1
        base = self.sb["apage_index0" if which == 0 else "apage_index1"]
        limit = abs(self.sb["apage_index1"] - self.sb["apage_index0"]) * 512
        idx = self.img[base * 512:base * 512 + limit]
        out = []
        for i in range(len(idx) // 16):
            apage, end = struct.unpack_from("<QQ", idx, i * 16)
            if not end:
                break
            page = self.img[apage * 512:apage * 512 + self.page]
            magic, cc, txc = struct.unpack_from("<HHi", page, 0)
            if magic != APAGE_MAGIC:
                out.append((i, apage, None, None))
                continue
            half = 0 if self.cc_valid(cc, txc) else 1
            h = self.page // 2
            out.append((i, apage, page[half * h:(half + 1) * h], half))
        return out

    def dnodes(self):
        """(sector, header, active version) for every dnode page found."""
        dyn = self.sb["flags_compat_none"] & FLAG_COMPAT_NONE_DYNAMIC_MAGIC
        bs = 1 << self.sb["sectors_per_block_bits"]
        gflags_want = self.sb["sectors_per_page_bits"] - 1
        out = []
        for s in range(0, len(self.img) // 512, bs):
            (m,) = struct.unpack_from("<I", self.img, s * 512)
            want = DNODE_PAGE_MAGIC ^ (s & 0xffffffff) if dyn else DNODE_PAGE_MAGIC
            if m != want or (self.img[s * 512 + 6] & 7) != gflags_want:
                continue
            txc, cc = struct.unpack_from("<iH", self.img, s * 512 + 16)
            out.append((s, not self.cc_valid(cc, txc)))
        return out

    def dnode_data_size(self):
        pagesize_bits = self.sb["sectors_per_page_bits"] + 9
        return 6 << (pagesize_bits - 3 - 1)


def judge(vol):
    rows = []
    for i, sec, half, which in vol.apages():
        if half is None:
            rows.append(("apage", sec, "bad-magic"))
            continue
        flags = half[8]
        if not flags & APAGE_CHECKSUM_VALID:
            rows.append(("apage", sec, "unsealed"))
        elif xor(half[8:]) == CHECKSUM_BASE:
            rows.append(("apage", sec, "good"))
        else:
            rows.append(("apage", sec, "bad"))
    ds = vol.dnode_data_size()
    for sec, version in vol.dnodes():
        hdr = vol.img[sec * 512:sec * 512 + DNODE_ENTRY_OFFSET]
        flags, checksum = hdr[4 + version], hdr[22 + version]
        off = sec * 512 + DNODE_ENTRY_OFFSET + version * ds
        if not flags & DNODE_CHECKSUM_VALID:
            rows.append(("dnode", sec, "unsealed"))
        elif xor(vol.img[off:off + ds]) == CHECKSUM_BASE ^ checksum:
            rows.append(("dnode", sec, "good"))
        else:
            rows.append(("dnode", sec, "bad"))
    return rows


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: metasum.py IMAGE [--list]")
    vol = Volume(sys.argv[1])
    rows = judge(vol)
    if "--list" in sys.argv[2:]:
        for kind, sec, verdict in rows:
            print("%s %d %s" % (kind, sec, verdict))
    for kind in ("apage", "dnode"):
        mine = [v for k, _, v in rows if k == kind]
        print("%s_total=%d" % (kind, len(mine)))
        for v in ("good", "bad", "unsealed", "bad-magic"):
            print("%s_%s=%d" % (kind, v.replace("-", "_"), mine.count(v)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
