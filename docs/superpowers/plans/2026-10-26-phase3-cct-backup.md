# Phase 3: Crash-Count Table Backup

**Depends on:** Phase 2 (crc32c side table), Phase 1 (spadfs2 identity)
**Format impact:** Additive — a new region plus one superblock pointer
**Testable without root:** Mostly. Task 3.1 and 3.2 fully; 3.3 needs a mount.

---

## Where Phase 2 leaves us

Phase 2.3 gave the cct **detection without recovery**.

`check_cct_crc()` (fsck) now catches every corruption class that was previously
silent — the baseline went from 3 silent corruptions to 0. But when it fires,
there is nowhere good to copy from. The only recovery available today is
`fix_cct()` (`sckcct.c:95`), which **resets the entire table** and zeroes
`tx.cc`.

Resetting is not recovery. It discards the epoch mapping, and the epoch mapping
is what tells the filesystem which version of every dual-versioned structure is
the live one. After a reset you are revalidating the whole volume from scratch,
and anything that depended on a specific `cc` epoch has lost its reference.

So: **we can now tell that a cct is destroyed. Phase 3 is about not having to
destroy it.**

---

## The two redundancy precedents already in this format

This is not new machinery. The format already does exactly this twice.

### Precedent 1 — backup superblock

| | |
|---|---|
| Written | `mkspadfs.c:527` — `write_blocks(sb, size, block_sectors)` at the volume end |
| Validated | `scksuper.c:177` — `if (!memcmp(s, &super, 512)) backup_valid = 1;` |
| Discovered | `scksuper.c:198-210` — scans backwards from detected size |
| Repaired | `scksuper.c:239` — offers `CREATE BACKUP SUPERBLOCK?` when missing |

Simple mirror. Adequate because the superblock changes rarely.

### Precedent 2 — dual apage index (the one that actually matters here)

| | |
|---|---|
| Pointers | `super.apage_index[0]`, `super.apage_index[1]` |
| Selection | `sckapage.c:66` — `c_index = !cc_valid(tx.a_cc, tx.a_txc);` |
| Kernel | `super.c:1525` — `sec = CC_VALID(fs, &fs->a_cc, &fs->a_txc) ? ...` |

Two copies, **both kept current**, and the crash-count epoch decides which one is
authoritative. Writing one invalidates the other by advancing the epoch. This is
the pattern for a structure that changes continuously — which is exactly what
the cct is.

**Phase 3 should follow Precedent 2, not Precedent 1.** A naive mirror
(Precedent 1) is the wrong shape for a table that is written on every commit.

---

## The central problem: staleness is dangerous here

This is the thing to settle before writing code, and it is what makes the cct
different from the superblock.

Recovery semantics: `CC_VALID(cc, txc)` is `(s32)(cct[cc] - txc) >= 0`
(`spadfs.h:550`). Every dual-versioned structure — `fnode`, `fnode_block`,
`apage_head`, `dnode_page`, `txblock` — picks its live version through this.

Now suppose we restore a **stale** cct, where `cct[cc]` is behind the `txc`
recorded in structures:

```
cct[cc] = 1000   (backup, from last clean unmount)
txc     = 5000   (structure, written later)
(s32)(1000 - 5000) < 0  =>  CC_VALID = false
```

The structure's *newer* writes read as invalid, so the **alternate, older**
version becomes authoritative. Concretely: files created since the checkpoint
vanish, deleted files come back, renames unwind.

That is not corruption detected — that is **silent data reversion**. It is worse
than the bug we are fixing, because the filesystem mounts happily and lies.

**Conclusion: a cct backup that is not kept current is unsafe to restore.**
"Back it up at unmount and hope" is not a design. Any backup used for recovery
must either be written on every commit, or carry enough epoch information to
prove it is not stale.

---

## Options

### 3A — Synchronous mirror (Precedent 2 pattern)

Back up the cct to a second region, writing the mirror sector alongside each
primary sector in the commit path.

