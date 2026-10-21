# SpadFS Modernization — Roadmap and Phase 0 Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Modernize SpadFS's weakest subsystems in this repository, validated
against a pinned reference implementation of the original v1.0.22 running
side-by-side on the same host.

**Architecture:** Classify each weakness by **what a change costs**, not by
severity. Because backwards compatibility is explicitly not required, the
`flags_compat_*` negotiation that would otherwise dominate every change is
**out of scope** — format changes are free. What replaces it as the controlling
concern is the **reference rig**: every modernization must be measurable against
the unmodified v1.0.22 on identical workloads.

**Tech stack:** C (Linux kernel module + userspace tools), shell harness,
`SPADFS_NAME=` build parameterization for parallel module instances, GitHub Actions.

**Spec:** [`DESIGN-ANALYSIS.md`](../../DESIGN-ANALYSIS.md) — *Ranked weaknesses*.

---

## Decisions taken (these shape everything below)

| Question | Answer | Consequence |
|---|---|---|
| Upstream or fork? | **This repo is the upstream.** No fork. | No governance blocker. Changes land here directly. |
| Must v1.0.22 volumes keep mounting? | **No.** | Compat-flag gating abandoned. Real fixes unlocked, including widening `cc`. |
| What *is* required? | **Comparability to a reference implementation.** | The reference rig becomes a first-class Phase 0 deliverable and the acceptance test for every later phase. |

**What "no backwards compat" does *not* mean:** it does not mean the old and new
stacks can read each other's images. They cannot. Comparability is therefore at
the **operation and workload level** — same sequence of operations, old stack vs
new stack, compare correctness and timing — not at the image level.

---

## The reference rig: already supported by the build

`Kbuild` takes a `SPADFS_NAME` parameter that flows into the registered
filesystem type and every slab cache name:

```c
.name = __stringify(SPADFS_NAME);                                  /* super.c:1931 */
kmem_cache_create(__stringify(SPADFS_NAME) "_inode_cache", ...);   /* super.c:2000 */
kmem_cache_create(__stringify(SPADFS_NAME) "_ea_cache", ...);      /* super.c:2013 */
kmem_cache_create(__stringify(SPADFS_NAME) "_extent_cache", ...);  /* super.c:2026 */
```

Verified this session — `make SPADFS_NAME=spadfsref` produces a module with
`alias: fs-spadfsref` and caches `spadfsref_*`, disjoint from the main build's
`spadfs_*`. **Both modules load simultaneously on one host**, and volumes mount
as `-t spadfs` vs `-t spadfsref`.

**Caveat that must be designed around:** the userspace tools are **not**
name-parameterized. There is one `mkspadfs` and one `spadfsck`, each bound to
whatever format its source tree produces. A paired reference toolchain must be
built from the pinned reference tree into a separate output directory, or the
comparison is meaningless (modern `mkspadfs` output cannot be mounted by
`spadfsref.ko`).

---

## Revised ranking under "no backwards compat"

Two items move sharply. The rest hold.

- **#6 (crash-count ceiling) is now a real fix, not a workaround.** Previously
  constrained to fsck recycling because widening `cc` breaks every structure.
  With format freedom, `cc` can go 16 → 32 bits. Cost: `struct fnode`,
  `apage_head`, `dnode_page`, `fnode_block`, `txblock` all carry `cc`, so it
  touches every on-disk structure — but it is a *mechanical* change, and it
  removes the ceiling permanently (2^32 mounts).
- **#1 (checksum) is a straight replacement, not a negotiated migration.** No
  dual-algorithm period, no compat bit. Replace `__byte_sum` with crc32c
  everywhere and measure the detection-rate gain against the Phase 0 baseline.
- **#4 (hash quality) becomes cheap to evaluate.** The reference rig answers the
  "is it actually clustering?" question directly: same directory workload, old
  hash vs new hash, compare lookup counts and timing. No more speculation.

---

## Roadmap

| Phase | Scope | Format impact | Depends on |
|---|---|---|---|
| **0** | Test harness + reference rig | None | — |
| **1** | crc32c metadata checksums | Full replace | 0.3 baseline |
| **2** | cct backup table | Additive | 0.1, 1 |
| **3** | Widen `cc` 16→32 | Every structure | 0, 2 |
| **4** | Hash function + clustering measurement | Layout-derived | 0.4 rig |
| **5** | Symlink cap, sparse files | New structures | 1–3 |

