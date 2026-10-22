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


def corrupt_unit(u, cls):
    """Mutate a 512-byte bytearray in place. Returns False if nothing is live."""
    base = find_live_base(u)
    if base is None:
        return False
    a, b, c = base + 16, base + 24, base + 32
    blk_src, blk_dst = base + 48, base + 112

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
    return True


def target_offset(info, target):
    table = {
        "superblock": SUPERBLOCK_SECTOR * SECTOR,
        "txblock": info["txblock"] * SECTOR,
        "cct": info["cct"] * SECTOR,
        "apage_index": info["apage_index0"] * SECTOR,
        "root_dir": info["root"] * SECTOR,
    }
    if target not in table:
        raise ValueError(f"unknown target: {target} (have: {', '.join(sorted(table))})")
    return table[target]


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: corrupt.py <image> <target> <class>")
    path, target, cls = sys.argv[1], sys.argv[2], sys.argv[3]

    info = read_super(path)
    base_off = target_offset(info, target)

    # Many structures are longer than one 512-byte unit and their leading units
    # can be all-zero (e.g. the unused tail of the crash-count table). Search
    # forward for a unit where the corruption actually changes bytes, so a
    # NOOP means "this class cannot alter this kind of region" rather than
    # "we happened to aim at padding".
    with open(path, "rb+") as f:
        for unit in range(64):
            off = base_off + unit * SECTOR
            f.seek(off)
            unit_bytes = bytearray(f.read(SECTOR))
            if len(unit_bytes) < SECTOR:
                break
            before = bytes(unit_bytes)
            corrupt_unit(unit_bytes, cls)
            if bytes(unit_bytes) != before:
                f.seek(off)
                f.write(unit_bytes)
                print(f"changed at {target}/{cls} byte={off} unit={unit}")
                return 0
        print("noop")
        return 3


if __name__ == "__main__":
    sys.exit(main())
