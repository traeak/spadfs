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

## 2026-11-09: mirror flush dropped (3 flushes per commit -> 2)

The cct backup mirror is now written without its own flush; the next
commit's pre-cct flush (or unmount / remount-ro) makes it durable, so the
durable mirror is at most one commit behind and is never ahead. Root runs
by the operator on 7.3.0_1:

| Test | Result |
|---|---|
| `tests/crash_logwrites.sh` | pass, including the lagging-mirror crash states |
| `tests/cct_mirror_mount.sh` | pass (its two earlier "no kernel message" failures were the test's line-counted dmesg window, now replaced by a log marker) |

Commit cost, `tests/commit_cost_ab.sh` on `/dev/sda1`: 100 commits per
round, 5 rounds per module, modules alternated, `syncfs` on the spadfs
volume only.

| Module | Flushes/commit | Median ms/commit | Range | KiB/commit |
|---|---|---|---|---|
| A: HEAD before this change | 3 | 56.4 | 55.4-57.6 | 42.3 |
| B: mirror flush dropped | 2 | 44.7 | 43.9-45.5 | 42.2 |

B - A = -11.6 ms/commit (-21%); the ranges do not overlap. Bytes written
are the same, so one cache flush on this disk costs about 11-12 ms. The
earlier single-run figures (27, 56.7 and 72.2 ms) used global `sync` and are
not comparable with these.

## 2026-11-09: checksums always on (Phase 1a of the checksum follow-up)

The `no_checksums`/`checksums` mount options, `mkspadfs --no-checksums` and
the superblock `NO_CHECKSUMS` flag are gone; the kernel and spadfsck verify
every block whatever its per-block `*_CHECKSUM_VALID` bit says, and refuse a
volume with any `flags_compat_fsck` bit set. Root runs by the operator on
7.3.0_1, all pass:

| Test | What it now covers |
|---|---|
| `tests/meta_csum_mount.sh` | new "cleared flag" section: a damaged dnode and a damaged root directory fnode block with their flag cleared are still rejected, and a volume with `flags_compat_fsck` set does not mount; the seal workload adds symlinks, hard links, renames, xattrs, truncation and a fragmented file, and fsck (now flag-blind) agrees with every kernel-written block |
| `tests/mount_smoke.sh` | section 5 now forces anodes (fill, free every other 64 KiB file, write into the holes) and counts them on disk: at least one, all sealed, all with a valid CRC trailer. Before, it only warned that fsck printed nothing about anodes |
| `tests/crash_logwrites.sh` | every replayed crash state judged by the flag-blind fsck |

The positive control (the `HEAD` module before this change must fail the
"cleared flag" section) was not run.

## 2026-11-09: fnode_block header CRC (Phase 1b of the checksum follow-up)

The fnode_block header grew from 24 to 32 bytes to hold a CRC-32C over the
rest of the block, replacing its 8-bit XOR fold. (Apage halves and dnode
versions still use the 8-bit fold; an earlier version of this note and the
commit message of 228b65f wrongly called this the last one.)
FNODE_MAX_SIZE 488 -> 480, FNODE_MAX_EA_SIZE 176 -> 168 (symlinks up to 164
characters, was 172), fixed_fnode_block fnode slots 232 -> 224. Root runs
by the operator on 7.3.0_1, all pass: `meta_csum_mount.sh` (both block
sizes), `mount_smoke.sh`, `crash_logwrites.sh`, `cc_wrap_mount.sh`.

The first `meta_csum_mount.sh` run failed both block sizes with
`/e/hard: INVALID EXTENDED ATTRIBUTES ON FIXED FNODE BLOCK`. The kernel was
right: the tool Makefile rules had no header dependencies, so after the
`struct.h` change only the edited .c files were recompiled and spadfsck
linked a `sckfxfn.o` built against the old layout. It read the second fixed
fnode slot at the old offset (280, not 264). The saved images passed a clean
rebuild of spadfsck unchanged. The unprivileged suite missed it because it
never makes a hard link, so it never creates a fixed fnode block. The
Makefile now generates dependencies (`-MMD -MP`).

## 2026-11-09: txblock backup copy (#2 of the design review)

The superblock's new `txblock_backup` field points at a second txblock,
after the cct backup's CRC region. mkfs writes both copies, the kernel
rewrites the backup at every txblock write (mount/unmount/error flags under
the same flush, the stable-inode batch and the apage-index switch with the
commit), and only spadfsck reads it: a damaged primary is restored from it
instead of being replaced by an empty txblock. Root runs by the operator on
7.3.0_1, all pass:

| Test | What it now covers |
|---|---|
| `tests/mount_smoke.sh` | new section 7: after real mounts both copies verify and match; with the primary damaged the kernel refuses the volume; `spadfsck -y` restores it from the backup; the volume mounts again with its data and checks clean |
| `tests/crash_logwrites.sh` | every replayed crash state, with the backup written alongside the primary |
| `tests/meta_csum_mount.sh` | unchanged checks on the new layout |

## 2026-11-09: dnode CRC-32C

Each dnode version's entry array is covered by a CRC-32C stored in the unused
tail of the dnode page, after both arrays (DNODE_CRC_POS). The driver seals
new dnodes and copies, checks the active version once per load, and updates
the CRC on each entry write from the changed bytes alone (CRC linearity).
Root runs by the operator on 7.3.0_1, all pass: `meta_csum_mount.sh` (the
driver creates dnodes and rewrites them across two commits, fsck agrees with
every one, and a damaged dnode is rejected), `crash_logwrites.sh`,
`mount_smoke.sh`.

## 2026-11-09: apage CRC-32C

Each apage half gives up its last 16-byte entry slot; a CRC-32C over
[8, half - 4) sits in its last 4 bytes. fs->apage_size is now the usable part
of a half (every entry walk and bitmap bound), fs->apage_half the full size
(mapping, copying, checksumming). No structure uses the 8-bit XOR fold any
more; the apage index still has no checksum. Root runs by the operator on
7.3.0_1, all pass: `meta_csum_mount.sh` (every driver-written apage half
sealed and matching, fsck agrees, a damaged apage stops the mount),
`mount_smoke.sh` (fragmenting fill), `crash_logwrites.sh`, `cc_wrap_mount.sh`
(fsck's reset pass rewrites every apage).

## 2026-11-09: apage index CRC-32C

The txblock carries a CRC-32C per apage index copy (apage_index_crc[2]); the
driver updates the copy's CRC at every index write and checks the live copy
at mount. Every metadata structure now has a CRC-32C. Root runs by the
operator on 7.3.0_1, all pass:

| Test | What it covers |
|---|---|
| `tests/apage_index_mount.sh` (new) | a 256 MiB, 512-byte-block volume fragmented until an apage splits (copy 0 -> 1, 2 apages in use); the driver's CRC matches, fsck agrees, the volume remounts with all 1200 files; a flipped byte in an unused tail entry of the live copy (covered by nothing but the CRC) stops the mount with a bad apage index checksum, and fsck -y repairs it |
| `tests/meta_csum_mount.sh`, `tests/mount_smoke.sh`, `tests/crash_logwrites.sh`, `tests/cc_wrap_mount.sh` | pass on the new txblock layout |

The other mount tests use 64 MiB volumes, where a full list apage converts
to a bitmap instead of splitting, so they never reach the index write.
