#!/usr/bin/env python3
"""Crash-state replay of dm-log-writes logs, for the spadfs2 crash test.

dm-log-writes (drivers/md/dm-log-writes.c) records every write that reaches a
device, with each cache flush as an ordering point. This module reads that log
and rebuilds the device contents a power cut could have left, then judges each
one with spadfsck and (as root) a read-only mount of the kernel module.

WHY NOT xfstests' `replay-log`, which replays up to each flush point. A prefix
that ends at a flush cannot show the bug this test exists for. Before the
commit-flush fix (ef221c6), a commit's metadata and its cct write sat in the
SAME flush interval, so every flush-point prefix holds either all of them or
none. The dangerous state -- cct durable, some metadata not -- is a REORDERING
inside the interval, which a volatile write cache is free to produce. So a
crash state here is

    every write before flush k
  + any subset of the writes between flush k and flush k+1

(FUA writes are always in the subset: they were durable when they completed).
The enumeration deliberately includes the subsets that keep the commit record
and drop one other write -- the exact shape of the ordering bug.

Log format, checked against dm-log-writes.c and xfstests src/log-writes/:

  block 0       struct log_write_super { le64 magic, version, nr_entries;
                                         le32 sectorsize; }
  per entry     one sectorsize block holding struct log_write_entry
                { le64 sector, nr_sectors, flags, data_len; }, then
                nr_sectors blocks of data -- none for DISCARD or MARK.
                A MARK's text is inline at byte 32, exactly data_len bytes.

  sector and nr_sectors count sectorsize units (the logical block size of the
  logged device), not 512-byte sectors. nr_entries is only rewritten on FUA
  and MARK entries, so it is NOT the end of the log; the walk stops at a zero
  or invalid header instead.

  Writes are logged on completion. A write that completed before a flush was
  issued is logged before that flush's entry, so "everything before the flush
  entry" is exactly what the flush made durable.

Subcommands:
  parse LOG                     summarise a log
  replay --log --base --out     apply the whole log (fidelity check)
  manifest DIR OUT.json         record a directory tree for later comparison
  check --log ... --base ...    replay every crash state and judge it

Unprivileged except for `check` without --no-mount (losetup + mount).
"""

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import stat
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sbinfo  # noqa: E402  (tests/sbinfo.py: superblock reader)

LOG_MAGIC = 0x6a736677736872
LOG_VERSION = 1
F_FLUSH, F_FUA, F_DISCARD, F_MARK, F_METADATA = 1, 2, 4, 8, 16
F_KNOWN = F_FLUSH | F_FUA | F_DISCARD | F_MARK | F_METADATA
END_MARK = "dm-log-writes-end"
ENTRY_SIZE = 32          # sizeof(struct log_write_entry) in the kernel
SUPER_FMT = "<QQQI"      # 28 bytes; the struct's 4 tail bytes are padding

# struct.h: CCT_SIZE >> SSIZE_BITS and CCT_CRC_UNITS
CCT_SECTORS = 512
CCT_CRC_UNITS = 5


class LogError(Exception):
    pass


# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------

class Entry:
    __slots__ = ("idx", "sector", "nr_sectors", "flags", "data_off", "mark")

    def __init__(self, idx, sector, nr_sectors, flags, data_off, mark):
        self.idx = idx
        self.sector = sector
        self.nr_sectors = nr_sectors
        self.flags = flags
        self.data_off = data_off
        self.mark = mark


class Log:
    def __init__(self, path, sectorsize, super_entries, entries, stop):
        self.path = path
        self.sectorsize = sectorsize
        self.super_entries = super_entries
        self.entries = entries
        self.stop = stop
        self._f = None

    def read(self, off, length):
        if self._f is None:
            self._f = open(self.path, "rb")
        self._f.seek(off)
        data = self._f.read(length)
        if len(data) != length:
            raise LogError("short read of entry data at %d" % off)
        return data

    def marks(self):
        return [(e.idx, e.mark) for e in self.entries if e.mark is not None]


