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
| How is the new stack identified? | **As `spadfs2`** — a separate filesystem type, with `mkspadfs2` / `spadfsck2`. | The type name *replaces* the `flags_compat_*` machinery as the compatibility boundary. `main` stays pristine as the reference. |

**What "no backwards compat" does *not* mean:** it does not mean the old and new
stacks can read each other's images. They cannot. Comparability is therefore at
the **operation and workload level** — same sequence of operations, old stack vs
new stack, compare correctness and timing — not at the image level.

---

## The `spadfs2` identity

The modernized stack is a **separate filesystem type**, not a version of
`spadfs`. `main` keeps building `spadfs` / `mkspadfs` / `spadfsck` untouched as
the reference; `modernization` builds `spadfs2` / `mkspadfs2` / `spadfsck2`.

This replaces the `flags_compat_*` machinery with a stronger boundary: an old
`spadfs.ko` cannot mount a `spadfs2` volume at all — wrong filesystem type — so
no compat bits, no dual-algorithm migration windows, and no code whose only
purpose is "don't misread the old format."

**Verified this session.** `Kbuild`'s `SPADFS_NAME` flows into the registered
type and every slab cache name:

```c
.name = __stringify(SPADFS_NAME);                                  /* super.c:1931 */
kmem_cache_create(__stringify(SPADFS_NAME) "_inode_cache", ...);   /* super.c:2000 */
kmem_cache_create(__stringify(SPADFS_NAME) "_ea_cache", ...);      /* super.c:2013 */
kmem_cache_create(__stringify(SPADFS_NAME) "_extent_cache", ...);  /* super.c:2026 */
```

`make SPADFS_NAME=spadfs2` yields `alias: fs-spadfs2`, `name: spadfs2`, and
`spadfs2_{inode,ea,extent}_cache` — disjoint from `spadfs_*`. There are **zero
`EXPORT_SYMBOL`s in the tree**, so the thousands of internal `spadfs_*` function
names cannot collide either. **No C symbol renaming is required.** Both modules
load simultaneously on one host, and because the userspace tools are renamed too,
**both full stacks install side-by-side permanently** — no worktree juggling and
no output-directory isolation.

### The format discriminator

Of the eight magic constants, only `SUPERBLOCK_SIGNATURE "FS SPAD\000"`
(`struct.h:68`; written `mkspadfs.c:459`, checked `super.c:1197` and
`scksuper.c:121`) is a stable identifier. The rest — `TXBLOCK_MAGIC`,`FNODE_BLOCK_MAGIC`, `DNODE_PAGE_MAGIC`, `ANODE_MAGIC` — pass through
`magic(block, m) = m ^ (dynamic_magic ? block : 0)` (`mkspadfs.c:148`), and
`dynamic_magic` **defaults to 1** (`mkspadfs.c:89`), so they are already
per-volume randomized and are not identifiers. Changing `SUPERBLOCK_SIGNATURE` to
`"FS SPAD2\0"` is therefore the single decisive break.

**Sequencing:** do *not* change the signature during Phase 0. Phase 0 must
compare two format-identical stacks to establish behavioral equivalence first.
The signature change is the deliberate fork point and belongs at the head of
Phase 1.

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
| **0** | Test harness + side-by-side reference rig | None | — |
| **1** | **Identity switch** — `spadfs2` name, signature, version reset, Makefile | The fork point | 0 |
| **2** | crc32c metadata checksums | Full replace | 1, 0.3 baseline |
| **3** | cct backup table | Additive | 2 |
| **4** | Widen `cc` 16→32 | Every structure | 3 |
| **5** | Hash function + clustering measurement | Layout-derived | 0.4 rig |
| **6** | Symlink cap, sparse files | New structures | 2–4 |

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

### Task 0.4: Side-by-side reference rig ← *the answer to the comparability requirement*

**Files:** `tests/reference.sh`, `Makefile` (modified)

With the `spadfs2` rename, the rig no longer needs worktrees or isolated output
directories — the two stacks have disjoint module names, fs types, slab caches,
and tool names, so **both install permanently on one host**.

- [ ] **Step 1:** Pin the reference. `git tag -a reference-v1.0.22 59e33f2 -m "..."`.
  This is the comparison baseline and must never move. (Note: `59e33f2` already
  carries the minimal `strncpy`/include build fixes — a truly pristine tree does
  not compile on modern kernels, so this *is* the usable reference.)
