#!/usr/bin/env python3
"""Prime the crash-count table so it looks like a volume that has actually been
mounted and run through some crash/transaction cycles.

Why this exists: on a freshly formatted volume the entire 256 KiB cct is zeros,
so every corruption class is a no-op against it and the harness reports NOOP.
That left the single most important finding in this tree -- the cct has no
checksum at all -- supported only by source inspection. This makes it testable.

Validation rules read from utils/spadfsck/sckcct.c:45-56:
  * entry index <= tx.cc : must NOT have the 0x80000000 bit set
  * entry index >  tx.cc : must be zero
  * 1-2 bad entries in a sector -> "INVALID ENTRIES" (repairable)
  * 3+ bad entries in a sector  -> "INVALID DATA", sector not loaded

tx.cc lives inside the checksummed txblock, so changing it requires restriping
the block checksum. The invariant follows from super.c:507
    tb->checksum ^= CHECKSUM_BASE ^ __byte_sum(tb, 512);
i.e. after a valid stripe, the XOR of all 512 bytes equals CHECKSUM_BASE.
"""

import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sbinfo import SECTOR, read_super  # noqa: E402

CHECKSUM_BASE = 0x43  # struct.h:29  ((__u8)'C')

# struct txblock offsets (struct.h:134-146)
TX_MAGIC = 0
TX_CHECKSUM = 4
TX_CC = 6
TX_A_CC = 8
TX_A_TXC = 10


def xor_all(buf):
    v = 0
    for b in buf:
        v ^= b
    return v


def restripe(u, cksum_off):
    """Set the checksum byte so XOR(all 512 bytes) == CHECKSUM_BASE.

    Legacy: only valid for units still using the 8-bit XOR field. Cat 1
    (Phase 2.2) moved superblock/txblock/userblock to the CRC trailer --
    those must use seal_unit() instead, or the trailer goes stale and fsck
    correctly rejects the fixture.
    """
    u[cksum_off] ^= xor_all(u) ^ CHECKSUM_BASE


def seal_unit(u, legacy_cksum_off):
    """
    Zero the legacy 8-bit XOR field and write the CRC-32C trailer, matching
    spadfs_tx_block_checksum() / spadfs_meta_seal() exactly.

    The legacy field is zeroed rather than left alone so the sealed bytes are
    deterministic -- the same reason the kernel does it.
    """
    u[legacy_cksum_off] = 0
    struct.pack_into("<I", u, META_CRC_POS, crc32c_std(bytes(u[:META_COVER])))


def rd32(u, off):
    return int.from_bytes(u[off:off + 4], "little")


def wr32(u, off, val):
    u[off:off + 4] = (val & 0xFFFFFFFF).to_bytes(4, "little")


def rd16(u, off):
    return int.from_bytes(u[off:off + 2], "little")


def wr16(u, off, val):
    u[off:off + 2] = (val & 0xFFFF).to_bytes(2, "little")


# ---------------------------------------------------------------------------
# CRC-32C (Castagnoli), matching meta_crc.h / utils/crc32c.c exactly.
#
# Python has no built-in crc32c (zlib.crc32 is the *other* polynomial), so the
# table is built here. Reflected poly 0x82F63B78, init ~0, final invert -- the
# same convention spadfs_meta_seal uses. Verified against the published check
# vector in the self-test below.
# ---------------------------------------------------------------------------
_CRC32C_POLY = 0x82F63B78


def _crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ _CRC32C_POLY if (c & 1) else (c >> 1)
        table.append(c)
    return table


_CRC32C = _crc32c_table()


