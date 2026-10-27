# AGENTS.md — SpadFS

Project-specific instructions for agents working in this repository.
Read this before touching anything. Several rules here exist because they cost real
time to discover the hard way.

---

## What this repository is

An **out-of-tree Linux kernel filesystem module** plus its userspace tools.

```text
spadfs.ko          kernel module        (name.c dir.c alloc.c file.c super.c ...)
utils/mkspadfs/    formatter, standalone userspace program
utils/spadfsck/    checker/repair, standalone userspace program
```

**This is a mirror, not upstream.** SpadFS was designed and written by
**Mikuláš Patočka** (Mikulas Patocka) as his PhD thesis. No agent working here
may present this code as authored by us. See [`CREDITS`](CREDITS).

Version in tree: **1.0.22**. Supports kernels from 2.6.9 through 7.2.

---

## ⚠️ Rule 1 — Do NOT trust LSP / clangd diagnostics in this tree

This is the single most expensive thing to learn here.

There is **no clangd installed**, and no configuration that makes kernel headers
resolve for out-of-tree module code. Diagnostics from this tree are systematically
false. Typical garbage output:

```text
error: In included file: 'linux/init.h' file not found
error: Call to undeclared function 'unlikely'
error: Use of undeclared identifier 'EINVAL'
error: Incomplete definition of type 'struct qstr'
```

`unlikely`, `likely` and `EINVAL` are *definitely* defined — by the kernel headers
the tool cannot find. Every one of these errors cascades from the first missing
include. A report of "19 issues" on a file that compiles clean is a broken tool,
not broken code.

**The only arbiter is GCC via kbuild.** Before acting on any diagnostic:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD \
     STANDALONE_BUILD=y CONFIG_SPADFS_FS=m <file>.o
```

If that compiles clean, the diagnostics are wrong. Do not "fix" them. Do not add
includes, casts, or declarations to satisfy a tool that cannot see the kernel.

**Never claim a change is verified based on LSP output alone in this repo.**

### Known non-actionable reserved-identifier reports

Three classes of "🔴 must fix" reserved-namespace findings recur on every turn in
this tree and **must not be "fixed"**. They are not defects and renaming them
breaks the build or the thing being guarded:

| Report | Why it is wrong to act on it |
|---|---|
| `__KERNEL__` (e.g. `meta_crc.h:41`) | Predefined by the compiler itself. `echo \| gcc -dM -E -D__KERNEL__` shows the toolchain owns this name. It is how every kernel source file distinguishes kernel from userspace. |
| `__cold` (`linux-compat.h:155-156`) | Kernel/compiler attribute vocabulary. Used as an attribute at `super.c:3`, `super.c:146`, `super.c:361`. The `#ifndef __cold` guard exists precisely to *supply* it on toolchains that lack it — renaming makes the guard test for the wrong symbol and the fallback never fires. |
| `_SPADFS_LINUX_COMPAT_H` and friends | Include guards. Leading `_` + uppercase is technically reserved by C, but this is the established convention across **all 6 headers** in the tree (`_SPADFS_COMMON_STRUCT_H`, `_SPADFS_SPADFS_H`, `_SPADFS_COMMON_ENDIAN_H`, `_SPADFS_UTILS_ENDIAN_H`, `_SPADFS_COMMON_SFSAPAGE_H`, `_SPADFS_SPADFSCK_SCK_H`). Renaming one breaks consistency with the rest. |

If a new report of this shape appears, check whether the identifier is
compiler-predefined, kernel-provided, or tree-conventional before treating it as
actionable. The arbiter is still GCC via kbuild, per Rule 1 above.

---

## Rule 2 — Build and verification