def parse_log(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        sup = f.read(struct.calcsize(SUPER_FMT))
        if len(sup) < struct.calcsize(SUPER_FMT):
            raise LogError("log is shorter than its super block")
        magic, version, super_entries, ss = struct.unpack(SUPER_FMT, sup)
        if magic != LOG_MAGIC:
            raise LogError("bad log magic %#x: not a dm-log-writes log, or "
                           "no FUA/MARK entry was ever logged" % magic)
        if version != LOG_VERSION:
            raise LogError("unsupported log version %d" % version)
        if ss < 512 or ss & (ss - 1):
            raise LogError("bad log sectorsize %d" % ss)

        entries = []
        off = ss                # the first header follows the super block
        stop = None
        while True:
            if off + ss > size:
                stop = "end of file"
                break
            f.seek(off)
            hdr = f.read(ss)
            sector, nr, flags, dlen = struct.unpack_from("<QQQQ", hdr)
            n = len(entries)
            if not (sector or nr or flags or dlen):
                stop = "zero header"
                break
            if flags & ~F_KNOWN:
                stop = "unknown flags %#x at entry %d" % (flags, n)
                break
            mark = None
            if flags == F_MARK:
                # log_mark(): exactly MARK, text inline after the struct
                if dlen > ss - ENTRY_SIZE:
                    stop = "mark too long at entry %d" % n
                    break
                mark = hdr[ENTRY_SIZE:ENTRY_SIZE + dlen].decode("utf-8", "replace")
                data_len = 0
            else:
                # xfstests log_entry_valid(): non-marks carry no inline data
                if flags & F_MARK or dlen or hdr[ENTRY_SIZE]:
                    stop = "invalid header at entry %d" % n
                    break
                data_len = 0 if flags & F_DISCARD else nr * ss
            data_off = off + ss
            if data_off + data_len > size:
                stop = "truncated data at entry %d" % n
                break
            entries.append(Entry(n, sector, nr, flags, data_off, mark))
            off = data_off + data_len
    return Log(path, ss, super_entries, entries, stop)


# ---------------------------------------------------------------------------
# Flush intervals ("epochs") and crash states
# ---------------------------------------------------------------------------

class Write:
    __slots__ = ("idx", "off", "length", "data_off", "fua", "discard")

    def __init__(self, entry, ss):
        self.idx = entry.idx
        self.off = entry.sector * ss
        self.length = entry.nr_sectors * ss
        self.data_off = entry.data_off
        self.fua = bool(entry.flags & F_FUA)
        self.discard = bool(entry.flags & F_DISCARD)

    def overlaps(self, ranges):
        return any(self.off < s + n and s < self.off + self.length
                   for s, n in ranges)


class Epoch:
    """The writes between two flushes. `end` is the log index of the
    closing flush entry, or len(entries) for the unflushed tail."""

    def __init__(self, number, start):
        self.number = number
        self.start = start
        self.end = None
        self.writes = []

    def fua_positions(self):
        return {i for i, w in enumerate(self.writes) if w.fua}


def build_epochs(log):
    epochs = [Epoch(0, 0)]
    for e in log.entries:
        if e.mark is not None:
            continue
        if e.flags & F_FLUSH:
            # Everything logged so far is durable once this flush completes.
            # A REQ_PREFLUSH write's own data is NOT made durable by its
            # flush, so it opens the next interval.
            epochs[-1].end = e.idx
            epochs.append(Epoch(len(epochs), e.idx))
        if e.nr_sectors:
            epochs[-1].writes.append(Write(e, log.sectorsize))
    epochs[-1].end = len(log.entries)
    return epochs


class State:
    __slots__ = ("epoch", "include", "kinds", "acked")

    def __init__(self, epoch, include, kind):
        self.epoch = epoch
        self.include = include
        self.kinds = [kind]
        self.acked = -1

    def name(self):
        return "e%d:%s" % (self.epoch, "+".join(self.kinds))


def enumerate_states(epochs, commit_ranges, prefix_cap=16, n_random=6, seed=1):
    """Crash states, deduplicated by (epoch, included writes)."""
    states = {}

    def add(k, include, kind):
        inc = tuple(sorted(set(include) | epochs[k].fua_positions()))
        key = (k, inc)
        if key in states:
            if kind not in states[key].kinds:
                states[key].kinds.append(kind)
        else:
            states[key] = State(k, inc, kind)

    for k, ep in enumerate(epochs):
        w = len(ep.writes)
        add(k, range(w), "flush")
        if not w:
            continue
        # in-order tears: the first j writes of the interval landed
        js = list(range(w))
        if len(js) > prefix_cap:
            js = sorted({round(i * (w - 1) / (prefix_cap - 1))
                         for i in range(prefix_cap)})
        for j in js:
            add(k, range(j), "prefix")
        # the ordering bug's shape: commit record durable, something else not
        commit = [i for i, wr in enumerate(ep.writes) if wr.overlaps(commit_ranges)]
        if commit:
            add(k, commit, "commit-only")
            for i in range(w):
                if i not in commit:
                    add(k, [x for x in range(w) if x != i], "commit-drop-one")
        rnd = random.Random(seed * 1000003 + k)
        for _ in range(n_random if w > 1 else 0):
            add(k, [i for i in range(w) if rnd.random() < 0.5], "random")

    return sorted(states.values(), key=lambda s: (s.epoch, len(s.include), s.include))


def ack_marks(log):
    """[(log index, N)] for marks named ack.N, in log order."""
    out = []
    for idx, text in log.marks():
        m = re.fullmatch(r"ack\.(\d+)", text)
        if m:
            out.append((idx, int(m.group(1))))
    return out


def acked_before(acks, end):
    """The highest N whose ack.N mark was logged before log index `end`.

    A mark is logged when it is sent, which the workload does only after
    syncfs returned -- after that commit's flushes completed and were logged.
    So an ack.N logged before the interval's closing flush was acknowledged
    before any crash that interval can represent."""
    n = -1
    for idx, num in acks:
        if idx < end:
            n = max(n, num)
    return n


def ordering_violations(epochs, commit_ranges, companion_ranges):
    """Commit intervals that also carry writes other than the commit record.

    The protocol: flush the metadata, then write the cct (and its CRC), then
    flush. The interval holding a primary cct write must therefore hold
    nothing else but the cct CRC region. Anything more means a cache could
    make the commit durable without it."""
    out = []
    allowed = list(commit_ranges) + list(companion_ranges)
    for ep in epochs:
        if not any(w.overlaps(commit_ranges) for w in ep.writes):
            continue
        extra = [w for w in ep.writes if not w.overlaps(allowed)]
        if extra:
            out.append((ep, extra))
    return out


def apply_writes(log, path, writes):
    size = os.path.getsize(path)
    with open(path, "r+b") as f:
        for w in writes:
            if w.off + w.length > size:
                raise LogError("write at %d+%d is past the end of the %d-byte "
                               "image: wrong base image for this log"
                               % (w.off, w.length, size))
            f.seek(w.off)
            if w.discard:
                f.write(bytes(w.length))
            else:
                f.write(log.read(w.data_off, w.length))


def spadfs_regions(base):
    """(commit ranges, companion ranges) in bytes, from the base superblock:
    the primary cct, and the cct CRC region written with it."""
    sb = sbinfo.read_super(base)
    bs = 1 << sb["sectors_per_block_bits"]
    crc_sectors = (CCT_CRC_UNITS + bs - 1) // bs * bs
    commit = [(sb["cct"] * 512, CCT_SECTORS * 512)]
    companion = [(sb["cct_crc"] * 512, crc_sectors * 512)] if sb["cct_crc"] else []
    return commit, companion


# ---------------------------------------------------------------------------
# Oracle: spadfsck
# ---------------------------------------------------------------------------

# Findings a crash can legitimately leave, all with a designed recovery:
#   - the cct and its CRC are written in one flush interval, so a cut inside
#     it can leave them disagreeing; fsck restores the mirror, which holds the
#     previous commit (the in-progress one was never acknowledged)
#   - the mirror is written after the primary is durable, so it can lag or be
#     torn; fsck refreshes it from the good primary
ALLOWED_FINDINGS = [re.compile(p) for p in (
    r"CCT SECTOR \d+ DOES NOT MATCH ITS CHECKSUM",
    r"CRASH COUNT TABLE CORRUPT: \d+ SECTOR\(S\) FAIL CHECKSUM",
    r"CRASH COUNT TABLE IS DAMAGED BUT ITS BACKUP VERIFIES",
    r"THE BACKUP IS FROM THE CURRENT CRASH EPOCH \(.*\)",
    r"RESTORING IT ROLLS BACK AT MOST THE COMMIT IN PROGRESS",
    r"CRASH COUNT TABLE RESTORED FROM BACKUP",
    r"CCT BACKUP SECTOR \d+ DOES NOT MATCH ITS CHECKSUM",
    r"CCT BACKUP CORRUPT: \d+ SECTOR\(S\) FAIL CHECKSUM",
    r"CCT BACKUP STAMP DOES NOT MATCH ITS DATA \(.*\)",
    # both forms: "(BACKUP CC .. TXC ..)" and "(NO EPOCH STAMP)", the latter
    # when the cut tore the mirror from its stamp. With a good primary fsck
    # only refreshes the mirror. Restoring FROM an unstamped mirror prints
    # "THE BACKUP HAS NO VALID EPOCH STAMP..." and stays disallowed.
    r"CCT BACKUP IS OUT OF DATE \(.*\)",
    r"CCT BACKUP REFRESHED",
    r"CCT BACKUP RECREATED",
)]

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def is_finding(line):
    """spadfsck findings (log_printf) are upper case; progress and status
    lines (status_printf) start lower case or with a percentage."""
    for ch in line:
        if ch.isalpha():
            return ch.isupper()
    return False


def is_allowed(finding):
    return any(p.fullmatch(finding) for p in ALLOWED_FINDINGS)


def fsck_run(fsck, img, mode):
    p = subprocess.run([fsck, mode, "-f", img], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    text = ANSI.sub("", p.stdout.decode("utf-8", "replace")).replace("\r", "\n")
    findings = [ln.strip() for ln in text.split("\n") if is_finding(ln.strip())]
    return p.returncode, findings


def judge_fsck(fsck, img, repaired):
    """('clean' | 'recovered' | 'broken', notes). For 'recovered' the
    repaired image is left at `repaired`."""
    rc, found = fsck_run(fsck, img, "-n")
    if rc == 0 and not found:
        return "clean", []
    notes = ["fsck -n rc=%d: %s" % (rc, f) for f in found] or ["fsck -n rc=%d" % rc]
    if not all(is_allowed(f) for f in found):
        return "broken", notes
    # -n stops after a damaged cct ("skipping further checks"), so the rest
    # of the volume is only examined by the repair pass. Any finding there
    # beyond the allowed set is damage the crash should not have caused.
    shutil.copyfile(img, repaired)
    rc2, found2 = fsck_run(fsck, repaired, "-y")
    notes += ["fsck -y rc=%d: %s" % (rc2, f) for f in found2]
    if not all(is_allowed(f) for f in found2):
        return "broken", notes
    rc3, found3 = fsck_run(fsck, repaired, "-n")
    if rc3 or found3:
        notes += ["after repair, fsck -n rc=%d: %s" % (rc3, f) for f in found3] \
            or ["after repair, fsck -n rc=%d" % rc3]
        return "broken", notes
    return "recovered", notes


# ---------------------------------------------------------------------------
# Oracle: directory tree against the acknowledged manifests
# ---------------------------------------------------------------------------

def walk_tree(root):
    """{relative path: description}, plus read errors. Any error reading a
    crash state through the kernel is a failure in its own right."""
    tree, errors = {}, []

    def onerror(e):
        errors.append("%s: %s" % (os.path.relpath(e.filename or root, root), e.strerror))

    for dirpath, dirnames, filenames in os.walk(root, onerror=onerror):
        for name in sorted(dirnames + filenames):
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root)
            try:
                st = os.lstat(p)
                if stat.S_ISLNK(st.st_mode):
                    tree[rel] = ["l", os.readlink(p)]
                elif stat.S_ISDIR(st.st_mode):
                    tree[rel] = ["d"]
                elif stat.S_ISREG(st.st_mode):
                    h = hashlib.sha256()
                    with open(p, "rb") as f:
                        for chunk in iter(lambda: f.read(1 << 20), b""):
                            h.update(chunk)
                    tree[rel] = ["f", st.st_size, h.hexdigest(), st.st_nlink]
                else:
                    tree[rel] = ["o", stat.S_IFMT(st.st_mode)]
            except OSError as e:
                errors.append("%s: %s" % (rel, e.strerror))
    return tree, errors


def load_manifests(d):
    out = {}
    for name in os.listdir(d):
        m = re.fullmatch(r"manifest\.(\d+)\.json", name)
        if m:
            with open(os.path.join(d, name)) as f:
                out[int(m.group(1))] = json.load(f)
    return out


def compare_tree(tree, errors, manifests, acked):
    """Paths the next, unacknowledged operation touches may be in either
    state (or in between); every other path must be exactly as acknowledged."""
    problems = ["read error: " + e for e in errors]
    if acked >= 0 and acked not in manifests:
        return problems + ["no manifest for ack.%d" % acked]
    exp = manifests.get(acked, {})
    nxt = manifests.get(acked + 1)
    touched = set()
    if nxt is not None:
        touched = {p for p in set(exp) | set(nxt) if exp.get(p) != nxt.get(p)}
    for p in sorted(set(exp) | set(tree)):
        if p not in touched and tree.get(p) != exp.get(p):
            problems.append("%s: acknowledged %s, found %s"
                            % (p, exp.get(p, "absent"), tree.get(p, "absent")))
    return problems


def run(cmd):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True)


