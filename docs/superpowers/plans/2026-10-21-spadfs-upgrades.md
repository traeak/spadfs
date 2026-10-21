# SpadFS Upgrades — Roadmap and Phase 0 Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the nine ranked weaknesses in `DESIGN-ANALYSIS.md` into a sequenced,
format-safe upgrade program, starting with the verification harness that every
later phase depends on.

**Architecture:** Classify each weakness by **on-disk format impact**, not by
severity. Format-neutral work proceeds immediately; format-affecting work is gated
behind the existing `flags_compat_*` negotiation machinery and a decision about
upstream vs fork. Nothing that silently changes what bytes land on disk may be
merged before a corruption-detection baseline exists.

**Tech stack:** C (Linux kernel module + userspace tools), POSIX shell for the
harness, `mkspadfs`/`spadfsck` as the test oracle, GitHub Actions for CI.

**Spec:** [`DESIGN-ANALYSIS.md`](../../DESIGN-ANALYSIS.md) — *Ranked weaknesses*.

---

## ⚠️ Read this before touching anything

Three facts established by direct verification in this session, each of which
overrides what a reasonable reader would assume:

1. **`version_major` is an exact-match gate.** `super.c:1210` rejects any volume
   whose major differs. There is no minor-version tolerance for layout. Any change
   to what bytes go where is a hard fork from every existing volume.

2. **The compat-flag machinery is the only safe vehicle.** `super.c:1222-1227`:

   ```c
   flags_compat_rw  & ~(0)                              /* ANY bit → reject */
   flags_compat_ro  & ~(FLAG_COMPAT_RO_DIRECTORY_SIZES)
   flags_compat_none & ~(UNIX_NAMES|DYNAMIC_MAGIC|EXTRA_SPACE)
   ```

   Setting a bit in `flags_compat_rw` makes **old drivers refuse to mount** —
   which is exactly the safety property we want for format-affecting changes.
   The superblock also has a spare `__u64 pad2` (`struct.h:105`), so an additive
   pointer fits without growing the structure.

3. **The LSP in this repo lies.** No clangd is installed; kernel headers never
   resolve, so it reports `unlikely`, `EINVAL` and `struct qstr` as undeclared.
   GCC via kbuild is the only arbiter. See `AGENTS.md` Rule 1.

---

## Scope check — this is not one plan

The nine weaknesses split into four groups with incompatible risk profiles. Each
group is its own plan; **only Phase 0 is written in full here**, because its
output is a prerequisite for the other three and it carries no format risk.

| Phase | Weakness | Format impact | Status |
|---|---|---|---|
| **0** | *(none — enabler)* | **None** | **Plan below, actionable now** |
| **1** | #1 checksum, #2 cct mirror | Additive, flag-gated | Blocked on Q1–Q3 |
| **2** | #4 hash quality | Layout-derived; needs full dir rebuild | Blocked on Q1, Q4 |
| **3** | #7 symlink cap, #8 sparse files | New on-disk structures | Blocked on Q1–Q3 |
| **—** | #3, #5, #9 | Architectural | **Not planned — see below** |

### Two corrections to `DESIGN-ANALYSIS.md` found during verification

These change the ranking and should be folded back into the spec:

- **#6 (2^16 crash-count ceiling) is already mitigated.** `fix_cct()`
  (`sckcct.c:99-133`) handles an invalid table, can allocate a **new** cct at a
  different location, and `--reset-crash-counts` zeroes the table and resets
  `tx.cc` to 0. The residual risk is *operational* (hitting the ceiling without
  noticing), not structural. **Downgrade from #6 to a monitoring task.**

- **#2 (cct single point of failure) is narrower and easier than stated.** The
  superblock **already has a backup copy** near the end of the device
  (`mkspadfs.c:77`), located and validated by `test_backup_superblock()`
  (`scksuper.c:166`), and fsck offers `CREATE BACKUP SUPERBLOCK?` when absent.
  The crash-count table is written exactly once (`mkspadfs.c:516`) and has no
  such copy. So mirroring `cct` is **not new machinery — it is applying a
  precedent the format already uses.** This makes #2 cheap and idiomatic.