Full build:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD STANDALONE_BUILD=y CONFIG_SPADFS_FS=m
```

A **known-good** build emits exactly one warning:

```text
WARNING: modpost: missing MODULE_DESCRIPTION() in spadfs.o
```

Anything else is a regression. `spadfs.ko` must link. Treat that single warning as
the baseline; do not add to it, and do not report success if new ones appear.

Verify against more than one kernel when touching `linux-compat.h` or anything
version-gated. Available here: `7.2.10_1`, `7.2.11_1`, `6.18.57_1`, and
`kernel-headers-6.1.*` under `/usr/src` (note: 6.1 has headers but no
`/lib/modules` entry, so it cannot be built against directly).

---

## Rule 3 — Kernel API gotchas hit in this tree

**`strncpy` no longer exists.** Removed from `include/linux/string.h` in recent
kernels (still present in 6.18, gone by 7.2). Replacement for fixed-size on-disk
slots:

```c
memcpy_and_pad(dest, dest_len, src, count, 0);
```

Do **not** substitute these, even though tooling may suggest them:

| Rejected | Why |
|---|---|
| `strscpy` / `strscpy_pad` | Require NUL-terminated `src`. Kernel `qstr->name` is **not** NUL-terminated. Breaks on embedded NULs. |
| `strtomem_pad` / `strtomem` | Require compile-time-known dest via `ARRAY_SIZE(dest)`. `FNODE_NAME(fnode)` is a cast pointer — will not compile. |

`memcpy_and_pad` landed in **v5.9**. For older kernels a shim lives in
`linux-compat.h`. Keep that shim.

**`#include <string.h>` is wrong here.** Use `<linux/string.h>`. The bare form
does not exist in kernel trees and fails the build.

**`spadfs_memcpy` exists for a reason.** Under `CONFIG_FORTIFY_SOURCE` it becomes a
byte loop to dodge false-positive field-spanning warnings. Use it, not raw
`memcpy`, in compat-sensitive paths.

---

## Rule 4 — `.gitignore` traps

Two patterns in `.gitignore` are **root-anchored on purpose**:

```gitignore
/mkspadfs      ← the built binary at repo root
/spadfsck      ← the built binary at repo root
```

Without the leading slash they match the **source directories** `utils/mkspadfs/`
and `utils/spadfsck/`. That bug silently dropped **24 source files** — the entire
formatter and the entire fsck, ~40% of the codebase and the only way to create or
repair a SpadFS volume.

After any `.gitignore` edit, verify:

```bash
git add --dry-run -A . | grep -c utils/        # expect 31 — the tools' sources
git add --dry-run -A . | grep -c spadfsck/    # expect 21 — fsck must be present
git status --ignored --porcelain -uall        # only binaries/artifacts listed
```

If the `utils/` count collapses, a `.gitignore` pattern is eating source again.

**Never commit `compile_commands.json` or `compile_flags.txt`** — they contain
machine-specific absolute paths (`/usr/src/kernel-headers-<ver>/...`) and are
wrong for every other machine. They are ignored; keep them that way.

---

## Rule 5 — Format discipline (no compatibility burden)

This is a filesystem. **A bug here destroys user data, not just a process.**

**Backwards compatibility is explicitly out of scope.** `spadfs2` is a clean
break; no v1.0.22 volume must ever be mountable by it. Do not spend effort on
compat flags, migration windows, dual-algorithm periods, or "don't misread the
old format" code. Do not let fear of breaking an old volume block a format
change that is otherwise correct.

What follows is **not** about old volumes. These are intra-format invariants of
the format you are building right now. Violating them corrupts the filesystem
regardless of what came before.

- **All on-disk pointers are in 512-byte sectors**, even when the block size is
  larger. Pointers must be divisible by `block_size/512`.
- **Every on-disk field goes through `CPU2SPAD*` / `SPAD2CPU*_LV`.** Never
  assign raw. `tests/roundtrip.sh` carries a `bigendian` profile precisely
  because getting this wrong is silent and architecture-dependent.
- **Structures with `(cc, txc)` are dual-versioned** (`fnode.size[2]`, apage
  halves, dnode versions, `fixed_fnode_block.fnode0/fnode1`). Read the state
  machine comment at `struct.h:297-313` before modifying any of them. The
  validity test is `(s32)(cct[cc] - txc) >= 0` — **sign-sensitive, do not
  "simplify" it.** That signed comparison is the entire commit mechanism, not
  a style choice.
- `CCT_SIZE` and `SSIZE_BITS` are marked `/* do not change it */` in
  `struct.h`. These are addressing-width constraints, not compat shims.

### The three-reader rule

**Every on-disk structure has three readers: the kernel module, `mkspadfs`, and
`spadfsck`. A format-visible change must be made in all three, and proven by
an actual mount.**

