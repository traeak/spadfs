# Next steps after the 2026-11-09 design review

**Status: planned, not started.** (Open performance questions added after
the benchmarks; see the last section.) Written at the end of the session that
finished roadmap Phases 0–5 and the long-symlink half of Phase 6 (branch
`modernization`, last commit `0537a47`). Do these in order; each has its own
reason for coming before the next.

Workflow reminders (see `AGENTS.md` and `tests/HARDWARE-NOTES.md`):
the agent has no sudo, so root tests are handed to the operator as exact
`sudo bash tests/...` commands, and a kernel change is not called verified
until the operator reports the run. Never commit without an explicit
instruction. Format changes go into all three readers (driver, mkspadfs,
spadfsck) and are proven by a mount.

---

## The review in one page

spadfs2 against ext4, XFS and btrfs, as of `0537a47`:

**Strengths**
- No log replay and no double write: commit = one cct write between two
  cache flushes (44.7 ms/commit on the operator's laptop HDD, about the cost
  of ext4's ordered-mode commit). Mount after a crash is constant time.
- Inodes live in directory entries: `stat` over a directory and `open` need
  no inode-table reads. Good for small-file, metadata-heavy workloads.
- Directory lookups: keyed SipHash-2-4 extendible hash, ~3 page reads up to
  1M names, and crafted collisions no longer degrade it (`tests/hashsim.py`).
- Metadata integrity at parity with ext4/XFS: CRC-32C on every structure,
  checksums always on, no reader trusts a per-block "valid" bit; backups of
  the superblock, cct and txblock.
- Crash ordering proven by dm-log-writes replay of every reordering state
  (`tests/crash_logwrites.sh`).
- ~15 600 lines: small enough to reason about end to end.

**Weaknesses, most serious first**
1. **Never run under xfstests.** Our suite is strong on format and crash
   ordering, thin on POSIX/API behaviour (rename edge cases, O_DIRECT, mmap,
   xattrs, quota, fsync semantics).
2. **File data after a crash -- confirmed, then fixed (Step 1).** A commit
   flushes only metadata (`sync_blockdev` on the block device); file data
   goes out through normal writeback, and a file's size reaches its fnode
   when blocks are allocated (`spadfs_extend_file` -> `spadfs_write_file`),
   before the data is written. So a crash can likely leave a committed size
   covering blocks that still hold their previous contents -- possibly
   another file's deleted data. ext4's default `data=ordered` and XFS both
   prevent this. The long-symlink work hit exactly this, which is why
   `spadfs_symlink` flushes the target before recording the size.
3. Missing features: sparse files, punch hole, reflink/snapshots, online
   resize, fscrypt, DAX. Xattrs share one 168-byte pool per file.
4. Concurrency: one filesystem-wide rw lock (`sync_lock`) that commits take
   exclusively; XFS allocation groups and ext4 per-group locks should scale
   far better with many writers. Unmeasured.
5. No delayed allocation (blocks allocated at `write()`), so files written
   in small pieces fragment more than on ext4/XFS; preallocation mitigates.
6. Inode-in-directory costs: appends dirty the parent's block, rename moves
   the whole fnode (up to 480 bytes), no stable 32-bit inode numbers, no NFS
   export / `open_by_handle_at`, first hard link moves the file to its own
   block.
7. Crash-count ceiling: 65 535 unclean shutdowns, then
   `spadfsck --reset-crash-counts` (warned and tested; operational, not
   structural).
8. No data checksums (same as ext4/XFS).
9. Only the non-rebuildable structures are duplicated, all on one device.

---

## Step 1 -- settle the data-ordering question

**Done.** Confirmed: `crash_logwrites.sh` op_15 (write a 2 MiB file, fsync
another) showed the new file holding a previous file's bytes in 12 of 424
crash states. Fixed without a format change. Each file has an in-memory
`safe_size`, the bytes known to be on disk. Its fnode never shows more than
that; anything allocated beyond it is recorded as preallocation.
`spadfs_make_size_safe` writes and waits for the file's new tail, outside
every filesystem lock, before the size is raised. Periodic and sync commits
first do this for every file that grew (`spadfs_write_unsafe_files`). fsync
does it for its own file only, so it never waits on other files' data.
Eviction records the full size. One more path had to change: the
`SPADFS_OPTIMIZE_FSYNC` in-place write into the committed fnode slot is now
refused when it would make the file show more (`spadfs_in_place_grows`). It
takes effect without a commit, so the disk could reorder it ahead of the
data; 4 crash states still failed that way until it was refused. All crash
states now pass. Cost (tests/HARDWARE-NOTES.md): commit, create/stat/unlink
and 1-writer fsync unchanged; 8-writer fsync 51 -> 46 ops/s, after an
fsync fast path that grows a committed size in place once its data is on
the media (`flushed_size`, numbered flushes in `spadfs_issue_flush`). A
crash-test op (op_16) covers that path.

The original plan follows.

**Test first.** Extend `tests/crash_logwrites.sh` / `tests/logwrites.py` so
that, in every replayed crash state, each regular file's content is checked
against the two legal states (the content before and after the operation in
flight -- the manifests already record both) or an all-zero tail, and never
anything else. Today unacknowledged states are allowed "either state or in
between" (`compare_tree`), which would let stale data through. A workload op
that appends to a file and one that writes a new file are enough to show it.
Positive control: a state where the size is durable but the data is not
should exist in the log if the suspicion is right.

**If it fails, fix with ordered writeback:** before the commit that records
a file's new size, write that file's dirty data (as ext4 `data=ordered`
does). Candidate shape: keep a per-commit list of inodes whose disk size grew
since the last commit (`spadfs_set_disk_size` already tracks
`commit_sequence`/`crash_disk_size`), and in `spadfs_commit_unlocked` call
`filemap_write_and_wait` on each before `sync_blockdev` and flush #1. Watch
the lock order (commit holds `sync_lock` for write; writeback takes it for
read in `spadfs_get_block`?) -- check before writing code. Re-measure commit
cost with `tests/commit_cost_ab.sh`.

## Step 2 -- run xfstests' generic group

**Done for the quick group** (tests/HARDWARE-NOTES.md, "xfstests baseline"):
`tests/xfstests_vm.sh` runs it in a QEMU guest without host root, and runs
this tree's root tests there too (`--script`). 200 of 646 tests run; the
run found and fixed an anode bug that made fragmented files unreadable,
plus ctime, truncate-time, FITRIM and timestamp-range bugs. 16 still fail:
missing features (atime, pre-1970 times, unwritten extents and holes,
norecovery, mkfs overwrite check, stable i_ino) and four open findings
(347/441 sticky I/O error flags, 483 fallocated space lost at a crash, 676
readdir at a foreign cookie). The full `-g auto` run followed (304 of 794
run, 34 failing; tests/HARDWARE-NOTES.md, "auto group") once the harness
gaps were closed. It found an upstream allocator bug that damaged ~1 in 25
filled volumes, a fragmented-space extend that crawled, and fallocate and
xattr metadata bugs, all fixed; so are generic/363 (EOF pollution) and
generic/062, 611 (empty xattr values are now allowed). Open: the four
findings above.

The original plan follows.

Run in a VM (needs a test and a scratch device, mkfs/fsck hooks for
`mkspadfs2`/`spadfsck2`, and `FSTYP=spadfs2`). Record the baseline pass/fail
list in `tests/HARDWARE-NOTES.md`; triage failures into real bugs vs
unsupported features (sparse, reflink, etc. will fail by design). Do this
before the extent-map redesign so that redesign has a regression baseline.

## Step 3 -- sparse files (design note, then decide)

Findings so far (from reading `file.c`, not yet written up):

- **Today's extent map** is contiguous from logical block 0: direct runs
  `blk1/blk1_n` and `blk2/blk2_n` in the fnode, then an anode tree
  (`struct anode`, 30 extents of `(blk, end_off)`; an `end_off` of -1 marks
  a pointer to a child anode). `spadfs_add_extent` and
  `spadfs_extend_last_extent` only ever append; truncation trims the last
  extent's `end_off` and lowers `valid_extents` in place.
- **Why that is crash-safe without versioning:** the fnode's two sizes
  (`size[2]`, picked by cc/txc) say how much of the map is committed;
  anything beyond is ignored. Truncated extents stay in the anode array past
  `valid_extents`, and after a crash `spadfs_get_blocks_to_resurrect` /
  `spadfs_do_extend(..., resurrect_blks)` re-extend the map to the committed
  size, because frees are transactional (apage halves are versioned), so the
  blocks are still allocated in the committed state.
- **Why holes break it:** filling a hole in the middle means splitting one
  extent into up to three below the committed size -- an in-place change
  the size versioning does not cover. After a crash the map could point at
  blocks whose allocation rolled back.
- **Most promising design: copy-on-write extent map with two root
  pointers.** Directories already keep two root pointers in the fnode
  (`run10/run11` and `run20/run21`, chosen by `CC_VALID`); a "tree-mode"
  file could do the same, with every mapping through anodes and holes as
  extents with `blk == 0` (sector 0 is never file data). A change below the
  committed size copies the affected anode path to new blocks and points
  the current root at the copy. **Key point:** freeing the old anodes
  immediately is already safe, because frees only take effect at commit
  (apage versioning) -- no deferred-free machinery is needed. Appends past
  the committed size can keep using the cheap in-place path.
- Alternative to evaluate: dual-versioned anodes (two halves with cc/txc,
  like apages and dnodes) -- simpler pointers, but halves the extent
  capacity and copies a whole anode on first change per commit.
- Scope to cost out: driver mapping/extend/truncate for tree mode,
  `fallocate` (punch hole, zero range), `SEEK_HOLE`/`SEEK_DATA` (iomap
  already reports holes), fiemap; spadfsck's file checker; crash tests that
  punch and fill holes; xfstests' sparse/fallocate groups as the acceptance
  test.

**Expected performance impact** (design-stage reasoning, nothing measured;
assumes the copy-on-write map with two roots, holes as `blk == 0` extents,
and the in-place append path kept):

- **Only files with holes use the new map ("tree mode").** Today a small
  file's first two extents live in its fnode, so reading it needs no
  metadata beyond the directory entry. Putting every file in tree mode would
  cost every cold file read at least one anode read and erase much of the
  small-file lead over ext4/XFS (40-90% on create/stat/unlink in
  `tests/HARDWARE-NOTES.md`). A file should switch on its first hole; dense
  files -- nearly all files -- then cost nothing.
- **Gains:** growing a file past EOF (`truncate -s`, seek-and-write) becomes
  one metadata update instead of writing the gap as zeros (today
  `generic_cont_expand_simple` writes every zero block); holes read as zeros
  without I/O; `SEEK_HOLE`/`SEEK_DATA` let `cp`, `tar`, `rsync` skip holes;
  VM images and database files can be created at full size without
  allocating it.
- **Costs, concentrated in random writes into holes:**
  - The first write into a hole within a commit copies the anodes on its
    path (1-3 blocks of 512 bytes, by tree depth); later writes to that path
    in the same commit are in place. So more metadata is written per commit
    when random writes touch many paths, but there are no extra flushes.
    ext4 pays similarly, writing changed extent blocks to its journal and
    then in place; copy-on-write writes them once.
  - Each random 4 KiB write into a hole is its own extent; at 30 extents per
    anode a scattered file grows a deeper tree, so cold lookups read more
    anodes (the per-inode extent cache hides this when warm).
  - No delayed allocation: blocks are allocated in write order, so randomly
    filled files scatter across the disk and later sequential reads slow
    down. Allocate near the logical neighbour's blocks to mitigate.
  - Punch hole and truncate also take the copy-on-write path: a few more
    metadata writes than today's in-place trim.
  - spadfsck time grows with extent count.
  - The two-versions-per-anode alternative would hold 15 extents per
    version instead of 30, deepening trees sooner -- a reason to prefer
    copy-on-write.
- **Benchmark phases to add to `tests/bench_ab.sh`** (against ext4 and XFS):
  random 4 KiB writes into a large sparse file with fsync (database/VM
  pattern -- where the costs show); `truncate -s` up to a large size (where
  the gain should be large); `cp --sparse` of a mostly empty file; and the
  existing small-file phases re-run to prove the dense fast path did not
  regress.

Write this up as `docs/superpowers/plans/<date>-phase6-sparse-files.md` with
the options, the crash argument for the chosen one, the on-disk changes in
all three readers, the tests, the performance impact above, and an effort
estimate -- then decide with the operator whether to build it.

---

## Open performance questions (from the benchmarks, commit `9d0b0b1`)

Added after the review. The side-by-side rig now exists:
`tests/build_reference.sh` builds v1.0.22 as `spadfs`, and `tests/bench_ab.sh`
compares any number of filesystems on one scratch device (root; it erases
the device). Results so far are in `tests/HARDWARE-NOTES.md`. On the
operator's laptop SATA disk, at 20 000 files per directory, spadfs2 was
40-90% faster than ext4 and XFS at create/stat/unlink, matched XFS on commit
and single-writer fsync, and trailed XFS by 14% on 8-writer fsync. These
questions remain; none blocks Steps 1-3, but re-run the comparison after
Step 1 if it adds ordered writeback, since that could cost part of the
commit and fsync advantage.

1. **Create regression that grows with directory size.** Against v1.0.22,
   spadfs2's create costs +3 us per file at 20 000 files and +17 us at
   200 000 (+30%: 11.4 -> 14.8 s), so it is not fixed per-file CPU work
   (CRCs, SipHash). Leading hypothesis, unconfirmed: building a large
   directory forces commits partway through (e.g. `NEED_SYNC` from
   `spadfs_add_fnode_to_directory`), and each now costs one more flush
   (~12 ms); ~280 of them would explain the 3.4 s. To confirm, count flush
   requests per phase -- the block layer's diskstats flush field reads 0
   on this kernel, so use tracing (`block:block_rq_issue`, flush requests
   have F in rwbs) -- or count commits in the driver. Compare v1.0.22 and
   spadfs2 with `tests/bench_ab.sh -f 200000`.
2. **Large directories against ext4 and XFS -- answered.** At 200 000 names
   spadfs2 keeps the fastest median create, but the lead narrows to 15-22%
   (ext4's best round beat it); unlink and 1-writer fsync leads hold. Two
   new findings to explain (tests/HARDWARE-NOTES.md): ext4's cold stat is
   27% faster at this size (wide spread), and spadfs2's sequential read is
   5-7% behind after the small-file churn -- check the big file's placement
   with `filefrag -v` under each filesystem.
3. **Fast storage and many cores.** On the HDD every writer mostly waits on
   an ~11 ms flush; on NVMe, lock contention dominates instead, and that is
   where spadfs2's single filesystem-wide `sync_lock` (commits take it
   exclusively) should fall further behind XFS's allocation groups than the
   14% seen on 8-writer fsync. Needs an erasable NVMe scratch partition, and
   probably a higher writer count (`fsync_rate` in bench_ab.sh is
   parameterised by writer count). Aged, fragmented volumes are also
   untested: every round starts from a fresh mkfs.
