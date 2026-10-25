# Phase 2 — Metadata integrity: 32-bit CRC

Status: **DRAFT — needs one design decision before execution.**
Depends on: Phase 0 harness (done), Phase 1 identity switch (done), kernel floor 6.1 (done).

---

## Why, from the Phase 0 evidence

| Measured | Value |
|---|---|
| `__byte_sum` is | XOR of every byte in the block |
| Escape rate, random multi-byte corruption | **1/256** (measured 0.00388 vs predicted 0.00391) |
| Byte permutations | **invisible by proof** (200 000 trials, zero changed the sum) |
| What actually catches corruption today | structural validation, not the checksum |
| cct coverage | **none at all** |

Goal: 32-bit CRC → escape rate 1/4 294 967 296, real burst-error distance, and
coverage extended to the cct.

---

## The design decision

Every metadata unit is 512 bytes. A 32-bit checksum costs 4 bytes that must come
from somewhere. Two viable layouts:

### Option A — widen the existing field in place

Replace `__u8 checksum` with `__u32 checksum` in each struct.

* Header grows: `fnode_block` 24 → 28, so `FNODE_MAX_SIZE` 488 → 484.
* **Breaks the hardcoded `SIZEOF_FNODE_BLOCK (6 * 4)`** and therefore the
  `make_dir_off_t` / `dir_off_t_pos` bit-packing that encodes directory
  positions relative to it. Those must be recomputed and re-verified together.
* Touches 8 struct definitions.

### Option B — uniform 512-byte sector trailer  ← recommended

Last 4 bytes of every 512-byte metadata unit hold `crc32c(unit[0:508])`.
The per-struct `__u8 checksum` field is removed.

* **Header offsets do not change at all** — `SIZEOF_FNODE_BLOCK` stays 24, the
  dir_off_t bit-packing stays valid.
* Checksum location is *positional*, not per-struct, so **one helper covers
  every metadata unit including the cct**, which currently has no checksum and
  no natural field.
* Payload still shrinks 4 bytes per unit (488 → 484), same cost as A.
* Removes the "flags & checksum must match struct fnode_block" cross-struct
  coupling rather than preserving it.

**Recommendation: B.** Same payload cost, but it avoids the single riskiest
thing in this phase — desyncing a hardcoded size macro from a struct that is
bit-packed into on-disk directory offsets. It also makes cct coverage a
one-liner rather than a new field.

**Cost to accept either way:** 4 bytes per 512-byte metadata unit. That is
~0.8% of metadata capacity. Confirm this is acceptable before starting.

---

## Work breakdown

### 2.1 — Checksum abstraction (kernel + shared)

Add one pair of helpers and delete the raw pattern:

```c
void  spadfs_meta_seal(void *unit512);     /* zero trailer, crc over 508, store */
int   spadfs_meta_verify(const void *unit512); /* 0 = good */
```

* Use `crc32c()` from `<linux/crc32c.h>` — hardware-accelerated on x86/ARM64,
  and available at the 6.1 floor.
* Userspace gets a matching implementation in `utils/` (no kernel headers
  there), so mkfs/fsck and the module must agree bit-for-bit.
* **Verify the two implementations agree before anything else** — a mismatch
  here silently breaks every volume.

Replaces ~20 XOR-update sites.

### 2.2 — Convert all call sites

~45 sites across:

| Area | Files |
|---|---|
| kernel | `super.c`, `bufstruc.c`, `dir.c`, `file.c` |
| mkfs | `utils/mkspadfs/mkspadfs.c` |
| fsck | `scksuper.c`, `sckdir.c`, `sckfxfn.c`, `sckfile.c`, `sckfblk.c`, `sckrcv.c`, `sckcrt.c`, `sckmass.c`, `sckapage.c` |

Note `bufstruc.c` is the hot read path — verify no measurable regression.

### 2.3 — Cover the cct

With the trailer model this is applying `spadfs_meta_seal/verify` to each
512-byte cct sector. Directly addresses the finding that `zero_out` on
`cct[tx.cc]` silently flips `CC_VALID`.

### 2.4 — Close the hardcoded-size hazard

Independent of which option is chosen:

* Replace `#define SIZEOF_FNODE_BLOCK (6 * 4)` with a compile-time assertion
  that it equals `sizeof(struct fnode_block)`, so it can never silently drift.
* Extend `spadfs_struct_check_correctness()` (currently only checks
  `dnode_page_entry == 6`) to cover every on-disk struct size that is
  hardcoded somewhere. This is the tree's only ABI guard and it is thin.

### 2.5 — Prove the new checksum actually buys detection

This is the part that matters and the reason the restripe experiment exists.

A wider CRC can still be masked by structural checks, so measuring "fsck now
detects more" is meaningless unless the structural layer is held constant.

Extend `tests/corrupt.py` with a **restripe mode**: corrupt the payload, then
recompute the trailer so the checksum *matches the corrupted bytes*. Any
remaining detection is purely structural; any loss of detection proves the
checksum was the only thing catching it.

Then re-run the full matrix and compare against
`tests/BASELINE-checksum-detection.md`:

| | before | after |
|---|---|---|
| XOR-preserving classes (word_swap, byte_transpose, xor_cancel) | caught structurally or not at all | must be caught by CRC |
| 1/256 escape rate | measured | ~1/4.3e9 |
| cct silent corruption | **3 cases** | must be 0 |

**Acceptance gate:** `tests/cct_integrity.sh --strict` must pass (it currently
exits 1 with 3 silent corruptions). That is the hard, pre-registered criterion.

---

## Suggested commit sequence

1. `feat(compat): add spadfs_meta_seal/verify with crc32c` — helpers + userspace twin + agreement test
2. `chore(abi): assert SIZEOF_FNODE_BLOCK == sizeof(struct fnode_block)` — 2.4, do this **before** touching layout
3. `refactor: convert metadata checksums to 32-bit trailer` — 2.1/2.2
4. `feat: checksum the crash-count table` — 2.3
5. `test: restripe mode + updated detection baseline` — 2.5

Each step keeps the tree building and `make test` green.

---

## Risks

| Risk | Mitigation |
|---|---|
| Kernel/userspace CRC mismatch → every volume silently corrupt | 2.1 agreement test gates everything |
| `SIZEOF_FNODE_BLOCK` desync breaks dir offsets | do 2.4 **first**; compile-time assert |
| Hot-path regression in `bufstruc.c` | measure before/after on the read path |
| 4-byte payload loss breaks a size-dependent invariant | `FNODE_MAX_EA_SIZE` and friends are derived — recheck all |
| New CRC masked by structural checks, gain is illusory | 2.5 restripe experiment is the whole point |

---

## Open question for the operator

**Option A or B?** I recommend **B (uniform trailer)** because it leaves the
bit-packed directory offsets untouched and makes cct coverage trivial. The
choice is load-bearing for steps 2.2–2.4 and should be settled before code.

Also confirm the **4-byte-per-unit payload cost** is acceptable, since both
options pay it.