- **Always current.** Restore is always safe.
- **Cost:** the cct write is already block-sized —
  `512U << fs->sectors_per_buffer_bits` bytes (`super.c:722`). On a 4 KiB-block
  volume that is 8 sectors per commit, so mirroring makes it 16, plus 2 CRC
  units instead of 1. **Roughly doubles the cct-related write amplification on
  the hot path.**
- **Space:** +256 KiB per volume (plus ~2.5 KiB for its CRC region).
- **Testability:** mkfs + fsck halves fully testable unprivileged. The kernel
  commit path needs a mount.

### 3B — Checkpointed backup with an epoch stamp

Back up periodically (unmount + every N commits), and store in the backup header
the `cc`/`txc` it was taken at. At recovery, compare the backup's epoch against
the primary's surviving evidence; **refuse to restore if the backup is too far
behind** and fall back to reset.

- **Cost:** near-zero on the hot path.
- **Safety:** preserved — the staleness guard prevents silent reversion. You
  degrade to today's behaviour (reset) instead of to data loss.
- **Downside:** recovery only succeeds when the backup happens to be recent.
  For a volume that mounts, works hard, and crashes, the backup may be useless.
- **Needs:** a place for the epoch stamp, and a rule for "how far behind is too
  far" that is actually decidable from on-disk evidence.

### 3C — Mirror only the live prefix

Only `cct[0..tx.cc]` is meaningful; entries above `tx.cc` must be zero (fsck
enforces this today). The prefix grows by **one 4-byte entry per mount**, not per
commit.

- The hot entry is always the *last* one (`cct[tx.cc]` changes every commit), so
  the mirror work concentrates in one sector — the same sector the primary is
  already writing.
- **Cost:** effectively the same as 3A for the hot path, but the untouched tail
  is never rewritten.
- **Complication:** the prefix wraps across sectors as `cc` grows, so "which
  sectors are live" is dynamic state the mirror logic must track.

---

## Recommendation

**3A, but only after measuring the write cost — and 3B as the fallback if 3A is
too expensive.**

Reasoning:

1. 3A is the only option that makes restore *unconditionally* safe. For a
   filesystem, unconditional safety is the right default.
2. It is the established pattern in this format (Precedent 2), not a new idea.
3. The cost is real but bounded and measurable — and **measuring it is exactly
   what the Phase 0.4 reference rig exists for.** We should not guess at whether
   doubling cct writes matters; we should run the workload old-stack vs new-stack
   and look.
4. If the measurement says 3A is too hot, 3B is a strictly cheaper design that
   keeps the safety property via the staleness guard — but it needs a rule for
   "too stale", and that rule must be derivable from disk, not from hope.

**Do not implement 3A's kernel commit-path change until the rig has been run.**
That is the one part that is both untested here and on the hot path.

---

## Tasks

Ordered so the risky part comes last and the testable part comes first.

### Task 3.1 — Layout and format (testable, no root)

- New superblock field: `__u64 cct_backup`. Space is not a constraint —
  `sizeof(struct superblock)` is **112 bytes inside a 512-byte unit**, leaving
  **400 free bytes** (measured). Add it after `cct_crc` and leave the rest as
  explicit padding. Do not squeeze `pad1`; there is no reason to.
- `mkspadfs2` allocates the backup region immediately after the cct CRC region,
  **block-aligned** — the Cat 2 lesson: a non-block-aligned gap becomes an
  unaligned apage entry and fsck rejects it.
- Zero-fill the backup at format time and seal it with the standard trailer so it
  is a valid, empty, CRC-covered copy from birth.
- Extend `CCT_CRC_*` style constants: `CCT_BACKUP_SECTORS`, and a second CRC
  region for the backup (or widen the existing one to cover both).
- **Pin every new size with `_Static_assert`** before writing logic. Phase 2.4
  caught three real desyncs this way; the `apage_head == aentry` discovery came
  from exactly such an assert.

**Acceptance:** `mkspadfs2` produces a volume where the backup region exists,
is block-aligned, verifies, and `roundtrip.sh` passes all 18 profiles.

### Task 3.2 — fsck: verify and restore (testable, no root)