This rule exists because we broke it. Phase 2.2 Cat 2 converted
`fixed_fnode_block` from the XOR fold to the CRC trailer in `struct.h`,
`mkspadfs` and `spadfsck` — but not the kernel, which still read it with
`__byte_sum` (`bufstruc.c:103`) and wrote it with `do_fnode_block_checksum`.
Since mkfs zeroes the legacy XOR field, the fold can never equal
`CHECKSUM_BASE`:

```text
__byte_sum(block,512) = 0x96   CHECKSUM_BASE = 0x43   -> kernel rejects
trailer stored        = 0x88b78f11
trailer computed      = 0x88b78f11                   -> fsck accepts
```

**Every mount of every formatted volume failed with `EUCLEAN` — and 26 commits
of green tests did not catch it.** The suite is `mkfs → fsck`: both readers
were updated, so it agreed with itself perfectly while being wrong about the
module. `anode` avoided this only because its kernel write path (`file.c:97`)
happened to be converted too.

So: when you change anything a reader validates — checksum scheme, field
meaning, flag bits, layout —

1. grep for **all three** readers, not the two that are convenient
2. update the kernel path even when only userspace code prompted the change
3. **mount it.** `sudo bash tests/mount_smoke.sh` exists for this. A format
   change that has never been mounted is unverified, no matter how many
   unprivileged tests pass.

`mkfs → fsck` agreement is not evidence of correctness. It is two programs
agreeing with each other while a third disagrees with both.

---

## Rule 6 — Code style

Follow **Linux kernel style** (`Documentation/process/coding-style.rst`).

- **Indent with tabs**; continuation lines use tab + spaces. The tree is tabs
  throughout — `name.c`, `dir.c`, `alloc.c` and `struct.h` contain zero
  space-indented lines.
- **Comparison operators go in whichever direction reads clearly.** `>` and `>=`
  are used freely here (5725 and 301 occurrences). Do not swap operands to force
  `<` / `<=`.

Above all: match the surrounding file. Consistency with the 15,600 existing lines
beats any abstract preference. Where this file is silent, kernel style is the
default.

---

## Rule 7 — Licensing is UNRESOLVED

Do not guess, assume, or fabricate a license.

**`LICENSE` in this repo is a status notice, not a grant.** It exists precisely to
make the unresolved state visible at the place people look first. Do **not**
replace it with standard license text until the author confirms terms — doing so
would assert a licensing position that nobody with standing has granted.

Current state:
- **No copyright header in any source file.** (Grep hits for `(c)` are C macro
  parameters, not notices.)
- Only signal: `MODULE_LICENSE("GPL")` at `super.c:2058`.
- **That tag covers the kernel module only.** `utils/mkspadfs` and
  `utils/spadfsck` are standalone userspace programs not linked into the kernel —
  they are *not* licensed by it.
- No `MODULE_AUTHOR`, no upstream URL in the tree.

Open questions that must be settled with the author before any grant is made:
GPL-2.0-only vs GPL-2.0-or-later; whether the grant extends to `utils/`; and what
license (if any) applies to the mirror-authored documents — `CREDITS`,
`DESIGN-ANALYSIS.md`, `AGENTS.md` and `LICENSE` itself.

Do not add `SPDX-License-Identifier` tags until that is settled.

---

## Rule 8 — Git

Global rule applies: **never auto-commit.** Stage, tell the user what changed, let
them review with `git diff --cached`, commit only on explicit instruction.

Note the tree was moved into `/home/bolsen/master/spadfs` and `git init`'d on
branch `main` with no commits yet.

---

## Before claiming any change is done

1. `make ... <file>.o` compiles clean.
2. Full `spadfs.ko` builds; **no new warnings** beyond the known
   `MODULE_DESCRIPTION()` one.
3. Built against ≥2 kernel versions if compat code was touched.
4. `git add --dry-run -A . | grep -c utils/` still reports 31 (gitignore intact).
5. No LSP-derived "fixes" were made to satisfy the broken tool.
6. On-disk format unchanged unless that was the explicit, reviewed task.

Evidence before assertions. In this repo the tooling lies and the compiler does
not.
