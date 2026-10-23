#!/usr/bin/env python3
"""Apply a named structured corruption to a SpadFS image, in place.

Why structured and not random: the on-disk metadata checksum is `__byte_sum`
(struct.h:947), an XOR fold of 8-byte words down to 8 bits. XOR is commutative
and associative, so:

  * a single-bit flip changes the sum with certainty  -> always detected
  * swapping two whole words leaves the sum identical -> checksum-blind
  * copying a block of words elsewhere leaves the sum identical if the multiset
    of words is preserved -> checksum-blind

A random-bit test would therefore report ~100% detection and give a false
all-clear. The classes below are the ones real hardware actually produces: link
glitches, controller copy-forward, torn writes.

Exit codes:
  0  corruption applied and the bytes changed
  3  corruption was a no-op (target words already equal) -- caller must not
     count this as "detected" or "undetected", it tested nothing
  1  error
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sbinfo import SECTOR, SUPERBLOCK_SECTOR, read_super  # noqa: E402

# Offsets inside a 512-byte unit are chosen relative to the first "live" region
# (see find_live_base) so corruptions land on real structure fields rather than
# on padding that mkspadfs zeroed -- corrupting zeros over zeros tests nothing.
WIN = 192


def _word(u, off):
    return int.from_bytes(u[off:off + 8], "little")


def _setw(u, off, v):
    u[off:off + 8] = (v & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")


def find_live_base(u):
    """Anchor on the first non-zero byte, aligned down to a word boundary.

    Anchoring at a fixed offset made the crash-count table useless: its live
    entries are cct[0]/cct[1] at the very start, while a fixed base+16 landed on
    zeroed counters, so every class swapped zeros for zeros and reported NOOP.
    """
    for i, byte in enumerate(u):
        if byte:
            return (i // 8) * 8
    return None


def apply_class(u, cls, a, b, c, blk_src, blk_dst):
    """Mutate u at explicitly supplied offsets. Split out so a caller can pin
    the anchor exactly (e.g. straight at cct[tx.cc]) instead of relying on a
    search that could drift to some other live byte."""
    if cls == "bitflip":
        u[a] ^= 0x01
    elif cls == "byte_transpose":
        u[a], u[a + 1] = u[a + 1], u[a]
    elif cls == "word_swap":
        u[a:a + 8], u[b:b + 8] = u[b:b + 8], u[a:a + 8]
    elif cls == "word_dup":
        u[b:b + 8] = u[a:a + 8]
    elif cls == "block_copy":
        u[blk_dst:blk_dst + 64] = u[blk_src:blk_src + 64]
    elif cls == "xor_cancel":
        # Flip the SAME bit in two different words: the XOR of all words is
        # unchanged, so __byte_sum cannot see this, but the data differs.
        mask = 0x0000000000000001
        _setw(u, a, _word(u, a) ^ mask)
        _setw(u, c, _word(u, c) ^ mask)
    elif cls == "zero_out":
        u[a:a + 64] = bytes(64)
    else:
        raise ValueError(f"unknown corruption class: {cls}")


def corrupt_unit(u, cls):
    """Mutate a 512-byte bytearray in place. Returns False if nothing is live."""
    base = find_live_base(u)
    if base is None:
        return False
    apply_class(u, cls, base + 16, base + 24, base + 32, base + 48, base + 112)
    return True


def live_cc(path, info):
    """tx.cc -- the current crash-count index, read from the txblock.

    struct txblock (struct.h:134) puts cc at byte offset 6.
    """
    with open(path, "rb") as f:
        f.seek(info["txblock"] * SECTOR + 6)
        return int.from_bytes(f.read(2), "little")


def target_region(path, info, target):
    """(base_sector, extent_sectors, anchor_override_or_None) for a structure.

    Extents matter: several of these structures occupy only block_sectors (8
    sectors on a 4KiB block). Without a bound, a forward search for "live" bytes
    walks straight past the structure into its neighbour, so the reported target
    is a lie. Measured here: a "txblock" corruption landed at sector 16408,
    which is apage_index1, not the txblock at 16392.

    Derived from utils/mkspadfs/mkspadfs.c:331-339.
    """
    blk = 1 << info["sectors_per_block_bits"]
    cc = live_cc(path, info)
    # Byte offset of cct[cc] within its 512-byte sector.
    cc_in_sector = (cc * 4) % SECTOR
    table = {
        "superblock": (SUPERBLOCK_SECTOR, blk, None),
        "txblock": (info["txblock"], blk, None),
        "apage_index": (info["apage_index0"],
                        info["apage_index1"] - info["apage_index0"], None),
        # CCT_SIZE is 65536*4 bytes = 512 sectors; marked "do not change it".
        "cct": (info["cct"], 512, None),
        # The live entry. cct[tx.cc] is what every CC_VALID() decision reads,
        # and it is guarded by no checksum and by no ordering rule -- the rule
        # in sckcct.c only constrains entries ABOVE cc. Pin the anchor so we
        # hit that entry and nothing else.
        "cct_live": (info["cct"] + (cc * 4) // SECTOR, 1, cc_in_sector),
        # sb.root points at the root FIXED FNODE, not the directory.
        "root_fnode": (info["root"], blk, None),
        # root_dir_sector = fnode_align(root_fnode + block_sectors, ROOT_DIR_SECTORS)
        # and ROOT_DIR_SECTORS == block_sectors (mkspadfs.c:86).
        "root_dir": (info["root"] + blk, blk, None),
    }
    if target not in table:
        raise ValueError(f"unknown target: {target} (have: {', '.join(sorted(table))})")
    return table[target]


def target_offset(path, info, target):
    return target_region(path, info, target)[0] * SECTOR


def main():
    if len(sys.argv) not in (4, 6):
        sys.exit("usage: corrupt.py <image> <target> <class> [--at <byte_off>]")
    path, target, cls = sys.argv[1], sys.argv[2], sys.argv[3]
    pinned = None
    if len(sys.argv) == 6:
        if sys.argv[4] != "--at":
            sys.exit(f"corrupt.py: unknown option {sys.argv[4]}")
        pinned = int(sys.argv[5])

    info = read_super(path)
    base_sector, extent, anchor = target_region(path, info, target)
    if pinned is not None:
        anchor = pinned

    # Search within the structure's real extent only. Its leading units can be
    # all-zero (e.g. the unused tail of the crash-count table), so look for a
    # unit the class can actually change -- but never leave the structure.
    with open(path, "rb+") as f:
        for unit in range(extent):
            off = (base_sector + unit) * SECTOR
            f.seek(off)
            unit_bytes = bytearray(f.read(SECTOR))
            if len(unit_bytes) < SECTOR:
                break
            before = bytes(unit_bytes)
            if anchor is None:
                corrupt_unit(unit_bytes, cls)
            else:
                # Pinned anchor: corrupt exactly where we say, so a target like
                # cct_live cannot drift to some other live byte.
                apply_class(unit_bytes, cls, anchor, anchor + 8,
                          anchor + 16, anchor + 32, anchor + 112)
            if bytes(unit_bytes) != before:
                f.seek(off)
                f.write(unit_bytes)
                print(f"changed at {target}/{cls} byte={off + anchor if anchor is not None else off} "
                      f"sector={base_sector + unit} unit={unit}/{extent}")
                return 0
        print("noop")
        return 3


if __name__ == "__main__":
    sys.exit(main())