def crc32c_std(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC32C[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# meta_crc.h geometry
META_UNIT = 512
META_CRC_BYTES = 4
META_COVER = META_UNIT - META_CRC_BYTES          # 508
META_CRC_POS = META_COVER                       # 508

# struct.h cct geometry
CCT_SECTORS = 512          # CCT_SIZE >> SSIZE_BITS
CCT_CRC_PER_UNIT = META_COVER // 4             # 127
CCT_CRC_UNITS = (CCT_SECTORS + CCT_CRC_PER_UNIT - 1) // CCT_CRC_PER_UNIT  # 5


def restripe_cct_crc(f, info):
    """
    Recompute the cct side CRC region from the cct as it now stands on disk.

    prime.py writes the cct out of band, so it owns the responsibility of
    keeping the side table consistent with it. Skipping this makes fsck report
    a mismatch -- which is the new check working, but it breaks the fixture.
    """
    cct_off = info["cct"] * SECTOR
    crc_off = info["cct_crc"] * SECTOR

    f.seek(cct_off)
    cct = f.read(CCT_SECTORS * SECTOR)
    if len(cct) != CCT_SECTORS * SECTOR:
        sys.exit("prime: short read of cct")

    crcs = [crc32c_std(cct[i * SECTOR:(i + 1) * SECTOR])
            for i in range(CCT_SECTORS)]

    for u in range(CCT_CRC_UNITS):
        unit = bytearray(META_UNIT)
        for i in range(CCT_CRC_PER_UNIT):
            sector = u * CCT_CRC_PER_UNIT + i
            if sector >= CCT_SECTORS:
                break
            struct.pack_into("<I", unit, i * 4, crcs[sector])
        struct.pack_into("<I", unit, META_CRC_POS,
                        crc32c_std(bytes(unit[:META_COVER])))
        f.seek(crc_off + u * SECTOR)
        f.write(unit)


def _self_test():
    # published CRC-32C check vector
    assert crc32c_std(b"123456789") == 0xE3069283, "crc32c mismatch"
    assert crc32c_std(b"a") == 0xC1D04330, "crc32c mismatch"
    assert CCT_CRC_UNITS == 5 and CCT_CRC_PER_UNIT == 127


_self_test()

def prime(path, new_cc, base_txc, dry_run=False):
    info = read_super(path)
    tx_off = info["txblock"] * SECTOR
    cct_off = info["cct"] * SECTOR

    with open(path, "rb+") as f:
        f.seek(tx_off)
        tx = bytearray(f.read(SECTOR))
        if len(tx) < SECTOR:
            sys.exit("prime: short txblock")

        old_cc = rd16(tx, TX_CC)
        old_sum = xor_all(tx)

        # The volume must still be self-consistent: a_cc indexes the alternate
        # apage index and must not exceed the current cc.
        if rd16(tx, TX_A_CC) > new_cc:
            wr16(tx, TX_A_CC, new_cc)

        wr16(tx, TX_CC, new_cc)
        # The txblock is a Cat 1 unit: trailer-authoritative. Restriping the
        # old XOR byte would leave the trailer stale.
        seal_unit(tx, TX_CHECKSUM)

        # Sanity: the trailer must verify against what we just wrote.
        assert struct.unpack_from("<I", tx, META_CRC_POS)[0] == \
               crc32c_std(bytes(tx[:META_COVER])), "txblock seal failed"

        if dry_run:
            print(f"dry-run: cc {old_cc} -> {new_cc}, "
                  f"txblock xor {old_sum:#04x} -> {xor_all(tx):#04x}")
            return

        f.seek(tx_off)
        f.write(tx)

        # Write plausible transaction counts into cct[0..new_cc].
        # Values must have the top bit clear; increasing counts read as a
        # volume that committed work across several crash cycles.
        for i in range(new_cc + 1):
            off = cct_off + i * 4
            val = (base_txc + i * 97) & 0x7FFFFFFF
            f.seek(off)
            f.write(val.to_bytes(4, "little"))

        # The side CRC table must describe the cct we just wrote, or fsck's
        # new checksum pass reports the fixture itself as corrupt.
        restripe_cct_crc(f, info)

    print(f"primed: cc {old_cc} -> {new_cc}, "
          f"cct[0..{new_cc}] = "
          f"{[(base_txc + i * 97) & 0x7FFFFFFF for i in range(new_cc + 1)]}")


def fsck_clean(path):
    r = subprocess.run([os.environ.get("FSCK", "./spadfsck"), "-n", "-f", path],
                      capture_output=True, text=True)
    return r.returncode == 0, r.stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument("image")
    p.add_argument("--cc", type=int, default=4, help="crash count to set")
    p.add_argument("--base-txc", type=int, default=1000)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--verify", action="store_true",
                  help="prime a copy and confirm spadfsck still calls it clean")
    a = p.parse_args()

    if a.verify:
        tmp = a.image + ".primed"
        subprocess.run(["cp", "-f", a.image, tmp], check=True)
        prime(tmp, a.cc, a.base_txc, a.dry_run)
        ok, out = fsck_clean(tmp)
        print(f"spadfsck -n -f on primed volume: {'CLEAN' if ok else 'DIRTY'}")
        if not ok:
            print("\n".join(out.splitlines()[:12]))
        sys.exit(0 if ok else 1)

    prime(a.image, a.cc, a.base_txc, a.dry_run)


if __name__ == "__main__":
    main()
