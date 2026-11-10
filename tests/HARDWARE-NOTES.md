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

## 2026-11-09: backups at the end of the volume -- commit cost

mkspadfs first moved the cct backup, its CRC region and the txblock backup to
the end of the volume. Same module, layouts alternated with
`tests/commit_cost_ab.sh --mkfs-a <HEAD mkspadfs2> --mkfs-b <new mkspadfs2>`
on `/dev/sda1` (WDC WD5000LPVX, rotational), 5 rounds of 100 commits:

| Layout | Median ms/commit | Range | KiB/commit |
|---|---|---|---|
| A: backups next to the primaries | 34.0 | 33.3-34.1 | 42.7 |
| B: backups at the end | 89.3 | 89.2-91.2 | 42.9 |

+55.3 ms per commit (+163%): the cct backup is written synchronously after
every commit, so each commit sends the head to the far end and back. The
txblock backup is written rarely and costs nothing measurable there. So
mkspadfs now chooses per device: the txblock backup always at the end, the
cct backup at the end on non-rotating devices and image files and next to
the primary on rotating disks (sysfs queue/rotational, via the parent disk
for a partition), with --cct-backup-far/--cct-backup-near to override. All
root tests passed with both backups at the end (mount_smoke, cct_mirror_mount,
cct_crc_mount, crash_logwrites, meta_csum_mount).

Re-run after the per-device choice (B now picks the near cct backup on
`/dev/sda1` by itself, txblock backup at the end): both medians around 34 ms
per commit, within noise, as reported by the operator.

## 2026-11-09: keyed directory hash (Phase 5)

`tests/hashsim.py` found no clustering in the v1.0.22 hash for real or
synthetic names up to 1M per directory, but the hash is linear and unkeyed:
16 384 crafted names put a directory into one leaf chain (346 page reads per
lookup on average, 687 at worst). The hash is now keyed SipHash-2-4 of the
case-folded name with a random per-volume seed in the superblock
(`name_hash.h`); in the model the crafted names take 3 page reads. Root runs
by the operator on 7.3.0_1, all pass: `meta_csum_mount.sh` (a 3000+ name
directory split by the new hash; spadfsck's placement check agrees with the
driver for every name), `apage_index_mount.sh` (2400 names), `mount_smoke.sh`,
`crash_logwrites.sh`, `cc_wrap_mount.sh`.

## 2026-11-09: long symlinks (Phase 6, first half)

Targets over 164 characters, up to 4095, are stored as the symlink's file
data: UNX attribute of mode S_IFLNK, an empty SYM attribute as the marker.
Root runs by the operator on 7.3.0_1, all pass: `symlink_mount.sh` (new: 1,
164, 165, 1000 and 4095 characters read back, 4096 refused, a long target
followed, fsck clean, targets survive a remount, deletes free the blocks;
both block sizes), `crash_logwrites.sh` (new op 14: a 400-character symlink;
every acknowledged crash state shows the exact target), `mount_smoke.sh`,
`meta_csum_mount.sh`.

Three bugs were found by the root runs before they passed: page_symlink was
given strlen instead of strlen + 1 (it writes len - 1 bytes), so every long
target lost its last character; the fnode written while the target's block
was allocated but i_size still 0 encoded that block as preallocation, which
both readers reject on a non-file; and nothing wrote the fnode after the
target -- the driver writes a file's fnode on extend, fsync, truncate,
setattr and close, and a symlink is never opened -- so the size and runs
were never recorded (12 lost sectors at 512-byte blocks).

## 2026-11-09: spadfs2 against v1.0.22 (side-by-side rig)

`tests/build_reference.sh` builds v1.0.22 (59e33f2, plus only the 7.3
`->create` compile fix in `tests/reference/`) as spadfs.ko / type "spadfs";
`tests/bench_ab.sh` alternates it with spadfs2 on `/dev/sda1` (WDC
WD5000LPVX, rotational), reformatting every round. 3 rounds, 50 commits,
20 000 files, 256 MB. Operator's run on 7.3.0_1, medians:

| Metric | v1.0.22 | spadfs2 | B vs A |
|---|---|---|---|
| commit (ms) | 22.83 | 34.76 | +52% |
| create 20k + sync (s) | 1.01 | 1.07 | +6% |
| cold stat 20k (s) | 0.08 | 0.11 | +38% |
| unlink 20k + sync (s) | 0.06 | 0.09 | +50% |
| seq write (MB/s) | 98.7 | 95.7 | -3% |
| seq read, cold (MB/s) | 102.9 | 99.8 | -3% |

- Commit: +11.9 ms, consistent within 1 ms across rounds -- the one cache
  flush added before the cct write (11.6 ms measured earlier) plus ~0.3 ms
  of cct CRC and mirror writes. A correctness fix: v1.0.22 could commit
  before its metadata reached the media.
- create/unlink: tens of ms over 20 000 files (~3 us per file), consistent
  with per-modification CRC sealing, SipHash and dnode CRC updates, plus the
  one extra flush in each phase's final sync.
- cold stat: +30 ms over 20 000 files; candidate causes are the keyed hash
  packing sequential names less densely (~62% vs 68% leaf fill in
  tests/hashsim.py, so ~10% more directory pages) and CRC checks on every
  directory block read. Durations are near the script's 0.01 s
  resolution; rerun with -f 200000 to confirm the percentage.
- write/read: the control (identical data path). Write is consistently 3%
  lower, reads overlap; most likely the file's position on the disk (outer
  tracks are faster) under the two layouts. Not yet checked with filefrag.

Rerun at 200 000 files (`-f 200000`), medians:

| Metric | v1.0.22 | spadfs2 | B vs A |
|---|---|---|---|
| commit (ms) | 22.83 | 34.76 | +52% |
| create 200k + sync (s) | 11.37 | 14.75 | +30% |
| cold stat 200k (s) | 2.87 | 2.81 | -2% |
| unlink 200k + sync (s) | 0.78 | 0.93 | +19% |
| seq write (MB/s) | 97.1 | 95.4 | -2% |
| seq read, cold (MB/s) | 102.5 | 102.5 | 0% |

- cold stat: no regression; the +38% at 20k was the script's resolution.
- create: the per-file cost grows with directory size (+3 us/file at 20k,
  +17 us/file at 200k), so it is not the fixed per-file CPU work (CRCs,
  SipHash). Leading hypothesis, unconfirmed: building a large directory
  forces commits partway through, each now one flush (~12 ms) dearer;
  ~280 of them would explain the 3.4 s. The block layer's flush counters
  (field 15 of /sys/class/block/{sda,sda1}/stat) read 0 on this kernel, so
  counting flushes per phase needs block tracing (block_rq_issue).

## 2026-11-09: spadfs2 against ext4 and XFS

`tests/bench_ab.sh ./spadfs2.ko spadfs2 ./mkspadfs2 - ext4,noinit_itable
'mkfs.ext4 -F -q' - xfs 'mkfs.xfs -f -q'` on `/dev/sda1` (WDC WD5000LPVX,
rotational), 3 rounds, side order rotated, fresh mkfs every time; 50
commits, 20 000 files, 50 fsyncs per writer, 256 MB. Defaults otherwise
(ext4 data=ordered; ext4 mounted noinit_itable so its background inode-table
init does not run during the measurements). Operator's run on 7.3.0_1,
medians:

| Metric | spadfs2 | ext4 | XFS |
|---|---|---|---|
| commit, syncfs (ms) | 34.54 | 85.98 | 34.67 |
| create 20k + sync (s) | 1.05 | 1.86 | 1.80 |
| cold stat 20k (s) | 0.11 | 0.18 | 0.37 |
| unlink 20k + sync (s) | 0.09 | 0.33 | 0.95 |
| fsync, 1 writer (ops/s) | 30 | 18 | 27 |
| fsync, 8 writers (ops/s) | 51 | 40 | 58 |
| seq write (MB/s) | 98.4 | 98.9 | 98.6 |
| seq read, cold (MB/s) | 103.8 | 104.1 | 108.9 |