Each phase is its own plan. **Only Phase 0 is written in full here** — it is the
prerequisite for all others and carries no format risk.

### Still not planned

**#3** (size in directory block), **#5** (no ordered iteration), **#9** (no
`open_by_handle_at`) remain consequences of the two founding bets. "No backwards
compat" does not make these cheap — they are redesigns of the inode-embedding and
indexing model, not format tweaks. Revisit only if a concrete workload demands it.

---

# Phase 0: Harness and Reference Rig

**Goal:** an unprivileged, scripted round-trip suite that (a) creates and checks
volumes, (b) measures structured-corruption detection, and (c) runs the pristine
v1.0.22 side-by-side with the working tree as a comparison oracle.

**Why first:** the repo has **zero** test infrastructure today (verified: no
`test/`, no kunit, only `#ifdef TESTCODE` hooks in spadfsck). Every later phase
is unverifiable without it, and the reference-comparison requirement cannot be
met at all without 0.4.

**Key capability verified:** the full create/check cycle runs **without root,
without loop devices, and without loading the module**:

```bash
dd if=/dev/zero of=/tmp/t.img bs=1M count=64
./mkspadfs --block-size 4096 --no-trim /tmp/t.img   # exit 0
./spadfsck -f /tmp/t.img                            # exit 0, clean
```

Note `mkspadfs` opens with `O_RDWR|O_EXCL` and **no `O_CREAT`** — the image
must be pre-created.

**Privileged tier (out of scope here):** mount, fsx, fio, crash-injection.
Those need root and a second tier of CI runner. 0.4's module comparison needs
`insmod`, so it is marked privileged and skipped when unavailable.

---

### Task 0.1: Fixture generator

**Files:** `tests/lib.sh`, `tests/fixtures.sh`

**Interfaces:**
- Produces: `make_image <path> <size> [opts...]` → 0; `REPO_ROOT`; `require_tools`.

- [ ] **Step 1:** `tests/lib.sh` — `REPO_ROOT` resolution, `log()`/`die()`,
  `require_tools` asserting `./mkspadfs` and `./spadfsck` exist (build products;
  fail with "run make" if absent).
