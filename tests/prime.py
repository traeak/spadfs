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
    """Set the checksum byte so XOR(all 512 bytes) == CHECKSUM_BASE."""
    u[cksum_off] ^= xor_all(u) ^ CHECKSUM_BASE


def rd32(u, off):
    return int.from_bytes(u[off:off + 4], "little")


def wr32(u, off, val):
    u[off:off + 4] = (val & 0xFFFFFFFF).to_bytes(4, "little")


def rd16(u, off):
    return int.from_bytes(u[off:off + 2], "little")


def wr16(u, off, val):
    u[off:off + 2] = (val & 0xFFFF).to_bytes(2, "little")


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
        restripe(tx, TX_CHECKSUM)

        # Sanity: the stripe must hold.
        assert xor_all(tx) == CHECKSUM_BASE, "txblock restripe failed"

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