- Single-threaded metadata work (create, cold stat, unlink) is 40-90%
  faster than both: the inode-in-directory design, with no inode table to
  allocate in, read or free.
- Commit and 1-writer fsync match XFS and beat ext4 (ext4's commit varied
  86-130 ms across rounds).
- Parallel fsync: XFS leads by 14%. Scaling from 1 to 8 writers: spadfs2
  1.7x, ext4 2.2x, XFS 2.1x -- consistent with spadfs2's single
  filesystem-wide lock; on this disk every writer mostly waits on flushes,
  so fast storage would likely widen the gap.
- Data throughput equal; XFS's +5% read is most likely file placement.
- Not covered: directories of 200k+ names (spadfs2's create cost grows with
  directory size, see above), SSD/NVMe, many cores, aged volumes, and equal
  crash guarantees for file data. These runs predate spadfs2's ordered
  writeback (Step 1 in
  docs/superpowers/plans/2026-11-09-next-steps-after-review.md), which
  makes commits write the data of files that grew first, like ext4's
  data=ordered; rerun before comparing commit or fsync cost.

Rerun at 200 000 files (`-f 200000`), same command otherwise, medians
(ext4's spread in brackets where it is wide):

| Metric | spadfs2 | ext4 | XFS |
|---|---|---|---|
| commit, syncfs (ms) | 34.75 | 129.82 | 35.55 |
| create 200k + sync (s) | 14.75 | 17.32 (14.50-21.62) | 18.94 |
| cold stat 200k (s) | 2.81 | 2.22 (1.65-4.64) | 2.80 |
| unlink 200k + sync (s) | 0.94 | 2.89 | 10.31 |
| fsync, 1 writer (ops/s) | 29 | 15 | 22 |
| fsync, 8 writers (ops/s) | 50 | 48 | 57 |
| seq write (MB/s) | 97.8 | 96.2 | 92.4 |
| seq read, cold (MB/s) | 101.9 | 107.5 | 109.6 |

- Create: spadfs2 keeps the fastest median, but the lead narrows from 40%+
  at 20k to 15-22%; ext4's best round (14.50 s) beat spadfs2's.
- Cold stat: ext4's median is 27% faster at this size (it was slower at
  20k), with a wide spread. Plausible, unverified: spadfs2 reads ~30 MB of
  directory pages holding the inode data, in hash order, while ext4 reads
  compact directory blocks and inode tables in order, which readahead suits.
- Seq read: spadfs2 is 5-7% behind only at this size, where the big file is
  written after 200k files were created and deleted; with no delayed
  allocation it may sit further in or in pieces. Check with filefrag -v.
- Unlink (3-11x faster than ext4/XFS) and 1-writer fsync leads hold; XFS
  still leads 8-writer fsync by 14%.

## 2026-11-09: ordered writeback (no stale data after a crash)

Same disk and command as above (`bench_ab.sh` against ext4 and XFS, 3
rounds, 20 000 files), after Step 1 of
docs/superpowers/plans/2026-11-09-next-steps-after-review.md: a file's
fnode never shows more than the data known to be written (`safe_size`),
and periodic/sync commits write the data of files that grew first. Medians,
with the run before the change for spadfs2:

| Metric | spadfs2 before | spadfs2 after | ext4 | XFS |
|---|---|---|---|---|
| commit, syncfs (ms) | 34.54 | 34.74 | 127.54 | 34.45 |
| create 20k + sync (s) | 1.05 | 1.11 | 1.76 | 1.76 |
| cold stat 20k (s) | 0.11 | 0.11 | 0.18 | 0.43 |
| unlink 20k + sync (s) | 0.09 | 0.10 | 0.30 | 1.09 |
| fsync, 1 writer (ops/s) | 30 | 30 | 22 | 28 |
| fsync, 8 writers (ops/s) | 51 | 46 | 40 | 58 |
| seq write (MB/s) | 98.4 | 98.5 | 91.1 | 96.4 |
| seq read, cold (MB/s) | 103.8 | 103.9 | 108.4 | 109.2 |