- [ ] **Step 2:** `make_image` in `tests/fixtures.sh` — `dd` a file filled with a
  **non-zero pattern** (not sparse zeros — see Review Focus #4), then `mkspadfs
  --no-trim "$@"`, assert exit 0.
- [ ] **Step 3:** Smoke: `make_image /tmp/x.img 64M` → exit 0 and
  `spadfsck -n /tmp/x.img` clean.
- [ ] **Step 4:** Commit `test: add fixture generator for unprivileged volume creation`

### Task 0.2: Round-trip matrix

**Files:** `tests/roundtrip.sh`

- [ ] **Step 1:** Matrix over the options that change on-disk layout:
  `--block-size {512,4096}`, `--unix-names`/`--spad-names`,
  `--big-endian`/`--little-endian`, `--dynamic-magic`, `--directory-sizes`,
  `--checksums`/`--no-checksums`, `--cluster-size`/`--cluster-threshold`.
- [ ] **Step 2:** Assert `spadfsck -f` exits 0 on every freshly created volume.
  Non-zero on a virgin image is a bug worth surfacing now.
- [ ] **Step 3:** Run; all combinations PASS.
- [ ] **Step 4:** Commit `test: add mkspadfs/spadfsck round-trip matrix`

### Task 0.3: Structured corruption + detection baseline

**Files:** `tests/corrupt.py`, `tests/detect.sh`, `tests/BASELINE-checksum-detection.md`

The current checksum is an 8-bit XOR fold (`__byte_sum`, `struct.h:947`).
XOR is commutative and associative, so it is **provably blind** to word swaps,
word duplication, and block copies — the classes real hardware produces. It
detects single-bit flips 100%. Random-bit testing would show ~100% and give a
false all-clear.

- [ ] **Step 1:** `tests/corrupt.py` classes: `bitflip`, `word_swap`, `word_dup`,
  `block_copy`, `byte_transpose`, `xor_cancel`, `zero_out`.
- [ ] **Step 2:** `tests/detect.sh` — for each class × each metadata target
  (superblock, txblock, cct, fnode block, dnode, apage), corrupt a copy and
  record whether `spadfsck -n` exits non-zero. Run every class **twice**: once
  with checksums on, once `--no-checksums`.
- [ ] **Step 3:** Write the matrix to `tests/BASELINE-checksum-detection.md`.
  For any class showing high detection, record **which layer** caught it —
  checksum vs structural validation (magic, range, `cc`/`txc` ordering).
  Classes caught only structurally are unprotected under `--no-checksums`.
- [ ] **Step 4:** Commit `test: add structured corruption harness and checksum baseline`

### Task 0.4: Reference rig ← *the answer to the comparability requirement*

**Files:** `tests/reference.sh`, `Makefile` (modified)

- [ ] **Step 1:** Pin the reference. Tag the pristine v1.0.22 import:
  `git tag -a reference-v1.0.22 59e33f2 -m "..."`. This is the comparison
  baseline; it must never move. (Note: `59e33f2` already carries the minimal
  `strncpy`/include build fixes — a truly pristine tree does not compile on
  modern kernels, so this *is* the usable reference.)
- [ ] **Step 2:** Build the reference into an isolated dir:
  `git worktree add /tmp/spadfs-ref reference-v1.0.22`, then
  `make -C /tmp/spadfs-ref SPADFS_NAME=spadfsref` and build its own
  `mkspadfs`/`spadfsck` there. Verified: produces `spadfsref.ko` with
  `alias: fs-spadfsref` and disjoint slab caches.
- [ ] **Step 3:** `tests/reference.sh` — a workload driver that runs the **same
  scripted operation sequence** against both stacks (privileged: `insmod`,
  `mkspadfs`, `mount -t spadfs` / `-t spadfsref`), then compares:
  operation exit codes, `spadfsck` cleanliness, and wall-clock timing.
  Skip cleanly with a notice if not root.
- [ ] **Step 4:** Add `make test-ref` target wrapping Step 3.
- [ ] **Step 5:** Commit `test: add side-by-side reference rig using SPADFS_NAME`

### Task 0.5: Make target and CI

**Files:** `Makefile`, `.github/workflows/test.yml`

- [ ] **Step 1:** `make test` → 0.2 + 0.3, **no root required**.
- [ ] **Step 2:** CI job on `ubuntu-latest`: `make`, `make test`, upload the
  baseline table as an artifact. `make test-ref` is a separate,
  `allow_failure: true` privileged job (GitHub-hosted runners cannot `insmod`).
- [ ] **Step 3:** Verify `make test` passes locally end to end.
- [ ] **Step 4:** Commit `ci: run unprivileged spadfs test suite on push`

---

## Review Focus

1. **Silent kernel/fsck divergence.** Every metadata structure is parsed twice —
   in the driver and in `utils/spadfsck/`. Changing one without the other is
   invisible until a crash. *Mitigation: every phase must change both in the same
   commit; 0.2's matrix is the only unprivileged cross-check.*
2. **Checksum-disabled volumes.** `--no-checksums` sets
   `FLAG_COMPAT_FSCK_NO_CHECKSUMS`. Detection rates measured only with
   checksums on overstate real protection. *0.3 runs every class under both.*
3. **Reference rig false equivalence.** If the modernized `mkspadfs` is used to
   create a volume mounted by `spadfsref.ko` (or vice versa), results are
   garbage, not comparable. *0.4 Step 2 must keep each stack's toolchain paired
   and isolated; assert the fs type on every mount.*
4. **Sparse-image false negatives.** `dd` sparse files read back as zeros, so a
   zero-writing corruption is indistinguishable from untouched space.
   *0.1 Step 2 uses a non-zero fill pattern.*
5. **Backup-table desync.** If a mirrored cct (Phase 2) is not updated on every
   commit it becomes worse than no backup: a stale copy that validates.
   *Phase 2 design requirement, not a Phase 0 test.*
6. **Widening `cc` changes struct sizes and thus all offsets.** Phase 3 touches
   `fnode`, `apage_head`, `dnode_page`, `fnode_block`, `txblock`. Any missed
   `FNODE_*_POS`-style derived constant silently corrupts layout.
   *Re-run 0.2's full matrix after Phase 3, and diff `sizeof()` assertions —
   `spadfs_struct_check_correctness()` in mkspadfs already does some of this.*

---

## Next step

Phase 0 is unblocked and should be executed first — it is format-neutral, mostly
unprivileged, and it is the only thing that makes every later phase measurable
rather than argued.

Phases 1–5 each need their own plan, written after Phase 0 produces the baseline
numbers. In particular **Phase 4 should not be designed before 0.4's rig can
answer whether the current hash actually clusters** — that measurement may make
the whole item moot, or may show it is worse than assumed.
