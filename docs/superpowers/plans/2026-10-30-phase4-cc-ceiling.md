# Phase 4: The Crash-Count Ceiling

**Status: DONE 2026-10-30 (unprivileged half verified; mount test pending a root run).**
**Format impact: none.**

## The roadmap was wrong about this

The roadmap planned to widen `cc` from 16 to 32 bits as "a *mechanical*
change". It is not: `cc` is the **index** into the crash-count table.
`CC_VALID(cc, txc)` reads `cct[cc]`, and the table is sized for every
possible `cc` (`CCT_SIZE = 65536 * 4`, marked `/* do not change it */`). A
32-bit `cc` needs a 2^32 × 4 B = **16 GiB** table. Widening means replacing the
direct-indexed table with a windowed or sparse structure, which redesigns
the commit mechanism. It is not a field resize.

## The ceiling is further away than assumed

`cc` does not advance per mount. A read-write mount increments `tx.cc`
(`spadfs_increase_cc(fs, 1)`) and a clean unmount or remount read-only
steps it back (`super.c`, `spadfs_increase_cc(fs, -1)`). Headroom is lost
only to:

- an **unclean shutdown** (the increment is never undone), or
- `cct[cc]` exhausting 2^31 transactions (`spadfs_start_new_tx`).

So the limit is 65,535 crashes, not 65,535 mounts. At 0xffff the kernel
mounts read-only, and `spadfsck --reset-crash-counts` recovers the volume
offline by rewriting every structure's `cc/txc` and zeroing the cct.

## Decision

Keep 16-bit `cc` and make the existing wrap path visible and tested.
Decided with the user 2026-10-30. Rejected alternatives: online recycling
(format-affecting, complex) and a 32-bit `cc` with a new table (the redesign
above).

## What changed

| Where | Change |
|---|---|
| `struct.h` | `CC_WARN_THRESHOLD = 0xff00`: a shared policy constant, not an on-disk field |
| `utils/spadfsck/sck.h` | `MAX_CC` is now `CC_WARN_THRESHOLD` (same value as before), so fsck and kernel cannot drift |
| `super.c` | Read-write mount at or above the threshold logs the crash count and the exact headroom left. The wrap error names the recovery command. The `FIXME` is replaced by why the wrap is handled offline. |
| `tests/cc_wrap.sh` | Unprivileged, in `make test`. Covers: the threshold boundary; `-n` stays read-only; `-y` resets `tx.cc` and the whole cct; a forced re-check is clean; the reset is idempotent. 10/10. |
| `tests/cc_wrap_mount.sh` | Root. Covers: no warning below the threshold; warning with exact headroom; a clean unmount costs no headroom; read-only at 0xffff with recovery text; reset → read-write → data survives a remount. **Not yet run.** |

fsck already had the right behaviour (`HIGH CRASH COUNT REACHED` and an
offered reset from 0xff00). Nothing tested it until now.