def mounted_tree(img, fstype, mnt):
    """Mount `img` read-only through a read-only loop device and walk it.
    Returns (tree, problems); tree is None when it did not mount."""
    r = run(["losetup", "--find", "--show", "--read-only", img])
    if r.returncode:
        return None, ["losetup failed: " + r.stderr.strip()]
    dev = r.stdout.strip()
    try:
        r = run(["mount", "-t", fstype, "-o", "ro", dev, mnt])
        if r.returncode:
            dm = run(["dmesg"]).stdout.strip().split("\n")[-4:]
            return None, ["mount failed: " + r.stderr.strip()] + ["dmesg: " + x for x in dm]
        try:
            tree, errors = walk_tree(mnt)
        finally:
            run(["umount", mnt])
        return tree, errors
    finally:
        run(["losetup", "-d", dev])


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def describe(log, epochs):
    n_flush = sum(1 for e in log.entries if e.flags & F_FLUSH)
    n_fua = sum(1 for e in log.entries if e.flags & F_FUA)
    n_disc = sum(1 for e in log.entries if e.flags & F_DISCARD)
    n_w = sum(len(ep.writes) for ep in epochs)
    print("sectorsize=%d" % log.sectorsize)
    print("entries=%d super_nr_entries=%d stop=%s"
          % (len(log.entries), log.super_entries, log.stop))
    print("writes=%d flushes=%d fua=%d discards=%d marks=%d intervals=%d"
          % (n_w, n_flush, n_fua, n_disc, len(log.marks()), len(epochs)))


