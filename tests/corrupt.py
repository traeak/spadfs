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
from prime import crc32c_std, META_COVER, META_CRC_POS  # noqa: E402

# ---------------------------------------------------------------------------
# Restripe
#
# Recompute whatever checksum protects a unit so that it MATCHES the corrupted
# payload. With the checksum satisfied by construction, any detection fsck
# still performs is purely structural -- which is the only way to attribute a
# detection to the checksum layer at all.
#
# Without this, "fsck detects more" is unattributable. The magic checks,
# field-range checks and cross-reference checks were already catching most
# corruption classes before Phase 2 and still would be. The existing baseline
# says exactly that: word_swap and xor_cancel were DETECTED on every target
# despite xor_delta == 0x00, i.e. caught structurally, not by the checksum.
#
# So the measurement is per (target, class):
#   normal    = corrupt payload, leave checksum stale
#   restriped = corrupt payload, then fix the checksum to match
#   checksum contribution = detected(normal) and NOT detected(restriped)
#
# Which protection each target carries was read off the verify call sites, not
# guessed:
#   trailer (crc32c)  scksuper.c:220/369/420, sckfxfn.c:76, sckfile.c:49
#   cct side CRC      sckcct.c:158 (region written by mkspadfs, sealed per unit)
#   legacy __byte_sum sckdir.c:322 (fnode_block -- not converted, Cat 2 blocker)
#   NONE              apage_index -- no checksum call site exists for it at all
# ---------------------------------------------------------------------------

TRAILER_TARGETS = {"superblock", "txblock", "root_fnode"}
CCT_SIDE_TARGETS = {"cct", "cct_live"}
XOR_TARGETS = {"root_dir"}
UNPROTECTED_TARGETS = {"apage_index"}

CHECKSUM_BASE = ord("C")          # struct.h:29
FNODE_CHECKSUM_OFF = 5            # magic(4) + flags(1)
CCT_CRC_PER_UNIT = META_COVER // 4    # 127, mirrors struct.h
CCT_SECTORS = 512


def byte_sum(u, length):
    """Replicate __byte_sum (struct.h:976) exactly.

    XOR of 8-byte words, then folded to 8 bits. The C writes `>> 31 >> 1`
    rather than `>> 32` on unsigned long; mirrored here rather than simplified,
    because a "cleaner" equivalent is exactly the kind of change that quietly
    stops matching the kernel.
    """
    total = 0
    for off in range(0, length, 8):
        total ^= int.from_bytes(u[off:off + 8], "little")
    total &= 0xFFFFFFFFFFFFFFFF
    total ^= (total >> 31) >> 1
    total ^= total >> 16
    total ^= total >> 8
    return total & 0xFF


def restripe_trailer(u):
    """Re-seal a standard 512-byte trailer: crc32c over bytes 0..507."""
    u[META_CRC_POS:META_CRC_POS + 4] = (
        crc32c_std(bytes(u[:META_COVER])) & 0xFFFFFFFF).to_bytes(4, "little")


def restripe_xor_fnode_block(u):
    """Apply the XOR update trick so __byte_sum(block,512) == CHECKSUM_BASE.

    Same identity the tree relies on at sckdir.c:1034: folding the old
    checksum out and the new one in lands the total on CHECKSUM_BASE.
    """
    u[FNODE_CHECKSUM_OFF] ^= CHECKSUM_BASE ^ byte_sum(u, 512)
    u[FNODE_CHECKSUM_OFF] &= 0xFF
    if byte_sum(u, 512) != CHECKSUM_BASE:
        raise AssertionError("xor restripe failed to satisfy the fnode_block invariant")


def restripe_cct_side(f, info, cct_sector):
    """Repoint the side CRC for one cct sector at the corrupted bytes.

    The cct carries no trailer of its own (Phase 2.3 option A), so restriping
    it means rewriting the CRC slot in the side region AND re-sealing that
    CRC unit -- otherwise the unit trailer goes stale and fsck fails the unit
    rather than the sector, which would be detected for the wrong reason.
    """
    u = cct_sector // CCT_CRC_PER_UNIT
    slot = cct_sector % CCT_CRC_PER_UNIT
    crc_abs = info["cct_crc"] + u
    f.seek(crc_abs * SECTOR)
    unit = bytearray(f.read(SECTOR))
    if len(unit) < SECTOR:
        raise IOError("short read of cct crc unit")

    f.seek(cct_sector * SECTOR)   # already-corrupted cct sector, as written
    data = f.read(SECTOR)

    unit[slot * 4:slot * 4 + 4] = (
        crc32c_std(data) & 0xFFFFFFFF).to_bytes(4, "little")
    restripe_trailer(unit)
    f.seek(crc_abs * SECTOR)
    f.write(unit)


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
    argv = sys.argv[1:]
    if len(argv) < 3:
        sys.exit("usage: corrupt.py <image> <target> <class> "
                "[--at <byte_off>] [--restripe]")
    path, target, cls = argv[0], argv[1], argv[2]
    pinned = None
    restripe = False

    i = 3
    while i < len(argv):
        if argv[i] == "--at":
            if i + 1 >= len(argv):
                sys.exit("corrupt.py: --at needs a value")
            pinned = int(argv[i + 1])
            i += 2
        elif argv[i] == "--restripe":
            restripe = True
            i += 1
        else:
            sys.exit(f"corrupt.py: unknown option {argv[i]}")

    info = read_super(path)
    base_sector, extent, anchor = target_region(path, info, target)
    if pinned is not None:
        anchor = pinned

    if restripe and target in UNPROTECTED_TARGETS:
        # Nothing to restripe: this structure has no checksum in this format.
        # Reported explicitly rather than silently treated as "restriped",
        # because a no-op here would otherwise look like a measurement.
        print("restripe: no-op, target has NO checksum protection")

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

                if restripe:
                    if target in TRAILER_TARGETS:
                        # Re-seal the unit we just wrote.
                        f.seek(off)
                        cur = bytearray(f.read(SECTOR))
                        restripe_trailer(cur)
                        f.seek(off)
                        f.write(cur)
                    elif target in XOR_TARGETS:
                        f.seek(off)
                        cur = bytearray(f.read(SECTOR))
                        restripe_xor_fnode_block(cur)
                        f.seek(off)
                        f.write(cur)
                    elif target in CCT_SIDE_TARGETS:
                        restripe_cct_side(f, info, base_sector + unit)

                print(f"changed at {target}/{cls} byte={off + anchor if anchor is not None else off} "
                      f"sector={base_sector + unit} unit={unit}/{extent}"
                      f"{' RESTIPED' if restripe else ''}")
                return 0
        print("noop")
        return 3


if __name__ == "__main__":
    sys.exit(main())