- Only 8-writer fsync moved, and it took two steps to get back this far.
  With the size capped alone it fell to 33-34: a file recorded by another
  writer's commit at its safe size needed a full commit to grow. Growing it
  in place after flushing its data (34) did not help -- two flushes either
  way. Numbering the flushes did (46): a writer usually waits on sync_lock
  behind another writer's commit, whose flush already covered its data, so
  one flush suffices again (`spadfs_flushed_since`, `spadfs_file_fsync`).
- The remaining 10% is the extra fnode write the in-place growth needs:
  before the change, the other writer's commit had already recorded the
  full size -- the very write that could expose stale data.
- 1-writer fsync and commit are unchanged: each fsync there creates a new
  file, which commits anyway, and the commit's extra pass finds nothing.

## 2026-11-09: xfstests baseline (quick group)

`tests/xfstests_vm.sh` runs xfstests (upstream 22348af, 2026-09-20) in a
throwaway QEMU guest booting the host's 7.3.0_1 kernel -- no root on the
host. Test and scratch devices are 4 GB and 8 GB virtio disks;
`tests/xfstests.exclude` skips generic/013 (see there). The same script with
`--script` runs this tree's root tests in the guest.

`./check -g quick`: 646 tests, 446 not run (reflink 146, hole punch /
zero range / insert / collapse 42, encryption 22, shutdown 16, exchange
range 14, dedupe 14, fs-verity 10, NFS export, O_TMPFILE, freeze, DAX;
plus harness gaps: no attr/chacl/quota tools in the host, the fsgqa user
cannot run commands, xfstests' sized mkfs does not know spadfs2), 200 run.

Fixed while building the baseline:

| Test | Problem | Fix |
|---|---|---|
| generic/465 (and any file in > ~50 pieces) | a full leaf anode got a 31st extent written over its CRC trailer; every reader then rejected it, so badly fragmented files became unreadable | `find_direct` in struct.h: leaf size is ANODE_N_EXTENTS (30), not a stale literal 31; tests/fragment_mount.sh |
| generic/236 | link and unlink of a hard-linked file left its ctime | namei.c sets ctime before rewriting the fnode |
| generic/313 | truncate left mtime and ctime | spadfs_file_setattr sets both when the size changes |
| generic/260, 288 | FITRIM accepted a start past the end and a length below one block; start + len could overflow | ioctl.c returns EINVAL, clamps the end |
| generic/634 | times past 2106 wrapped (to 2041) | s_time_min/max declared (5.4+), so the VFS clamps; s_time_gran is 1 s |
| generic/131, 732, 741, 798 | harness: no loopback; mkspadfs progress on stderr | xfstests_vm.sh |

Failing, by design or missing feature:

- generic/003: no access time on disk (fnodes hold ctime and mtime only).
- generic/258: times before 1970 (the 32-bit time field is unsigned).
- generic/092, 094, 225, 490, 677, 796, 797: unwritten extents and holes
  (no sparse files; Step 3 of the plan). Also generic/013, excluded: its
  writes at offsets up to ~670 MB fill the 4 GB test device without holes.
- generic/504: stat's inode number (the persistent stable_ino) is not the
  in-memory i_ino that /proc/locks prints.
- generic/537: no "norecovery" mount option (there is no log to skip).
- generic/740: mkspadfs overwrites another filesystem without being forced.

Open findings, to investigate:

- generic/347, 441: after a transient I/O error the volume keeps its
  TXFLAGS_IO_*_ERROR flags, so `spadfsck -n` fails once the device is
  healthy again; 441 also gets EIO from fsync on a reopened fd after the
  error cleared. ext4 marks only metadata errors.
- generic/483: space preallocated past EOF with fallocate(KEEP_SIZE) and
  fsynced does not survive a power cut (only the written part does). Also
  on the module before the ordered-writeback commit, so not a regression.
- generic/676: seeking a directory to a position that is not one of its
  cookies makes readdir return EOF.

## 2026-11-10: xfstests baseline (auto group)

Same harness, after closing its gaps: attr/acl/quota tools unpacked into
the guest only (EXTRA_PKGS), the fsgqa users given shadow entries (su needs
them), spadfs2 cases patched into the guest's copy of xfstests' sized and
block-size mkfs helpers and its timestamp-range table, and a third disk for
dm-log-writes.

`./check -g auto`: 794 tests, 490 not run (mostly reflink, hole punching,
encryption, shutdown, dedupe, exchange range, verity, POSIX ACLs and disk
quotas -- both now reach their real check and spadfs has neither), 304 run,
34 failed. The test device ended consistent (spadfsck clean); no oops, hang
or lockup.

Found and fixed during the run (tests/fill_mount.sh and
tests/fragment_mount.sh cover the first two):

| Problem | Fix |
|---|---|
| Filling a volume with small files damaged ~1 in 25 volumes: a directory page got 8 sectors where it asked for 16 and used all 16, overlapping a spare apage and later file data ("free ... does overlap with block", dnodes pointing at file contents, lost sectors). spadfs_alloc_blocks retried after freeing the small-file preallocation with the request its failed attempt had halved. In v1.0.22 too: random names hit it 1 fill in 200; the keyed hash spreads sequential names the same way, which made it common (bisected to 7373c9c). | alloc.c retries with the request as made |
| A write far past EOF on a volume with fragmented free space advanced ~2 pages a second (generic/285 ran over 40 minutes): each page preallocated toward the target one hole per extent, ran out near the end, freed everything and started over | spadfs_do_extend keeps a partial allocation that covers what the caller needs (rounded to a cluster) |
| fallocate kept setuid/setgid (generic/683, 688) | file_remove_privs, as a write does |
| setxattr/removexattr left ctime (generic/728) | xattr.c sets it |

Still failing, by design or missing feature:

- No access time: generic/003, 192, 402.
- No times before 1970: generic/258.
- No holes or unwritten extents (sparse files, Step 3): generic/092, 094,
  225, 285, 490, 677, 679, 796, 797. generic/251 and 795 fill the scratch
  device through fsstress's far-offset writes; generic/013 is excluded for
  the same reason.
- Extended attributes share one 168-byte pool and values are at most 255
  bytes: generic/020, 403, 486, 618.
- An attribute with an empty value was invalid on disk (GET_XAT rejected a
  zero value length in all three readers), so setting one was a no-op:
  generic/062, 611. Fixed since: empty values are allowed and an empty
  name is the invalid case; both tests pass.
- No "discard" mount option (log-writes replay mounts with it): 455.
- No "norecovery" mount option: 537.
- No RENAME_WHITEOUT, so overlayfs cannot use spadfs as its upper layer:
  631.
- stat's inode number is not the in-memory i_ino: 504.
- mkspadfs overwrites another filesystem without being forced: 740.

Open findings, to investigate (generic/347, 441, 483, 676 as in the quick
baseline, plus):

- generic/363: fsx with EOF pollution -- bytes written past EOF through a
  mapping read back after the size grows, where zeros belong. Only the
  file's own bytes, but wrong. Fixed since: cont_write_begin zeroes from
  mmu_private, which is rounded up to the end of the block holding EOF, so
  junk stored through mmap between EOF and that block's end survived. A
  write starting past EOF (truncate up and fallocate come through it too)
  now zeroes that tail first with block_truncate_page. Fails without the
  fix, passes with it.