- [ ] **Step 2:** Parameterize this tree's build on the identity so
  `make SPADFS_NAME=spadfs2` also produces `mkspadfs2` / `spadfsck2` (see
  Phase 1 preview). For Phase 0 the *format* stays identical — only names differ.
- [ ] **Step 3:** Install both stacks: reference `spadfs.ko` + `mkspadfs` +
  `spadfsck` from the tag, and `spadfs2.ko` + `mkspadfs2` + `spadfsck2` from
  this tree. Assert `lsmod` shows both and `cat /proc/filesystems` lists both
  types.
- [ ] **Step 4:** `tests/reference.sh` — run the **same scripted operation
  sequence** against each stack (`mkfs` → `mount -t <type>` → create/stat/
  rename/unlink workload → `umount` → `fsck`), comparing operation exit codes,
  fsck cleanliness, and wall-clock timing. Privileged; skip cleanly with a notice
  when not root.
- [ ] **Step 5:** Add `make test-ref` wrapping Step 4.
- [ ] **Step 6:** Commit `test: add side-by-side spadfs/spadfs2 reference rig`

### Task 0.5: Make target and CI

**Files:** `Makefile`, `.github/workflows/test.yml`

- [ ] **Step 1:** `make test` → 0.2 + 0.3, **no root required**.
- [ ] **Step 2:** CI job on `ubuntu-latest`: `make`, `make test`, upload the
  baseline table as an artifact. `make test-ref` is a separate,
  `allow_failure: true` privileged job (GitHub-hosted runners cannot `insmod`).
- [ ] **Step 3:** Verify `make test` passes locally end to end.
- [ ] **Step 4:** Commit `ci: run unprivileged spadfs test suite on push`

---

## Findings from Phase 0 execution

Produced by `tests/detect.sh` (see `tests/BASELINE-checksum-detection.md`) and
follow-up source inspection. All verified, not inferred.

### 1. `spadfsck -n` short-circuits

On a volume with no recorded error flag it prints "filesystem is clean, no
checking needed" and exits 0 **without traversing**. Every detector in this
harness must use `-n -f`. A baseline built on bare `-n` reports the entire
filesystem as blind — it is not.

### 2. `__byte_sum` is literally the XOR of every byte in the block

`struct.h:947` XORs 8-byte words, then folds `>>32 >>16 >>8` to the low byte.
Verified equivalent to a plain byte-wise XOR over 5 000 random 512-byte blocks.
Three consequences:

* **Any permutation of bytes within a block is invisible to it.** Swapping two
  adjacent bytes yields a delta of `(x^y) | ((x^y)<<8)`, whose byte XOR is
  `(x^y) ^ (x^y) = 0`. Confirmed over 200 000 random trials: zero produced a
  changed sum. This is not a probability, it is a proof — and it applies to
  *every* reordering, not just adjacent ones.
* **It is 8 bits wide.** Measured escape rate for random multi-byte corruption:

  | bytes changed | escape rate | predicted 1/256 |
  |---|---|---|
  | 1 | 0.00000 | — (always caught) |
  | 2 | 0.00388 | 0.00391 |
  | 4 | 0.00394 | 0.00391 |
  | 8 | 0.00388 | 0.00391 |
  | 16 | 0.00365 | 0.00391 |
  | 64 | 0.00394 | 0.00391 |

  A single corrupted byte is always caught. Beyond that it is a coin flip at
  1-in-256. No Hamming-distance property, no burst-error protection — it is not
  a CRC in any useful sense.
* **Structural validation is load-bearing.** Many permutation-invisible
  corruptions were still caught — by magic checks, field-range checks and
  cross-references, not by the checksum. `word_swap` and `xor_cancel` came out
  "DETECTED" on every target despite `xor_delta = 0x00`. fsck looks better than
  its checksum alone deserves, and that balance is fragile.

  This is the actual argument for Phase 2: not "the checksum misses things" but
  "the checksum is doing almost none of the work, and when a corruption is both
  XOR-preserving *and* structurally plausible, nothing fires."

### 3. The crash-count table has no checksum — and the consequence is semantic

`super.c:722` writes the cct sector with a plain `spadfs_memcpy`; there is no
`__byte_sum` or equivalent anywhere on that path, unlike the superblock
(`super.c:507`) and the fnode blocks. Combined with having no backup copy,
**the single most load-bearing structure in the filesystem — every `CC_VALID()`
decision routes through it — is the only one with neither a checksum nor
redundancy.**