### Not planned

**#3** (size in directory block), **#5** (no ordered iteration), **#9** (no
`open_by_handle_at`) are consequences of the two founding bets. Fixing any of
them means un-embedding the inode or adding a second index — a different
filesystem. They are documented as accepted trade-offs, not backlog items.

---

## Global constraints

- **Never change `version_major`.** (`SPADFS_VERSION_MAJOR`, `struct.h:71`.)
- **Never change `CCT_SIZE` or `SSIZE_BITS`** — both marked `do not change it`.
- Every format-affecting change **must** set a compat bit that old drivers reject,
  and must be **detectable** by `spadfsck` on a volume written by an older driver.
- All on-disk fields go through `CPU2SPAD*` / `SPAD2CPU*_LV`.
- **Kernel and fsck must agree.** Every metadata structure is parsed in both
  `super.c`/`dir.c`/`file.c` **and** `utils/spadfsck/`. A change to one without
  the other is a silent divergence that only a crash reveals.
- Clean build baseline: exactly one warning, `missing MODULE_DESCRIPTION()`.
- **Licensing is unresolved.** `LICENSE` is a status notice, not a grant. See
  `AGENTS.md` Rule 7. Do not add `SPDX-License-Identifier` anywhere.

---

## The governance question that gates Phases 1–3

This is a **mirror of a live upstream project by a named author**, currently
**without a confirmed license**. Every format-affecting change forks the format:
images written by our `mkspadfs` become unreadable by upstream, and vice versa.

That is not a technical objection — it is the reason Phases 1–3 are not written
in detail here. The right answer differs completely depending on whether this is
a contribution, a fork, or a research sandbox. See the questions at the end.

**Phase 0 is safe under all three answers**, which is why it is the one to do now.

---

# Phase 0: Verification Harness

**Goal:** an unprivileged, repeatable, scripted round-trip test that can create
volumes, corrupt them in *structured* ways, and measure what fraction of damage
each checksum detects — producing the baseline number that Phase 1 argues from.

**Why this is first:** there is currently **zero test infrastructure** in this
repository (verified: no `test/`, no kunit, only `#ifdef TESTCODE` hooks in
spadfsck). Proposing a checksum upgrade without a way to measure detection rates
is proposing a change that cannot be validated.

**Key capability discovered:** the whole cycle runs **without root, without loop
devices, and without loading the module**:

```bash
dd if=/dev/zero of=/tmp/t.img bs=1M count=64
./mkspadfs --block-size 4096 --no-trim /tmp/t.img     # exit 0
./spadfsck -f /tmp/t.img                              # exit 0, "clean"
```

Verified this session. Note `mkspadfs` opens with `O_RDWR|O_EXCL` and **no
`O_CREAT`** — the image file must be pre-created.

**Out of scope for Phase 0:** anything requiring the module loaded (mount, fsx,
crash-injection, fio). Those need a privileged runner and are a separate tier.

---

### Task 0.1: Fixture generator

**Files:**
- Create: `tests/lib.sh`
- Create: `tests/fixtures.sh`

**Interfaces:**
- Produces: `make_image <path> <size> [mkspadfs-opts...]` → 0 on success;
  `REPO_ROOT` variable; `require_tools` asserting `./mkspadfs` and `./spadfsck` exist.

- [ ] **Step 1: Write `tests/lib.sh`** with `REPO_ROOT` resolution, a
  `die()`/`log()` pair, and `require_tools` that fails loudly if `./mkspadfs` or
  `./spadfsck` is missing (they are build products — the script must build them
  or instruct the user to run `make`).