- `check_cct_backup_crc()` — same two-layer check as `check_cct_crc()`:
  verify the backup's CRC units, then the backup's sectors against them.
- **Restore path:** if the primary fails CRC and the backup passes, offer
  `RESTORE CRASH COUNT TABLE FROM BACKUP?`. On accept, copy backup → primary,
  re-seal, write.
- If **both** fail: fall back to today's `fix_cct()` reset. Say so explicitly in
  the log — the operator needs to know they are getting the destructive path.
- If primary passes and backup fails: offer `RECREATE BACKUP?` (mirrors
  `CREATE BACKUP SUPERBLOCK?`).
- **Reserve the region** with `alloc_blocks()` — skipping this produced
  `FILESYSTEM HAS 8 LOST SECTORS IN 1 RUNS` in Phase 2.3. Same trap.
- Extend `tests/corrupt.py` / add a target so the harness covers: primary
  corrupt + backup good, primary good + backup corrupt, both corrupt.

**Acceptance:** a new `tests/cct_recovery.sh` proving all four combinations
behave correctly, and that restore produces a volume that then passes
`cct_integrity.sh --strict`.

### Task 3.3 — Kernel: keep the mirror current (**needs mount to test**)

- In `spadfs_commit_unlocked`, after the primary cct write and CRC update,
  mirror the same sectors into the backup region and update the backup's CRC.
- Preserve the ordering discipline established in 2.3: primary first, then CRC,
  then mirror. A crash between primary and mirror leaves the mirror stale —
  which must be **detectable**, not silently trusted.
- This is why the mirror needs its own epoch stamp even in 3A: without one, a
  stale mirror is indistinguishable from a corrupt-but-restorable one.
- **Do not land this before 3.2 is green and the write cost is measured.**

**Acceptance:** mount, generate commits, unmount; fsck reports primary and backup
both valid and in agreement. Requires root — blocked on the reference rig.

### Task 3.4 — Measure the write cost (needs root)

Using the Phase 0.4 rig: same metadata-heavy workload, cct-mirror vs no-mirror,
compare commit latency and total sectors written. Decide 3A vs 3B on evidence.

---

## What is blocked, and what to do about it

Tasks 3.3 and 3.4 need `insmod`/`mount`. This environment has no passwordless
sudo and the machine cannot be rebooted right now.

**Tasks 3.1 and 3.2 are fully executable today** and are where most of the
design risk actually lives — layout, alignment, reservation, restore semantics.
They can be built, tested, and committed now.

3.3 is mechanically small once 3.1/3.2 exist, but it is the part that touches
the hot path and cannot be verified here. Draft it, review it, and hold the merge
until the rig runs.

---

## Open questions

1. **Does the backup need its own `cc`/`txc` epoch stamp even in 3A?**
   Probably yes — a mirror that is one commit behind must be distinguishable from
   a mirror that is corrupt. Needs thinking before coding 3.3.

2. **What is the "too stale to restore" rule for 3B?** It must be derivable from
   on-disk evidence alone. If it cannot be made rigorous, 3B is unsafe and 3A is
   mandatory regardless of cost.

3. **Is 256 KiB × 2 acceptable at the small end?** For a minimal volume the
   backup is a meaningful fraction of metadata. Worth checking against the
   smallest supported geometry.

4. **Should the backup be at a fixed offset from the end** (like the backup
   superblock) rather than adjacent to the primary? Fixed-from-end survives
   partial-table damage better and is easier to find when the superblock itself
   is compromised — but it complicates the layout code.

---

## Explicitly out of scope

- **Widening `cc` (Phase 4).** The current plan calls it "mechanical". It is
  not: `apage_head` must equal `aentry` at 16 bytes, and widening `cc` takes it
  to 20. Measured, asserted in `ccad05c`. Phase 4 needs its own design pass.
- **fnode_block trailer coverage.** Structurally blocked — the fnode chain is
  terminated by the 512-byte block boundary, so 508..511 cannot be reserved.
- **Cat 3 (dnode/apage).** Needs a side table and has no harness coverage.