This is no longer just a source-reading claim. `tests/cct_integrity.sh` primes a
volume to a plausible post-mount state (`tx.cc=4`, `cct[cc]=1776`) and corrupts
the live entry in place:

| Class | `cct[cc]` after | delta | `CC_VALID` | fsck |
|---|---|---|---|---|
| bitflip | 1777 | +1 | True | **silent** |
| byte_transpose | 61446 | +59 670 | True | **silent** |
| zero_out | 0 | −1776 | **False** | **silent** |
| word_swap | 0 | −1776 | False | detected (rc=4) |
| word_dup | 1776 | 0 | True | detected (rc=4) |
| xor_cancel | 1777 | +1 | True | detected (rc=4) |

Two things to read off this table:

* **`zero_out` silently inverts `CC_VALID` from True to False.** The core
  validity predicate of the filesystem changes its answer and `spadfsck -n -f`
  reports the volume clean. That is not a cosmetic gap.
* **The detections that do happen are not the checksum's doing.** They come from
  the ordering rule in `sckcct.c:45-56` — entries *above* `tx.cc` must be zero.
  Any class that spills above `cc` trips that rule; any class confined to
  `cct[cc]` itself is invisible. Confirmed by the pinned-anchor runs: every
  silent case landed at byte 8 486 928, exactly `cct_base + cc*4`.

So the cct's only integrity protection is a bound that guards the wrong region.

### 4. A correction: the "5 undetected classes" were a harness bug

An earlier revision of this document reported five classes as undetected
(`byte_transpose` on superblock/txblock/apage_index, `block_copy` on
txblock/apage_index). **That was wrong and the baseline has been regenerated.**

Cause: the corruptor searched forward up to 64 units for "live" bytes without
bounding itself to the target structure. Several of these structures are far
smaller than that — the txblock is one 512-byte sector with only **5 non-zero
bytes** on a fresh volume, and `sb.root` points at the root *fixed fnode*, not
the root directory. The search therefore landed in neighbouring slack space
(e.g. a "txblock" corruption at sector 16408, which is `apage_index1`) where
fsck correctly has nothing to verify. The misses were measuring the wrong
address, not the wrong property.

Fixed by deriving real extents from `mkspadfs.c:331-339` into
`corrupt.target_region()`, splitting `root_fnode` from `root_dir`, and adding a
pinned-anchor mode (`--at`) so a target cannot drift. Retained here because the
failure mode is easy to repeat and worth naming: **a corruption harness that
cannot prove *where* it wrote is worse than no harness.**

### 5. Priming makes the cct testable without root

The table is all zeros on a fresh volume, so every class used to return NOOP and
the finding above could not be exercised. `tests/prime.py` solves this without
mounting:

* sets `tx.cc` in the txblock and **restripes the txblock checksum** so the
  volume stays valid — the invariant is `XOR(all 512 bytes) == CHECKSUM_BASE`
  (`'C'`, `struct.h:29`), derived from `tb->checksum ^= CHECKSUM_BASE ^
  __byte_sum(tb, 512)` at `super.c:507`
* writes plausible counts into `cct[0..cc]` that satisfy the fsck rules: top
  bit clear at or below `cc`, zero above it
* the test asserts the primed volume is still clean before corrupting it, so a
  fixture bug cannot masquerade as a filesystem finding

The same restripe trick is the general answer to isolating checksum-vs-structural
coverage on any SpadFS block, and should be used in Phase 2 to verify that a new
checksum actually buys something rather than being masked by structural checks.

---

## What Phase 2 Should Actually Fix

Ranked by the evidence above, not by intuition:

1. **Widen the checksum.** 8 bits → 32 bits changes the multi-byte escape rate
   from 1/256 to 1/4 294 967 296. This is the single largest measurable gain.
2. **Use a real CRC, not an XOR fold.** XOR has zero burst-error property; a
   CRC-32C gives distance-4 over ~65 KB and hardware acceleration on every
   target CPU.
3. **Add a checksum to the cct, and back it up.** Currently it has neither, and
   `zero_out` on the live entry flips `CC_VALID` with no complaint at all. This
   is now the highest-priority fix in the list — it is the only place where a
   silent corruption changes a validity decision rather than just data.
