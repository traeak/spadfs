#!/usr/bin/env python3
"""Unprivileged checks for tests/metasum.py, the apage/dnode checksum scanner
that tests/meta_csum_mount.sh relies on to show the kernel seals what it
writes. Each verdict is cross-checked against spadfsck where it can be.

Also checks that checksums cannot be switched off: spadfsck verifies a block
whatever its *_CHECKSUM_VALID bit says, and refuses a volume with a bit set in
flags_compat_fsck (the removed NO_CHECKSUMS flag).

Run: python3 tests/metasum_test.py
"""

import functools
import os
import shutil
import subprocess
import sys
import tempfile

TESTS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)
import metasum  # noqa: E402
import sbinfo  # noqa: E402
from corrupt import restripe_trailer  # noqa: E402

FSCK = os.path.join(REPO, "spadfsck2")
PASS = FAIL = 0


def ok(cond, name, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  PASS  " + name)
    else:
        FAIL += 1
        print("  FAIL  " + name + (("\n        " + detail) if detail else ""))


def counts(img):
    rows = metasum.judge(metasum.Volume(img))
    return {v: sum(1 for k, _, x in rows if k == "apage" and x == v)
            for v in ("good", "bad", "unsealed")}


def fsck_out(img):
    p = subprocess.run([FSCK, "-n", "-f", img], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, universal_newlines=True)
    return p.returncode, p.stdout


def patch(img, off, fn):
    with open(img, "r+b") as f:
        f.seek(off)
        b = f.read(1)[0]
        f.seek(off)
        f.write(bytes([fn(b)]))


def main():
    tmp = tempfile.mkdtemp(prefix="metasum-test.")
    try:
        img = os.path.join(tmp, "a.img")
        subprocess.run(["bash", "-c", '. "$1/lib.sh"; . "$1/fixtures.sh"; '
                        'make_image "$2" 64M >/dev/null', "x", TESTS, img], check=True)

        b = os.urandom(12288)
        ok(all(metasum.xor(b[:n]) == functools.reduce(lambda a, c: a ^ c, b[:n], 0)
               for n in (1, 7, 8, 488, 512, 12288)),
           "xor fold equals the byte-wise xor (what __byte_sum computes)")

        vol = metasum.Volume(img)
        aps = vol.apages()
        ok(len(aps) >= 1, "apages found through the active apage index")
        c = counts(img)
        ok(c == {"good": len(aps), "bad": 0, "unsealed": 0},
           "a fresh volume: every apage sealed by mkfs and matching", str(c))
        ok(vol.dnodes() == [], "a fresh volume has no dnodes")

        _, sec, _, which = aps[0]
        half_off = sec * 512 + which * (vol.page // 2)

        bad = os.path.join(tmp, "bad.img")
        shutil.copyfile(img, bad)
        patch(bad, half_off + 100, lambda x: x ^ 0x10)
        rc, out = fsck_out(bad)
        ok(counts(bad)["bad"] == 1 and "BAD CHECKSUM ON APAGE" in out,
           "a damaged sealed apage is 'bad', and spadfsck agrees", out[-300:])

        uns = os.path.join(tmp, "unsealed.img")
        shutil.copyfile(img, uns)
        patch(uns, half_off + 8, lambda x: x & ~metasum.APAGE_CHECKSUM_VALID)
        patch(uns, half_off + 100, lambda x: x ^ 0x10)
        rc, out = fsck_out(uns)
        ok(counts(uns)["unsealed"] == 1 and "BAD CHECKSUM ON APAGE" in out,
           "an apage with its flag cleared is still checked: the damage is reported",
           out[-300:])

        info = sbinfo.read_super(img)
        blk = 1 << info["sectors_per_block_bits"]

        # The root directory's first fnode block (XOR fold). Clearing the
        # flag alone would change the fold, so put the fold right afterwards:
        # what is left is a block whose only fault is the cleared bit -- it
        # must still be checked, and so the payload damage must be caught.
        fb = os.path.join(tmp, "fblk.img")
        shutil.copyfile(img, fb)
        off = (info["root"] + blk) * 512
        rc, out = fsck_out(fb)
        ok(rc == 0, "baseline: the copied volume is clean", out[-300:])
        patch(fb, off + 4, lambda x: x & ~0x01)       # FNODE_BLOCK_CHECKSUM_VALID
        patch(fb, off + 5, lambda x: x ^ 0x01)        # the fold, rebalanced
        patch(fb, off + 40, lambda x: x ^ 0x20)       # payload damage
        rc, out = fsck_out(fb)
        ok("BAD CHECKSUM ON FNODE BLOCK" in out,
           "an fnode block with its flag cleared is still checked", out[-300:])

        # The root fixed fnode block (CRC trailer): clear the flag and reseal,
        # then damage the payload. A flag-gated check would skip it.
        fx = os.path.join(tmp, "fixed.img")
        shutil.copyfile(img, fx)
        off = info["root"] * 512
        with open(fx, "r+b") as f:
            f.seek(off)
            u = bytearray(f.read(512))
            u[4] &= ~0x01                             # FIXED_FNODE_BLOCK_CHECKSUM_VALID
            restripe_trailer(u)
            u[100] ^= 0x20
            f.seek(off)
            f.write(u)
        rc, out = fsck_out(fx)
        ok("BAD CHECKSUM ON FIXED FNODE BLOCK" in out,
           "a fixed fnode block with its flag cleared is still checked", out[-300:])

        # The anode scanner: plant a sealed anode in a free block, then damage
        # it. mount_smoke.sh relies on this to show the kernel's anodes exist
        # and verify, so both verdicts must be right.
        an = os.path.join(tmp, "anode.img")
        shutil.copyfile(img, an)
        sec = (os.path.getsize(an) // 512 // 2) & ~(blk - 1)
        u = bytearray(512)
        u[0:4] = (metasum.ANODE_MAGIC ^ sec).to_bytes(4, "little")  # dynamic magic
        u[4] = metasum.ANODE_CHECKSUM_VALID
        u[6] = 1                                     # valid_extents
        restripe_trailer(u)
        with open(an, "r+b") as f:
            f.seek(sec * 512)
            f.write(u)
        rows = [r for r in metasum.judge(metasum.Volume(an)) if r[0] == "anode"]
        ok(rows == [("anode", sec, "good")], "a sealed anode is found and judged good", str(rows))
        patch(an, sec * 512 + 30, lambda x: x ^ 0x08)
        rows = [r for r in metasum.judge(metasum.Volume(an)) if r[0] == "anode"]
        ok(rows == [("anode", sec, "bad")], "a damaged anode is judged bad", str(rows))

        # The removed NO_CHECKSUMS bit, set and resealed as a writer would.
        nc = os.path.join(tmp, "nocsum.img")
        shutil.copyfile(img, nc)
        off = sbinfo.SUPERBLOCK_SECTOR * 512
        with open(nc, "r+b") as f:
            f.seek(off)
            u = bytearray(f.read(512))
            u[96] |= 0x01                             # flags_compat_fsck
            restripe_trailer(u)
            f.seek(off)
            f.write(u)
        rc, out = fsck_out(nc)
        ok(rc != 0 and "INCOMPATIBLE FEATURE FLAGS" in out,
           "a volume with flags_compat_fsck set is refused", "rc=%d %s" % (rc, out[-300:]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed, 0 skipped" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
