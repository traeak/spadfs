#!/usr/bin/env python3
"""Unprivileged tests for tests/logwrites.py (the dm-log-writes replayer).

The synthetic logs here are assembled byte by byte from the kernel's own
struct layout (drivers/md/dm-log-writes.c), deliberately NOT with any helper
from logwrites.py, so a parser bug cannot be hidden by a writer with the same
bug.

The model tests at the end are the reason the crash test exists: a log shaped
like the pre-fix commit (metadata and cct in one flush interval) must produce a
crash state with the cct but without the metadata, and must fail the ordering
check; a log shaped like the fixed commit must do neither.

The fsck-oracle tests run the real spadfsck2 on images from tests/fixtures.sh.

Run: python3 tests/logwrites_test.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

TESTS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)
import logwrites as lw  # noqa: E402

PASS = FAIL = 0


def ok(cond, name, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  PASS  " + name)
    else:
        FAIL += 1
        print("  FAIL  " + name + (("\n        " + detail) if detail else ""))


# --- independent log writer (kernel layout) --------------------------------

MAGIC = 0x6a736677736872
FLUSH, FUA, DISCARD, MARK = 1, 2, 4, 8


class LogBuilder:
    def __init__(self, ss=512, super_entries=None):
        self.ss = ss
        self.blocks = []        # list of bytes, each ss long
        self.n = 0
        self.super_entries = super_entries

    def _hdr(self, sector, nr, flags, dlen, inline=b""):
        b = struct.pack("<QQQQ", sector, nr, flags, dlen) + inline
        return b + bytes(self.ss - len(b))

    def write(self, sector, data, flags=0):
        assert len(data) % self.ss == 0
        self.blocks.append(self._hdr(sector, len(data) // self.ss, flags, 0))
        self.blocks += [data[i:i + self.ss] for i in range(0, len(data), self.ss)]
        self.n += 1
        return self

    def flush(self):
        self.blocks.append(self._hdr(0, 0, FLUSH, 0))
        self.n += 1
        return self

    def discard(self, sector, nr):
        self.blocks.append(self._hdr(sector, nr, DISCARD, 0))
        self.n += 1
        return self

    def mark(self, text):
        t = text.encode()
        self.blocks.append(self._hdr(0, 0, MARK, len(t), t))
        self.n += 1
        return self

    def bytes(self, trailing=b""):
        cnt = self.n if self.super_entries is None else self.super_entries
        sup = struct.pack("<QQQI", MAGIC, 1, cnt, self.ss)
        # the kernel copies a 32-byte struct whose 4 padding bytes are
        # uninitialised: put garbage there to prove the parser ignores it
        sup += b"\xde\xad\xbe\xef"
        sup += bytes(self.ss - len(sup))
        return sup + b"".join(self.blocks) + trailing

    def save(self, path, trailing=b""):
        with open(path, "wb") as f:
            f.write(self.bytes(trailing))
        return path


def blk(ss, fill, n=1):
    return bytes([fill]) * (ss * n)


def main():
    tmp = tempfile.mkdtemp(prefix="lw-test.")
    try:
        run_all(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed, 0 skipped" % (PASS, FAIL))
    return 1 if FAIL else 0


def run_all(tmp):
    print("== parsing")
    b = LogBuilder(512)
    b.write(10, blk(512, 0x11, 2)).mark("ack.0").discard(40, 3) \
     .write(20, blk(512, 0x22)).flush().mark(lw.END_MARK)
    log = lw.parse_log(b.save(os.path.join(tmp, "a.log")))
    ok(log.sectorsize == 512, "sectorsize read from the super block")
    ok(len(log.entries) == 6, "six entries walked", "got %d" % len(log.entries))
    ok(log.stop == "end of file", "walk ends cleanly", log.stop)
    e0 = log.entries[0]
    ok(e0.sector == 10 and e0.nr_sectors == 2 and e0.data_off == 1024,
       "write header and data offset (data follows its header block)",
       "sector %d nr %d off %d" % (e0.sector, e0.nr_sectors, e0.data_off))
    ok(log.read(e0.data_off, 1024) == blk(512, 0x11, 2), "write data read back")
    ok(log.entries[1].mark == "ack.0", "mark text inline, exactly data_len bytes")
    ok(log.entries[2].flags & lw.F_DISCARD and log.entries[3].data_off == 512 * 7,
       "discard carries no data blocks (next header directly after it)",
       "entry 3 data at %d" % log.entries[3].data_off)
    ok(log.entries[-1].mark == lw.END_MARK, "end mark found")

    b4 = LogBuilder(4096)
    b4.write(3, blk(4096, 0x33)).flush()
    log4 = lw.parse_log(b4.save(os.path.join(tmp, "b.log")))
    w = lw.build_epochs(log4)[0].writes[0]
    ok(w.off == 3 * 4096 and w.length == 4096,
       "4K sectorsize: sector and nr_sectors count 4K units, not 512",
       "off %d len %d" % (w.off, w.length))

    bs = LogBuilder(512, super_entries=1)
    bs.write(1, blk(512, 1)).write(2, blk(512, 2)).flush().write(3, blk(512, 3))
    logs = lw.parse_log(bs.save(os.path.join(tmp, "c.log")))
    ok(len(logs.entries) == 4 and logs.super_entries == 1,
       "stale super nr_entries is not treated as the end of the log")

    bz = LogBuilder(512)
    bz.write(1, blk(512, 1)).flush()
    garbage = bytes(512) + struct.pack("<QQQQ", 7, 1, 0, 0) + bytes(480) + blk(512, 9)
    logz = lw.parse_log(bz.save(os.path.join(tmp, "d.log"), garbage))
    ok(len(logz.entries) == 2 and logz.stop == "zero header",
       "walk stops at a zero header and ignores what follows")

    bt = LogBuilder(512)
    bt.write(1, blk(512, 1, 4))
    raw = bt.bytes()[:-512]
    with open(os.path.join(tmp, "e.log"), "wb") as f:
        f.write(raw)
    logt = lw.parse_log(os.path.join(tmp, "e.log"))
    ok(len(logt.entries) == 0 and logt.stop.startswith("truncated"),
       "a truncated entry is not replayed", logt.stop)

    with open(os.path.join(tmp, "f.log"), "wb") as f:
        f.write(bytes(4096))
    try:
        lw.parse_log(os.path.join(tmp, "f.log"))
        ok(False, "an all-zero log is rejected")
    except lw.LogError:
        ok(True, "an all-zero log is rejected (no FUA/mark ever written)")

    print("== flush intervals and replay")
    b = LogBuilder(512)
    b.write(0, blk(512, 0xA1)).write(1, blk(512, 0xA2)).flush() \
     .write(0, blk(512, 0xB1)).write(0, blk(512, 0xB2)).discard(1, 1).flush() \
     .write(2, blk(512, 0xC1))
    log = lw.parse_log(b.save(os.path.join(tmp, "g.log")))
    eps = lw.build_epochs(log)
    ok([len(e.writes) for e in eps] == [2, 3, 1],
       "flushes split the writes into intervals, unflushed tail kept",
       str([len(e.writes) for e in eps]))
    ok(eps[0].end == 2 and eps[1].end == 6 and eps[2].end == len(log.entries),
       "interval end is the closing flush's log index")
    img = os.path.join(tmp, "g.img")
    with open(img, "wb") as f:
        f.write(blk(512, 0xEE, 4))
    lw.apply_writes(log, img, eps[0].writes + eps[1].writes)
    with open(img, "rb") as f:
        got = f.read()
    ok(got[:512] == blk(512, 0xB2) and got[512:1024] == bytes(512)
       and got[1024:] == blk(512, 0xEE, 2),
       "replay applies in log order (later write wins) and discards read as zeros")

    bf = LogBuilder(512)
    bf.write(5, blk(512, 1)).flush()
    bf.blocks.append(bf._hdr(6, 1, FLUSH, 0))   # REQ_PREFLUSH write with data
    bf.blocks.append(blk(512, 2))
    bf.n += 1
    bf.write(7, blk(512, 3), flags=FUA).write(8, blk(512, 4))
    logf = lw.parse_log(bf.save(os.path.join(tmp, "h.log")))
    epf = lw.build_epochs(logf)
    ok([w.off // 512 for w in epf[2].writes] == [6, 7, 8],
       "a PREFLUSH write's own data opens the next interval",
       str([[w.off // 512 for w in e.writes] for e in epf]))
    st = lw.enumerate_states(epf, [], prefix_cap=16, n_random=8, seed=3)
    fua_pos = 1
    ok(all(fua_pos in s.include for s in st if s.epoch == 2),
       "FUA writes are in every crash state of their interval")

    print("== acknowledgements")
    b = LogBuilder(512)
    b.write(1, blk(512, 1)).flush().mark("ack.0").write(2, blk(512, 2)).flush() \
     .write(3, blk(512, 3)).mark("ack.1").flush().mark("other")
    log = lw.parse_log(b.save(os.path.join(tmp, "i.log")))
    eps = lw.build_epochs(log)
    acks = lw.ack_marks(log)
    ok(acks == [(2, 0), (6, 1)], "ack.N marks found, other marks ignored", str(acks))
    ok([lw.acked_before(acks, e.end) for e in eps] == [-1, 0, 1, 1],
       "acknowledged count per interval: marks logged before its closing flush",
       str([lw.acked_before(acks, e.end) for e in eps]))

    print("== the ordering bug, modelled")
    ss = 512
    CCT = 1000          # sector of the cct in this toy device
    commit = [(CCT * ss, 512 * ss)]
    companion = [(1600 * ss, 8 * ss)]

    pre = LogBuilder(ss)      # v1.0.22 / pre-ef221c6: no flush before the cct
    pre.write(100, blk(ss, 0xA)).write(200, blk(ss, 0xB)) \
       .write(CCT, blk(ss, 0xC)).write(1600, blk(ss, 0xD)).flush()
    logp = lw.parse_log(pre.save(os.path.join(tmp, "pre.log")))
    epp = lw.build_epochs(logp)
    stp = lw.enumerate_states(epp, commit, prefix_cap=16, n_random=0)
    torn = [s for s in stp if 2 in s.include and 0 not in s.include]
    ok(bool(torn), "pre-fix log: a crash state has the cct but not the metadata",
       "states: %s" % [(s.epoch, s.include) for s in stp])
    ok(any("commit-drop-one" in s.kinds for s in torn),
       "... and the targeted commit-drop-one enumeration produces it")
    pre_prefixes = [s for s in stp if s.kinds == ["prefix"] or s.kinds == ["flush"]]
    ok(not any(2 in s.include and 0 not in s.include for s in pre_prefixes),
       "... which no in-order prefix would have (why replay-to-flush is not enough)")
    vp = lw.ordering_violations(epp, commit, companion)
    ok(len(vp) == 1 and [w.off // ss for w in vp[0][1]] == [100, 200],
       "pre-fix log fails the ordering check, naming the co-flushed writes",
       str([(e.number, [w.off // ss for w in x]) for e, x in vp]))

    post = LogBuilder(ss)     # ef221c6: metadata, flush, cct + crc, flush
    post.write(100, blk(ss, 0xA)).write(200, blk(ss, 0xB)).flush() \
        .write(CCT, blk(ss, 0xC)).write(1600, blk(ss, 0xD)).flush()
    logq = lw.parse_log(post.save(os.path.join(tmp, "post.log")))
    epq = lw.build_epochs(logq)
    stq = lw.enumerate_states(epq, commit, prefix_cap=16, n_random=8)

    def cct_without_meta(s, eps=epq):
        # A state is every write of the earlier intervals plus s.include of
        # its own. The cct is present only in interval 1, which always sits
        # on top of all of interval 0 -- so check that directly.
        has_cct = any(eps[s.epoch].writes[i].overlaps(commit) for i in s.include)
        meta_offs = {100 * ss, 200 * ss}
        present = {w.off for e in eps[:s.epoch] for w in e.writes} | \
                  {eps[s.epoch].writes[i].off for i in s.include}
        return has_cct and not meta_offs <= present
    ok(len(stq) > 3 and not any(cct_without_meta(s) for s in stq),
       "post-fix log: no crash state has the cct without the metadata",
       str([(s.epoch, s.include) for s in stq if cct_without_meta(s)]))
    ok(any(cct_without_meta(s, epp) for s in stp) and
       sum(cct_without_meta(s, epp) for s in stp) == len(
           [s for s in stp if 2 in s.include and not {0, 1} <= set(s.include)]),
       "the same predicate does flag the pre-fix log (it is not vacuous)")
    ok(lw.ordering_violations(epq, commit, companion) == [],
       "post-fix log passes the ordering check (cct + its CRC region alone)")

    print("== tree comparison")
    m = {0: {"a": ["f", 1, "x", 1]},
         1: {"a": ["f", 1, "x", 1], "b": ["f", 2, "y", 1]}}
    ok(lw.compare_tree({"a": ["f", 1, "x", 1]}, [], m, 0) == [],
       "acknowledged tree matches")
    ok(lw.compare_tree({"a": ["f", 1, "x", 1], "b": ["f", 0, "z", 1]}, [], m, 0) == [],
       "a path the next operation touches may be half-done")
    ok(lw.compare_tree({}, [], m, 0) != [], "an acknowledged file that vanished fails")
    ok(lw.compare_tree({"a": ["f", 1, "x", 1]}, ["a: Input/output error"], m, 0) != [],
       "a kernel read error fails even on an otherwise matching tree")
    ok(lw.compare_tree({"a": ["f", 1, "x", 1]}, [], m, 1) != [],
       "after ack.1 the second file must exist")

    print("== fsck oracle (real spadfsck2)")
    fsck = os.path.join(REPO, "spadfsck2")
    if not os.access(fsck, os.X_OK):
        ok(False, "spadfsck2 is built", "run make")
        return
    clean = os.path.join(tmp, "clean.img")
    r = subprocess.run(["bash", "-c", 'set -e; . "$1/lib.sh"; . "$1/fixtures.sh"; '
                        'make_image "$2" 32M >/dev/null; '
                        'python3 "$1/prime.py" "$2" --cc 1 --base-txc 100 >/dev/null; '
                        # prime.py moves the cct but not its mirror; refresh it
                        '"$3" -y -f "$2" >/dev/null 2>&1 || true',
                        "x", TESTS, clean, os.path.join(REPO, "spadfsck2")])
    ok(r.returncode == 0, "fixture image created and primed")
    rep = os.path.join(tmp, "rep.img")
    cls, notes = lw.judge_fsck(fsck, clean, rep)
    ok(cls == "clean", "a clean volume is judged clean", str(notes))

    torn = os.path.join(tmp, "torn.img")
    shutil.copyfile(clean, torn)
    cct = lw.sbinfo.read_super(torn)["cct"]
    with open(torn, "r+b") as f:       # cct[1] advanced, its CRC not: a cut
        f.seek(cct * 512 + 4)           # between the cct and CRC writes
        v = struct.unpack("<I", f.read(4))[0]
        f.seek(cct * 512 + 4)
        f.write(struct.pack("<I", v + 1))
    cls, notes = lw.judge_fsck(fsck, torn, rep)
    ok(cls == "recovered",
       "a torn cct/CRC pair is judged recovered via the designed backup path",
       "%s %s" % (cls, notes))

    broken = os.path.join(tmp, "broken.img")
    shutil.copyfile(clean, broken)
    root = lw.sbinfo.read_super(broken)["root"]
    with open(broken, "r+b") as f:     # garbage in the root fixed fnode block
        f.seek(root * 512 + 64)
        f.write(b"\x5a" * 64)
    cls, notes = lw.judge_fsck(fsck, broken, rep)
    ok(cls == "broken", "real damage is judged broken", "%s %s" % (cls, notes))

    print("== check pipeline end to end (log built from real images)")
    # A log that turns `clean` back into a fresh volume's successor is not
    # available without a kernel, so build one from two real images: a fresh
    # volume (base) and the primed one (target). Their differing sectors are
    # logged the way the fixed commit orders them -- everything else, flush,
    # then cct + cct CRC, flush -- and `check --no-mount` must pass every
    # state. This drives parse, ordering, enumeration, replay and the fsck
    # oracle together; a torn mirror (stamp vs data) is among the states.
    base = os.path.join(tmp, "base.img")
    r = subprocess.run(["bash", "-c", '. "$1/lib.sh"; . "$1/fixtures.sh"; '
                        'make_image "$2" 32M >/dev/null', "x", TESTS, base])
    A = open(base, "rb").read()
    B = open(clean, "rb").read()
    sb = lw.sbinfo.read_super(base)
    diff = [s for s in range(len(A) // 512) if A[s * 512:(s + 1) * 512] != B[s * 512:(s + 1) * 512]]
    crc_n = lw.spadfs_regions(base)[1][0][1] // 512
    in_commit = [s for s in diff if sb["cct"] <= s < sb["cct"] + lw.CCT_SECTORS
                 or sb["cct_crc"] <= s < sb["cct_crc"] + crc_n]
    lb = LogBuilder(512)
    lb.mark("ack.0")
    for s in diff:
        if s not in in_commit:
            lb.write(s, B[s * 512:(s + 1) * 512])
    lb.flush()
    for s in in_commit:
        lb.write(s, B[s * 512:(s + 1) * 512])
    lb.flush().mark("ack.1").mark(lw.END_MARK)
    lpath = lb.save(os.path.join(tmp, "real.log"))
    ok(len(in_commit) >= 2 and len(diff) > len(in_commit),
       "the image pair differs in the cct, its CRC and elsewhere", str(diff))
    out = os.path.join(tmp, "replayed.img")
    rr = subprocess.run([sys.executable, os.path.join(TESTS, "logwrites.py"), "replay",
                         "--log", lpath, "--base", base, "--out", out])
    ok(rr.returncode == 0 and open(out, "rb").read() == B,
       "replaying the whole log reproduces the target image exactly")
    mdir = os.path.join(tmp, "man")
    os.makedirs(mdir)
    for n in (0, 1):
        with open(os.path.join(mdir, "manifest.%d.json" % n), "w") as f:
            f.write("{}")
    work = os.path.join(tmp, "work")
    os.makedirs(work)
    rc = subprocess.run([sys.executable, os.path.join(TESTS, "logwrites.py"), "check",
                         "--log", lpath, "--base", base, "--fsck", fsck,
                         "--manifests", mdir, "--require-end", "--no-mount",
                         "--results", os.path.join(tmp, "res"), "--workdir", work],
                        stdout=subprocess.PIPE, universal_newlines=True)
    ok(rc.returncode == 0 and "PASS ordering" in rc.stdout and " 0 FAILED" in rc.stdout,
       "check passes the ordering and every crash state", rc.stdout)

    # the same images, logged the pre-fix way: no flush before the cct
    lb2 = LogBuilder(512)
    lb2.mark("ack.0")
    for s in diff:
        lb2.write(s, B[s * 512:(s + 1) * 512])
    lb2.flush().mark("ack.1").mark(lw.END_MARK)
    lpath2 = lb2.save(os.path.join(tmp, "real-prefix.log"))
    work2 = os.path.join(tmp, "work2")
    os.makedirs(work2)
    rc2 = subprocess.run([sys.executable, os.path.join(TESTS, "logwrites.py"), "check",
                          "--log", lpath2, "--base", base, "--fsck", fsck,
                          "--manifests", mdir, "--require-end", "--no-mount",
                          "--results", os.path.join(tmp, "res2"), "--workdir", work2],
                         stdout=subprocess.PIPE, universal_newlines=True)
    ok(rc2.returncode == 1 and "FAIL ordering" in rc2.stdout,
       "the same writes without the pre-cct flush fail the ordering check", rc2.stdout)

    ok(lw.is_finding("CCT SECTOR 0 DOES NOT MATCH ITS CHECKSUM")
       and not lw.is_finding("checking crash count table...")
       and not lw.is_finding("100%  (154 of 154)")
       and not lw.is_finding("crash count table remains untrusted; skipping further checks"),
       "findings (upper case) are told apart from progress and status lines")


if __name__ == "__main__":
    sys.exit(main())