def cmd_parse(a):
    log = parse_log(a.log)
    epochs = build_epochs(log)
    describe(log, epochs)
    for idx, text in log.marks():
        print("mark %d %s" % (idx, text))
    return 0


def cmd_manifest(a):
    tree, errors = walk_tree(a.dir)
    if errors:
        sys.stderr.write("manifest: read errors:\n  " + "\n  ".join(errors) + "\n")
        return 1
    with open(a.out, "w") as f:
        json.dump(tree, f, indent=0, sort_keys=True)
    return 0


def cmd_replay(a):
    """Apply the whole log to a copy of the base image. On a complete log
    the result must equal the device as the kernel left it -- the replayer's
    own fidelity check."""
    log = parse_log(a.log)
    shutil.copyfile(a.base, a.out)
    apply_writes(log, a.out, [w for ep in build_epochs(log) for w in ep.writes])
    return 0


def cmd_check(a):
    log = parse_log(a.log)
    epochs = build_epochs(log)
    describe(log, epochs)
    rc = 0

    if a.require_end:
        # A complete log ends with the target's own end mark, and its super
        # count was rewritten by that mark. Anything else means the replay
        # would silently test a truncated history.
        last = log.entries[-1] if log.entries else None
        if last is None or last.mark != END_MARK:
            print("FAIL log does not end with %r (stop: %s)" % (END_MARK, log.stop))
            return 1
        if log.super_entries != len(log.entries):
            print("FAIL super block counts %d entries, walk found %d"
                  % (log.super_entries, len(log.entries)))
            return 1

    commit, companion = spadfs_regions(a.base)
    commit_epochs = [ep for ep in epochs if any(w.overlaps(commit) for w in ep.writes)]
    acks = ack_marks(log)
    print("commit intervals=%d ack marks=%d" % (len(commit_epochs), len(acks)))
    if not commit_epochs:
        print("FAIL no write touched the cct: wrong base image, or nothing committed")
        return 1

    os.makedirs(a.results, exist_ok=True)
    report = open(os.path.join(a.results, "states.tsv"), "w")

    # 1. the protocol itself, read straight off the log
    viol = ordering_violations(epochs, commit, companion)
    for ep, extra in viol:
        rc = 1
        print("FAIL ordering: interval %d commits the cct together with %d other "
              "write(s), first at byte %d (log entry %d)"
              % (ep.number, len(extra), extra[0].off, extra[0].idx))
    if not viol:
        print("PASS ordering: every cct write has its own flush interval "
              "(%d checked)" % len(commit_epochs))

    # 2. every crash state, judged
    states = enumerate_states(epochs, commit, a.prefix_cap, a.random, a.seed)
    for s in states:
        s.acked = acked_before(acks, epochs[s.epoch].end)
    manifests = load_manifests(a.manifests) if a.manifests else {}
    mnt = os.path.join(a.workdir, "mnt")
    os.makedirs(mnt, exist_ok=True)
    base_k = os.path.join(a.workdir, "interval-base.img")
    state_img = os.path.join(a.workdir, "state.img")
    repaired = os.path.join(a.workdir, "repaired.img")
    shutil.copyfile(a.base, base_k)

    counts = {"clean": 0, "recovered": 0, "fail": 0}
    kept = 0
    by_epoch = {}
    for s in states:
        by_epoch.setdefault(s.epoch, []).append(s)
    print("crash states=%d" % len(states))

    for k, ep in enumerate(epochs):
        for s in by_epoch.get(k, []):
            shutil.copyfile(base_k, state_img)
            apply_writes(log, state_img, [ep.writes[i] for i in s.include])
            cls, notes = judge_fsck(a.fsck, state_img, repaired)
            problems = [] if cls != "broken" else notes
            if not a.no_mount:
                # the kernel's view, with no fsck: what a reboot actually sees
                tree, errs = mounted_tree(state_img, a.mount_type, mnt)
                problems += errs if tree is None else \
                    compare_tree(tree, errs, manifests, s.acked)
                if cls == "recovered":
                    tree, errs = mounted_tree(repaired, a.mount_type, mnt)
                    problems += ["after repair: " + x for x in
                                 (errs if tree is None else
                                  compare_tree(tree, errs, manifests, s.acked))]
            verdict = "FAIL" if problems else "PASS"
            counts["fail" if problems else cls] += 1
            report.write("%s\t%s\t%d/%d\tack=%d\t%s\n"
                         % (verdict, s.name(), len(s.include), len(ep.writes),
                            s.acked, cls))
            if problems:
                rc = 1
                tag = "fail-%04d" % counts["fail"]
                with open(os.path.join(a.results, tag + ".txt"), "w") as f:
                    f.write("state %s, %d of %d writes (log entries %s), ack=%d, fsck %s\n"
                            % (s.name(), len(s.include), len(ep.writes),
                               [ep.writes[i].idx for i in s.include], s.acked, cls))
                    f.write("\n".join(notes + problems) + "\n")
                if kept < a.keep_failed:
                    shutil.copyfile(state_img, os.path.join(a.results, tag + ".img"))
                    kept += 1
                if counts["fail"] <= 5:
                    print("FAIL %s (%d/%d writes, ack=%d): %s"
                          % (s.name(), len(s.include), len(ep.writes), s.acked,
                             problems[0]))
        apply_writes(log, base_k, ep.writes)
    report.close()

    print("states: %d clean, %d recovered by the designed cct path, %d FAILED"
          % (counts["clean"], counts["recovered"], counts["fail"]))
    print("details: %s" % a.results)
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("parse")
    p.add_argument("log")
    p.set_defaults(fn=cmd_parse)

    p = sub.add_parser("manifest")
    p.add_argument("dir")
    p.add_argument("out")
    p.set_defaults(fn=cmd_manifest)

    p = sub.add_parser("replay")
    p.add_argument("--log", required=True)
    p.add_argument("--base", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_replay)

    p = sub.add_parser("check")
    p.add_argument("--log", required=True)
    p.add_argument("--base", required=True, help="device contents before logging began")
    p.add_argument("--fsck", required=True)
    p.add_argument("--manifests", help="directory of manifest.N.json")
    p.add_argument("--mount-type", default="spadfs2")
    p.add_argument("--no-mount", action="store_true")
    p.add_argument("--require-end", action="store_true")
    p.add_argument("--random", type=int, default=6, help="random subsets per interval")
    p.add_argument("--prefix-cap", type=int, default=16)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--results", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--keep-failed", type=int, default=3)
    p.set_defaults(fn=cmd_check)

    a = ap.parse_args()
    try:
        return a.fn(a)
    except (LogError, OSError, ValueError) as e:
        sys.stderr.write("logwrites: %s\n" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
