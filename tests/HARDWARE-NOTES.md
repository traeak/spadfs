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