4. **Do not rely on structural checks as the de-facto integrity layer.** They
   are incidental coverage, not designed coverage, and they will not catch a
   corruption that happens to look plausible.

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

## Phase 1 preview — the identity switch

The fork point. Mechanical, fully researched, and deliberately kept separate from
the substantive Phase 2+ changes so the diff is reviewable as "this is where we
broke away".

Exact changes:

| What | Where | Change |
|---|---|---|
| Module / fs type | `Kbuild` | `SPADFS_NAME = spadfs2` as the branch default |
| Format discriminator | `struct.h:68` | `SUPERBLOCK_SIGNATURE` → `"FS SPAD2\0"` |
| Version semantics | `struct.h:71-73` | Reset to spadfs2's own `1.0.0`; do not inherit 1.0.22 numbering |
| Tool names | `Makefile` `all`, `install-userspace` | `mkspadfs2`, `spadfsck2` |
| Filesystem symlinks | `Makefile:82-83` | `mkfs.spadfs2`, `fsck.spadfs2` |
| Usage strings | `utils/spadfsck/sck.c:164` | `"spadfsck:"` → `"spadfsck2:"` (audit all tool banners) |
| Module identity | `super.c:2058-2061` | Keep `MODULE_LICENSE("GPL")`; add `MODULE_DESCRIPTION` (clears the standing modpost warning) once authorship terms are confirmed |

**Not changed:** the internal `spadfs_*` C symbol names. No `EXPORT_SYMBOL` exists,
so they cannot collide, and renaming 15,600 lines for nothing is exactly the kind
of churn this branch is trying to avoid.

**Acceptance:** Phase 0's rig runs against the reference and shows behavioral
parity on the operation sequence, *before* any Phase 2 format change is applied.

---

## Phase 1 — executed

Landed on `modernization`. Verified: full clean build produces `spadfs2.ko`
(`alias: fs-spadfs2`), `mkspadfs2`, `spadfsck2`, with only the known
`MODULE_DESCRIPTION` warning; `make test` passes 18/18 round-trip profiles
against the new format.

| What | Where | Done |
|---|---|---|
| Format discriminator | `struct.h` | `SUPERBLOCK_SIGNATURE` → `"FS SPAD2"` (8 chars, fills `__u8[8]`) |
| Version reset | `struct.h` | `1.0.22` → `1.0.0` — spadfs2's own numbering, not inherited |
| Module / fs type | `Kbuild` | default `SPADFS_NAME = spadfs2`; override to `spadfs` for the reference rig |
| Tool binaries | `Makefile` | `mkspadfs2` / `spadfsck2` via `SPADFS_TOOL_SUFFIX` |
| Filesystem symlinks | `Makefile` | `mkfs.spadfs2` / `fsck.spadfs2` |
| Tool banners | `utils/spadfsck/*.c`, `utils/mkspadfs/mkspadfs.c` | 16 literals now derive from one `-DTOOL_BANNER` macro |
| Test harness | `tests/lib.sh` | tool paths env-overridable (`MKSPADFS`, `SPADFSCK`) so one suite drives either stack |

### A latent overflow the signature change exposed

`mkspadfs.c:459` wrote the signature with **`strcpy`**. The old value
`"FS SPAD\000"` was 8 bytes *including* its terminator, so it fitted
`__u8 signature[8]` exactly. The new `"FS SPAD2"` is 8 characters, so
`strcpy` would write **9 bytes** — a one-byte overflow into the following
field. Replaced with a fixed-size `memcpy` plus a `_Static_assert` so a future
shortening of the literal fails the build instead of silently corrupting a
volume.

This is worth noting as a class: the old signature's fit was an accident of the
string being exactly the field size, and it would have broken on the first
plausible rename.

### Banner rewrite method

Done with a string-literal-aware transformer rather than `sed`, because
`SPADFSCK` also appears as an include-guard identifier (`_SPADFS_SPADFSCK_SCK_H`)
which must not be touched. It emits C concatenation (`"RUN " TOOL_BANNER " AGAIN"`),
so the banner comes from one build-time definition per tool.

### Note on the `struct.h` linter noise

Every edit to `struct.h` draws ~20 `__u8`/`__u32`/`__finline__`/`SPAD2CPU32_LV`
"unknown type" errors. These are the AGENTS.md Rule 1 phantom class, and three
things were checked rather than assumed:

1. **Pre-existing, not caused by this work** — pristine `main`'s `struct.h` has
   75 occurrences of those symbols and zero `#include` lines: identical structure.
2. **The suggested fix does not break userspace** — include guards stop the
   recursion and it compiles. (An earlier claim here said it would; that was wrong.)
3. **The suggested fix does break the kernel** — verified in a throwaway copy:
   `utils/common.h:22: fatal error: sys/types.h: No such file or directory`,
   because `common.h` pulls libc headers into a kernel translation unit.

So the only change the checker would accept is one that breaks the module build.
Not applied. `struct.h` is left as the author wrote it, and GCC via kbuild is
clean.

---

## Open decisions

1. **Kernel floor for spadfs2 — DECIDED: 6.1 LTS, distro-kernel support dropped.**
   Executed; see the section below.

---

## Kernel floor — decided and executed

`linux-compat.h`: **343 → 171 lines (172 deleted, 50%)**. Only four
`LINUX_VERSION_CODE` gates remain, all forward-looking.

### The rule that made the trim correct

The floor is the **minimum**, and we still build against newer kernels. So:

* `#if < X` shim → **delete** only when `X <= 6.1` (no supported kernel is older)
* `#if >= X` gate → **keep** when `X` is above the floor, because it still fires
  on kernels we build against

Getting this backwards is how the first attempt broke the build: deleting the
`>= 6.9` `SLAB_MEM_SPREAD` gate removed a shim that 7.2 genuinely needs.

### What survived and why

| Kept | Reason |
|---|---|
| `< 6.3` `mnt_idmap` → `user_namespace` | true at 6.1/6.2; needed for those kernels |
| `< 6.6` `inode_get/set_ctime` | same |
| `< 6.7` `inode_get/set_atime/mtime` | same |
| `>= 6.9` `SLAB_MEM_SPREAD` | fires on 6.9+ including our 7.2 target |
| `spadfs_cache_t`, `spadfs_free_cache` | **SpadFS-private** — the kernel has no such names |
| `get_current_{uid,gid,fsuid,fsgid}()` | **SpadFS-private** — kept in the post-uid-namespaces form |
| `spadfs_memcpy` / `spadfs_memmove` | `CONFIG_FORTIFY_SOURCE` workaround, not version-gated |

Everything else was either a `< 6.1` shim or an API the modern kernel provides
natively (`mutex_*`, `file_inode`, `kvfree`, `SB_RDONLY`, `inode_unlock`,
`memcpy_and_pad`, `PTR_ALIGN`, `set_nlink`, `match_strlcpy`, `vprintk`, …).

### Distro macros: stubbed, not deleted

`TEST_RHEL_VERSION` had **14 call sites outside** `linux-compat.h` (super.c,
file.c, inode.c, name.c, spadfs.h) and `TEST_STABLE_BRANCH` had 1. My original
analysis only counted gates inside the compat header and badly under-scoped this.

They are now `#define ... 0` stubs, so every `&& !TEST_RHEL_VERSION(...)` and
`|| TEST_RHEL_VERSION(...)` folds at compile time with **zero edits to those 15
sites**. Removing the dead conditions from the call sites is a follow-on
cleanup — cosmetic now that the compiler handles it.

### Verified

Built clean against **three** kernels, per AGENTS.md Rule 2:

| Kernel | Result |
|---|---|
| 6.18.58 | clean — only the known `MODULE_DESCRIPTION` warning |
| 7.2.10 | clean |
| 7.2.11 | clean |

`make test` → 18/18 round-trip profiles pass.

Note: 6.1 itself has headers on this machine but no `/lib/modules` entry, so the
floor could not be built against directly. Verified on 6.18 and 7.2 instead.

---
2. **Licensing.** Still unresolved. `LICENSE` is a status notice, not a grant.
   A `spadfs2` fork with a new signature does not change that — confirm terms
   with the author before publishing the fork as usable.

---

## Next step

Phase 0 is unblocked and should be executed first — it is format-neutral, mostly
unprivileged, and it is the only thing that makes every later phase measurable
rather than argued.

Phases 1–5 each need their own plan, written after Phase 0 produces the baseline
numbers. In particular **Phase 4 should not be designed before 0.4's rig can
answer whether the current hash actually clusters** — that measurement may make
the whole item moot, or may show it is worse than assumed.