- [ ] **Step 2: Write `make_image` in `tests/fixtures.sh`** — `dd` a sparse file,
  invoke `mkspadfs --no-trim "$@"`, assert exit 0.

- [ ] **Step 3: Smoke it**

  Run: `bash -c '. tests/lib.sh; . tests/fixtures.sh; make_image /tmp/x.img 64M'`
  Expected: exit 0, file exists, `./spadfsck -n /tmp/x.img` reports clean.

- [ ] **Step 4: Commit** `test: add fixture generator for unprivileged volume creation`

---

### Task 0.2: Round-trip matrix

**Files:**
- Create: `tests/roundtrip.sh`

**Interfaces:**
- Consumes: `make_image` from 0.1.
- Produces: pass/fail over a matrix of mkspadfs options.

- [ ] **Step 1: Write the matrix** — exercise the option combinations that change
  on-disk layout, since those are where format assumptions hide:
  `--block-size {512,4096}`, `--unix-names` vs `--spad-names`,
  `--big-endian` vs `--little-endian`, `--dynamic-magic`,
  `--directory-sizes`, `--checksums` vs `--no-checksums`,
  and `--cluster-size`/`--cluster-threshold` variants.

- [ ] **Step 2: For each, assert `spadfsck -f` exits 0** on a freshly created
  volume. Any non-zero exit on a virgin image is a bug worth finding now.

- [ ] **Step 3: Run and record**

  Run: `bash tests/roundtrip.sh`
  Expected: all combinations PASS.

- [ ] **Step 4: Commit** `test: add mkspadfs/spadfsck round-trip matrix`

---

### Task 0.3: Structured corruption injection — the important one

**Files:**
- Create: `tests/corrupt.py`
- Create: `tests/detect.sh`

**Why this task exists.** My analysis claimed the 8-bit XOR `__byte_sum` catches
"~255/256 of random corruption." That is **wrong in the direction that matters**,
and the harness must measure the real number:

```c
__sum ^= *(unsigned long *)__ptr;   /* struct.h:947 — XOR of 8-byte words */
```

For a **single-bit flip**, XOR detects **100%** — the sum always changes. The
actual blind spots are *structured* corruptions:

| Corruption | XOR `__byte_sum` detects? |
|---|---|
| Single bit flipped | **Yes, always** |
| Two identical words swapped | **No** |
| A word duplicated over a neighbour | **No** |
| Whole 8-byte block copied elsewhere | **No** |
| Byte transposition within a word | **Often no** |
| Two independent bit flips that XOR to zero | **No** |

This is the class that real hardware produces — a SATA link glitch, a controller
copy-forward, a torn write that duplicates a sector. **Random-bit testing would
show 100% and falsely conclude the checksum is fine.**

- [ ] **Step 1: Write `tests/corrupt.py`** taking a class name and applying it at
  a given offset: `bitflip`, `word_swap`, `word_dup`, `block_copy`,
  `byte_transpose`, `xor_cancel`, `zero_out`.

- [ ] **Step 2: Write `tests/detect.sh`** that, for each corruption class × each
  metadata target (superblock, txblock, cct, fnode block, dnode, apage), applies
  the corruption to a copy and records whether `spadfsck -n` exits non-zero.

- [ ] **Step 3: Produce the baseline table** and save it to
  `tests/BASELINE-checksum-detection.md`. This is the artifact Phase 1 argues from.

  Run: `bash tests/detect.sh | tee tests/BASELINE-checksum-detection.md`
  Expected: a matrix with a detect-rate per class.

  **Read the result as combined detection, not checksum-only.** The XOR
  `__byte_sum` is *provably* blind to word swaps and block copies — XOR is
  commutative and associative, so reordering whole 8-byte words leaves the sum
  identical. But `spadfsck` also runs structural validation (magic numbers,
  range checks, `cc`/`txc` ordering, monotonicity), which may catch those cases
  independently. So the measured rate is the **floor of the combined detector**,
  and any class showing high detection must be checked for *which* layer caught
  it. Record both: checksum-blindness and structural-detection, per class.

  If a class shows 100% detection, confirm whether the checksum contributed at
  all — if not, that class is unprotected on volumes mounted `--no-checksums`.

