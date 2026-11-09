# Hardware measurements

Hand-written results from runs on real hardware. Unlike the `BASELINE-*.md`
files, `make test` does not regenerate this file, so notes here survive it.
(They used to be appended to `BASELINE-restripe-attribution.md`, which
`make test` rewrites from scratch.)

## Post-mount verification (real kernel writes, 465 GB device)

The first mount of spadfs2 exposed two kernel-side defects that no
mkfs->fsck test could see. Both are fixed; this records the verified state.

| Bug | Symptom | Cause |
|---|---|---|
| `fixed_fnode_block` | every mount failed `EUCLEAN` | Cat 2 converted the trailer in struct.h/mkfs/fsck but not the kernel; the read path still tested the XOR, which mkfs zeroes |
| cct CRC units 1-4 | `UNIT 1..4 CHECKSUM MISMATCH` after any write | `spadfs_get_new_sector` is **block**-granular and does not read-modify-write, so writing unit 0 wrote the whole 4096-byte block and zeroed units 1..4, which live in sectors 1..4 of it |

After both fixes, `tests/diag_cct_crc.py` on a device that had real kernel
writes:

    cct sectors that are NOT all-zero: 1  ([0] -- bumped by mount's increase_cc)
    unit 0: stored 6de30e61 computed 6de30e61 MATCH
    unit 1: stored 9b4e89e7 computed 9b4e89e7 MATCH
    unit 2: stored 9b4e89e7 computed 9b4e89e7 MATCH
    unit 3: stored 9b4e89e7 computed 9b4e89e7 MATCH
    unit 4: stored 8075bb17 computed 8075bb17 MATCH
    SLOT CONTENT: 0 bad slots in every unit

Geometry that made the second bug possible: CCT_CRC_UNITS=5 fits entirely in
block 0 (block_sectors=8), so a commit that updates one unit necessarily
writes the other four.

### Commit-cost baseline (Task 3.4 input)

100 x (echo > file; sync) on the mounted volume: **2.687 s real = ~27 ms per
commit**. 64 MB dd: 29.4 MB/s. This is the number the mirror cost in 3.3 is
measured against.

### Still stale by design

    backup sector 0 all-zero: True

The mirror is mkfs-only until Task 3.3 lands. Its CRC region verifies because
it faithfully describes an all-zero table -- which is exactly why restoring it
today would reset the crash count and promote the older alternate of every
dual-versioned structure. Task 3.2's `Q_IMPORTANT` gate on restore stays until
the kernel keeps the mirror current.

## 2026-11-08: flush ordering, dm-log-writes crash states, apage/dnode checksums

Root runs by the operator on 7.3.0_1, after `ef221c6` (pre-cct flush),
`0d4d5db` (dm-log-writes test), `e2b6b4d` (pipefail test fixes) and
`82f4752` (kernel seals and verifies apage/dnode checksums).

### Current tree: all pass

| Test | Result |
|---|---|
| `tests/meta_csum_mount.sh` | pass |
| `tests/cc_wrap_mount.sh` | pass (first run since its pipefail fixes) |
| `tests/crash_logwrites.sh` | pass: ordering check and every replayed crash state |
| `tests/mount_smoke.sh` | pass |

Per-check counts were not recorded. The first `meta_csum_mount.sh` run,
before the pipefail fix, did record the kernel side in full: at 512 and
4096 blocks, every active apage half and the dnode the workload produced
were sealed and matching (`apage_good=1 dnode_good=1`, nothing unsealed),
fsck accepted every kernel-written checksum, a one-byte flip in the dnode
failed the directory listing with `bad checksum on dnode` in the kernel
log, and a flip in the apage failed the mount with `bad checksum on
apage`. Its 4 failures were the pipefail bug in the test, not fsck.

### Positive controls: both fail as they must

A test that cannot fail proves nothing, so each was run against a module
built from the commit before the change it guards (`SPADFS_KO=...`, steps
in each script's header).

**`crash_logwrites.sh` with the pre-flush module (`ef221c6~1`).** The
ordering check flags every commit; one block size's output:

    FAIL ordering: interval 4 commits the cct together with 8 other write(s), first at byte 9437184 (log entry 75)
    FAIL ordering: interval 6 commits the cct together with 8 other write(s), first at byte 9441280 (log entry 90)
    FAIL ordering: interval 8 commits the cct together with 2 other write(s), first at byte 9437184 (log entry 105)
    FAIL ordering: interval 10 commits the cct together with 56 other write(s), first at byte 9445376 (log entry 114)
    FAIL ordering: interval 12 commits the cct together with 324 other write(s), first at byte 9961472 (log entry 177)

So on a real device the v1.0.22 commit put the cct in the same flush
interval as up to 324 metadata writes; the fixed module passes the same
check. The control's crash-state summary (`states: ... FAILED`) was not
captured, so whether the old ordering also produced detectably damaged
crash states is still unrecorded.

**`meta_csum_mount.sh` with the pre-seal module (`82f4752~1`).**

    FAIL  bs512: apage halves unsealed or mismatched
    FAIL  bs512: dnodes unsealed or mismatched

The old module leaves its apage and dnode unsealed; the current one
seals both, so the test separates them.
