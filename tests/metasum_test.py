#!/usr/bin/env python3
"""Unprivileged checks for tests/metasum.py, the apage/dnode checksum scanner
that tests/meta_csum_mount.sh relies on to show the kernel seals what it
writes. Each verdict is cross-checked against spadfsck where it can be.

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
        ok(counts(uns)["unsealed"] == 1 and "BAD CHECKSUM ON APAGE" not in out,
           "with the flag clear the same damage is 'unsealed': nothing checks it",
           out[-300:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed, 0 skipped" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