- [ ] **Step 4: Commit** `test: add structured corruption harness and checksum baseline`

---

### Task 0.4: Make target and CI

**Files:**
- Modify: `Makefile`
- Create: `.github/workflows/test.yml`

- [ ] **Step 1: Add `make test`** depending on the userspace tools only
  (`mkspadfs spadfsck`), running 0.2 then 0.3. It must **not** require root.

- [ ] **Step 2: Add a GitHub Actions job** — `ubuntu-latest`, `make` then
  `make test`, no privileged steps. Upload the baseline table as an artifact.

- [ ] **Step 3: Verify locally** `make test` passes end to end.

- [ ] **Step 4: Commit** `ci: run unprivileged spadfs test suite on push`

---

## Review Focus

Failure modes the spec implies but no single task's happy path exercises:

1. **Silent kernel/fsck divergence** — a structure accepted by `spadfsck` but
   rejected by the driver (or vice versa) is invisible until a crash. *Mitigation:*
   Phase 1+ requires both parsers change in the same commit; 0.2's matrix is the
   only cross-check available unprivileged.
2. **Checksum disabled volumes** — `--no-checksums` sets
   `FLAG_COMPAT_FSCK_NO_CHECKSUMS`; a corruption test that assumes checksums on
   will report false confidence. *Task 0.3 must run each class under both.*
3. **Compat-bit asymmetry** — old driver reading new volume must **refuse**, not
   misread. Refusing is safe; misreading is data loss. *No unprivileged test can
   prove this; it needs two driver versions. Flagged as a Phase 1 design
   requirement, not a Phase 0 gap.*
4. **Sparse-image false negatives** — `dd` sparse files read back as zeros, so a
   corruption that writes zeros may be indistinguishable from untouched space.
   *Task 0.1 should use a non-zero fill pattern.*
5. **The `cct` mirror desync case** — if a backup table is added but not updated
   on every commit, it becomes worse than nothing: a stale copy that looks valid.
   *This is the central risk of Phase 1's #2 and must be designed in, not tested
   into.*

---

## Blocking questions

Phases 1–3 cannot be responsibly detailed until these are answered.

1. **Upstream or fork?** Is the intent to contribute these to Mikuláš Patočka, to
   maintain a divergent fork, or is this a research sandbox? Format-affecting
   changes are *hostile* to a mirror — they make our images unreadable by upstream
   and vice versa. This single answer changes the plan for 1–3 completely.

2. **Must existing v1.0.22 volumes keep mounting?** If yes, every change is
   constrained to additive, flag-gated, read-compatible forms. If no, we can
   consider real fixes (e.g. widening `cc` past 16 bits) that are otherwise
   impossible.

3. **Licensing is still unresolved.** We have no confirmed right to redistribute
   modified versions. GPL would permit it with source; nothing is confirmed.
   Should Phase 1 start before that is settled?

4. **Is there a real workload behind this?** The hash-quality question (#4) is
   only worth a full directory-rebuild migration if someone has large directories
   exhibiting clustering. Without a workload, it is speculation.

5. **Who owns the mirror-authored docs' license** (`DESIGN-ANALYSIS.md`,
   `AGENTS.md`, `CREDITS`, `LICENSE`)? They are not upstream content and should
   not silently inherit whatever the code gets.

---

## Recommendation

**Do Phase 0 now.** It is format-neutral, unprivileged, CI-able, useful under
every answer to the five questions, and it is the only thing that turns the
checksum argument from my assertion into a measured number. It also happens to
be the highest-value contribution to upstream of anything on this list — nobody
else has tests for this filesystem.

Then get answers to Q1 and Q2 before writing a single line of Phase 1.
