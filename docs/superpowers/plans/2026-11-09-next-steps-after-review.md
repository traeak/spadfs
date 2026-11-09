# Next steps after the 2026-11-09 design review

**Status: planned, not started.** Written at the end of the session that
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
2. **File data after a crash -- suspected, not yet tested.** A commit
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

Write this up as `docs/superpowers/plans/<date>-phase6-sparse-files.md` with
the options, the crash argument for the chosen one, the on-disk changes in
all three readers, the tests, and an effort estimate -- then decide with the
operator whether to build it.
