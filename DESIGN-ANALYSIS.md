# SpadFS — Design Analysis

> **Status of this document.** This is *third-party critical analysis* of the SpadFS
> codebase (v1.0.22), written 2026 by reading the source, not by the upstream
> author. It is commentary, not specification. For the author's own description of
> the design, see [`INTERNALS`](INTERNALS). Where this document disagrees with
> `INTERNALS`, `INTERNALS` wins as the statement of intent; this one wins as the
> statement of what the code in this tree actually does.
>
> Evidence is cited as `file:line` against v1.0.22.

---

## The two bets

Everything else in SpadFS follows from two decisions:

1. **Consistency without a journal** — ambient `(cc, txc)` validity markers plus one
   global counter table.
2. **Inodes live inside directory blocks** — the `fnode` is inode + name + first two
   extents, inline.

Both are old ideas, executed with unusual rigour. Both pay off, and both cost more
than the docs admit.

---

## Bet 1: Crash counts

### Mechanism

```c
CC_VALID:  (s32)(cct[cc] - txc) >= 0        /* spadfs.h:550 */
```

- `cc` = 16-bit **crash count**, one consumed per mount.
- `txc` = 32-bit **transaction count** within that crash epoch.
- Structures are written to disk *before* they are valid.
- Commit = **one 4-byte increment made durable in `cct[]`**.
- The top bit of `txc` encodes create(0) / delete(1), so the same signed
  comparison flips both sides of a rename atomically.

`CC_SET_CURRENT` (`spadfs.h:565`) pre-XORs `0x80000000` so that a
not-yet-committed record reads as *invalid* after a crash but *valid* to the live
mount. That single expression is the whole scheme.

`CCT_SIZE` is fixed at `65536*4` = 256 KiB (`struct.h:74`), with the comment
`do not change it`.

### What it buys

| Property | SpadFS | ext4 (journal) |
|---|---|---|
| Mount after crash | no replay | replay, O(journal size) |
| Metadata write amplification | 1× | 2–3× (journal + inode) |
| Commit cost | 4 B into rotating 256 KiB table | full barrier + journal block |
| fsck after clean crash | never | occasionally |

The **wear story is better than a journal header**. In `spadfs_commit_unlocked`:

```c
sector = fs->cc / (512 / 4);                    /* super.c:706 */
sector &= ~((1U << fs->sectors_per_buffer_bits) - 1);
```

Each commit writes a *different* 512-byte region, rotating across the full table as
`cc` advances. On flash that is ~65 536 distinct wear points instead of one hot
block. Genuinely nice property, and not obvious until you look at the arithmetic.

### What it costs

- **`cct[]` is the load-bearing wall.** 256 KiB, no redundancy visible in this
  tree. Damage it and *every* structure on the disk becomes validity-unknown. The
  existence of `--reset-crash-counts` in `spadfsck` is precisely because this is
  the one failure mode that cannot be resolved by local reasoning. A damaged journal
  means replay stops; a damaged counter table means nothing can be trusted.

- **Coarse commit granularity.** You cannot durably commit transaction *N* without
  committing *N-1*. `datasync` is threaded through `spadfs_write_file`
  (`dir.c:995`) but the epoch is global. Semantically acceptable, but one
  fsync-heavy process drags every other process's metadata to disk with it.

- **2^16 crashes.** `cc` is `__u16`; each mount burns one. The README's claim of
  "2^47 transactions" is honest, but the *2^16 mount* ceiling is a real
  operational cliff requiring fsck to clear. A long-lived fleet, or anything doing
  frequent mount/unmount (containers, ephemeral VMs), can reach it.

- **Dual-versioning tax.** Because uncommitted writes must coexist with committed
  ones, nearly every mutable structure is doubled:
  - `struct fnode.size[2]` (`struct.h:325`)
  - apage first half / second half (`struct.h:198`)
  - `dnode_page` two versions (`struct.h:677`)
  - `fixed_fnode_block.fnode0` / `fnode1` (`struct.h:621-628`)
  - `fnode_block.prev0/prev1`, `next0/next1` (`struct.h:601-608`)

  This doubling is *the* source of the codebase's density. Read the state machine
  comment at `struct.h:297-313` — six lines of pseudocode describing what a
  journal gives you for free. Every one of those transitions has to be got exactly
  right, in a filesystem with no ordered recovery to fall back on.

### One elegant exception: the anode

`struct anode` (`struct.h:721`) has **no** dual version and **no** `(cc, txc)`
pair. It doesn't need one, because extension is strictly monotone append:

```c
if (unlikely(SPAD2CPU64_LV(&anode->x[off].end_off) != lbn + 1)
 || unlikely(anode->valid_extents != off + 1)) {
        CPU2SPAD64_LV(&anode->x[off].end_off, lbn + 1);
        anode->valid_extents = off + 1;
        ...
}
```
— `file.c:244-250`

Only the tail extent is ever modified, only forward, and `valid_extents` gates how
much of the array is trusted. A torn anode write therefore exposes only
already-valid entries. **Append-only data needs no versioning.** Whoever realised
that saved a full 512 bytes per indirect block and removed a class of bugs. This is
the best-disciplined corner of the tree.

---

## Bet 2: The fnode — inode in the directory

`struct fnode` is **56 bytes** of header, then name (≤255), then the EA pool
(≤176), inside a 512-byte `fnode_block` (`struct.h:316-349`). Verified sizes:

```
sizeof(struct fnode)      = 56      (FNODE_NAME_POS)
FNODE_MAX_SIZE            = 488
FNODE_EA_POS(255)         = 312
FNODE_MAX_EA_SIZE         = 176
sizeof(fnode_block hdr)   = 24
```

**Win.** `open()` is one seek. No inode table, no block-group lookup, no
indirection between `struct inode` and its on-disk twin. For cold-cache directory
traversal — package installs, `find`, build systems — this is structurally faster
than any classically-laid-out filesystem. The README's framing ("saving one seek on
file opening or directory traversal") undersells it: it removes an entire class of
lookup, not just one seek.

**Cost — the serious one.** *File size lives in the parent directory block.*

```c
CPU2SPAD64_LV(&fnode->size[part], is);         /* dir.c:970 */
```

Every append dirties the parent directory. `spadfs_write_file` (`dir.c:995`) must
read the fnode block and rewrite it. On a hot directory with *N* actively-written
files, all *N* writers contend on the same 512 B–8 KiB blocks. ext4 keeps size in
the inode; the directory changes only on create/unlink. SpadFS couples them.

The mitigation is **preallocation**, and it is well done. `disk_size` vs `i_size`,
`prealloc[2]` in the `UNX` EA, and the `want_othersize` logic at `dir.c:986-991`
keeps the *inactive* copy of the size field rounded to the last-known-good value,
so a crash loses at most one block of prealloc bookkeeping:

```c
if (likely(f->commit_sequence == f->fs->commit_sequence))
        want_othersize = f->crash_disk_size;
else
        want_othersize = f->disk_size;
```

With `prealloc_part=8` the fnode is rewritten once per ~1/8-of-file growth instead
of per block — turning a disaster into roughly 12% overhead. Good engineering, but
it papers over the architectural choice, and it is *tunable* rather than *solved*.

**Second cost.** Rename is a **move, not a pointer swap**. Renaming across
directories relocates the whole fnode (up to 488 bytes) and rehashes. Renaming
within a directory is delete + create + possible dnode split. ext4 renames a
directory entry in tens of bytes.

**Third cost.** No stable inode→location mapping. Hence the README's honest list:
no NFS export, no `open_by_handle_at`, and 32-bit inode number collisions on
32-bit userspace. `stable_ino` in `ea_unx` (`struct.h:472`) papers over it for
userspace-visible numbering but cannot fix the width of the kernel's `ino_t`.

---

## Bet 3: Extendible hash, no B-tree

`dnode_page` (`struct.h:665`) → array of packed 6-byte `dnode_page_entry` →
fnode blocks. Split on overflow; add a deeper dnode when a slot cannot split; fall
back to fnode chains when hash bits run out (`INTERNALS`, `dir.c:1143-1240`).

Right call for the create/delete-heavy, lookup-by-name workload that motivated it.
No balancing, no sibling links, no left-to-right scan consistency problem. The
degradation ladder — split, then deeper dnode, then chain — is a good "never give
up" design: worst case is slow, not broken.

**But `readdir` yields hash order.** Not sorted, not insertion order, not
locality-ordered. Applications wanting sorted output sort it themselves, and large
directory scans touch fnode blocks in effectively random disk order. B-trees give
ordered iteration for free; this design gives it up entirely to win on update cost.
That is a real semantic regression users notice — `ls` on big directories,
tab-completion patterns, anything doing range-ish access.

### Hash quality — unverified risk

```c
static __finline__ hash_t name_len_hash(const char *name, int len)
{
        hash_t hash = 0;
        while (len--)
                hash = hash * 15 + (*name++ & 0xdf);
        return hash;
}
```
— `struct.h:689`

Multiplier **15** is a weak multiplicative constant, and the tree consumes
**low bits first**:

```c
hpos = hash & ((1 << fs->dnode_hash_bits) - 1);   /* dir.c:1173 */
...
hash >>= fs->dnode_hash_bits;                     /* dir.c:1237  */
```

In multiplicative hashing the low bits are the worst-mixed; entropy migrates
upward with each multiply. So the *top* level of the directory — which does the
most distribution work — gets the *weakest* bits. The textbook fix is
`hash >> (32 - k)`, or a proper finalizer (FNV-1a, murmur3 avalanche).

The split path partially compensates with a rotating-bit scheme:

```c
return ((hash >> (hash_bits & HASH_VAL)) ^ (hash_bits >> HASH_1_SHIFT) ^ 1) & 1;
```
— `dir.c:1259`

which is a reasonable band-aid. **I would want a benchmark of ~1M-name directories
with common prefixes and suffixes before trusting this.** Cheap to test, high
payoff if it is clustering.

Also: `& 0xdf` case-folds *unconditionally*, even in `unix_names` mode. `"Foo"`
and `"foo"` hash identically and are distinguished only by
`spadfs_compare_names_internal` (`name.c:41`). Intentional — one hash function,
Windows-compat — but it means case-variant directories are pure collision load.

---

## Bet 4: The sqrt(n) allocator

Free space is a sorted doubly-linked list of `struct aentry` (24 bytes,
`struct.h:245`) scattered anywhere in a 32 KiB apage, ordered only by
`prev`/`next` pointers, never by physical position. Lookup samples ~√n entries at
pseudorandom offsets, takes the highest one below the goal, then walks the list.
Average O(√n) — documented at `alloc.c:632-639`.

This is the most interesting piece in the design. Why it works:

- **Fixed-size pages.** No variable-length metadata growth. An apage is always one
  page.
- **Insert is O(1)** given the neighbour — no shifting, no rebalancing.
- **Split/merge is page-granular**, and the two-halves + `(cc,txc)` scheme makes
  it commit atomically with the directory it serves.
- **Bitmap fallback** when fragmentation beats the list — `APAGE_BITMAP`
  (`struct.h:170`), with the crossover *computed*, not guessed:

  ```c
  #define CONV_APAGE_SECTORS(page_bits, block_bits)   /* struct.h:484 */
  ```

The FS keeps enough spare apages for the worst-case fragmentation scenario
(`INTERNALS`), which is the correct way to make a randomized structure safe:
**bound the tail with reserved capacity** rather than pretend the average holds.

Costs: √n pointer chases per allocation — for a 32 KiB apage with ~1300 entries,
~36 random 24-byte reads in a hot page. Acceptable. `MAX_APAGES` is `0x7ffffffe`
(`struct.h:511`), bounding the index.

The zone/group scheme (metadata / small-file / large-file) is **memory-only** — no
on-disk structure. Correct: they are an allocation *policy*, not an on-disk
*layout*, so fsck can rebuild them differently without touching the disk.
Cluster-padding large files (`--cluster-size`) is the right answer to the
tail-fragmentation this scheme would otherwise suffer.

---

## Cross-cutting

### Endianness discipline

Explicit `CPU2SPAD*` / `SPAD2CPU*_LV` on every field, a `byte_sex =
0x01234567890ABCDEF` self-probe in the superblock (`struct.h:80`), and all
pointers in 512-byte sectors regardless of block size, with the divisibility
constraint documented. Textbook-correct, and rare.

The `MAKE_PART_0/1` splitting of 64-bit offsets into `run10/run11/run1n` triples
(`struct.h:33-38`) is packing for space at real readability cost — the `>> 31 >> 1`
idiom to dodge sign extension is not friendly. But it is consistent, documented,
and it works.

### Extended attributes

32-bit magic with the record length packed into the top byte
(`FNODE_EA_SIZE_SHIFT`, `struct.h:380`), skip-unknown parsing in `GET_EA`
(`struct.h:396`), so new EA types are forward-compatible with old drivers. This is
the right compat story and it is actually implemented, not merely promised.
`RESIZE_EA` (`struct.h:414`) and `REMOVE_EA` (`struct.h:440`) memmove the pool
correctly.

But 176 bytes total, and symlinks live *inside* the EA → **172-character symlink
limit** vs 4096 on ext4/XFS. A direct consequence of the "no extra indirection for
small files" philosophy taken slightly too far. Long symlinks are rare, but this is
a hard wall.

### Checksums — the weakest link

`__byte_sum` (`struct.h:947`) is an XOR-fold returning **8 bits**:

```c
unsigned long __sum = 0;
do {
        __sum ^= *(unsigned long *)__ptr;
        __ptr = (char *)__ptr + sizeof(unsigned long);
} while (__ptr < __e);
__sum ^= __sum >> 31 >> 1;
__sum ^= __sum >> 16;
__sum ^= __sum >> 8;
return __sum;
```

Pure XOR, 8-bit output. It detects ~255/256 of random single-bit corruption but
has **catastrophic blind spots**: any error that permutes or duplicates whole words
XORs to the same value. Byte-order-preserving swaps, reordered blocks, and
copy-forward corruption can pass cleanly.

And `APAGE_CHECKSUM_VALID` / `FNODE_BLOCK_CHECKSUM_VALID` gate whether it is even
consulted, and checksums are a mount-time and mkfs-time option (`--no-checksums`).

**For a filesystem whose entire safety model is "invalid structures are skipped,
corrupt structures are caught by checksum," an 8-bit XOR checksum is not
adequate.** This is the single change I would make first. `crc32c` is hardware
accelerated in the kernel, costs 4 bytes and negligible cycles, and improves
detection by orders of magnitude. The design has *excellent* crash semantics and
*poor* corruption-detection semantics underneath them. That mismatch is the most
dangerous thing in the tree.

**No data checksums at all.** Metadata only, and weakly. Silent data corruption is
undetectable. Fine for a scratch/performance filesystem; not fine for archival —
which the "no fsck needed" pitch actively invites people to assume.

### Compatibility engineering

`linux-compat.h` is ~350 lines of `LINUX_VERSION_CODE` conditionals spanning
2.6.9 → 7.2, plus `TEST_RHEL_VERSION`, `PASS_INODE`, and five different
`blkdev_issue_flush` signatures (`super.c:488-496`). Impressive maintenance, and
pure out-of-tree tax. Upstreaming would delete most of it.

---

## Ranked weaknesses

1. **8-bit XOR checksum under a design that trusts it absolutely.** Fix first —
   crc32c per 512 B unit.
2. **`cct[]` is a single point of failure** with no visible redundancy. A second
   copy of the table — or even just the current epoch's entry, mirrored — costs
   256 KiB and removes the worst failure mode.
3. **File size in the directory block** → append dirties the parent dir; contention
   on hot directories. Prealloc mitigates, does not solve.
4. **Hash quality unverified.** `*15` plus low-bits-first is a known-weak
   combination at the level that matters most. Benchmark before trusting.
5. **No ordered iteration.** Structural, not fixable without a second index.
6. **2^16 crash-count ceiling.** Real operational cliff; needs fsck to clear.
7. **172-char symlink cap.** Arbitrary consequence of EA sizing.
8. **No sparse files.** Documented and deliberate, but a real gap for VM images,
   databases, and git.
9. **No `open_by_handle_at` / NFS export.** Partly a Linux `ino_t` width problem,
   but the design chose not to fight it.

---

## Verdict

This is a **serious, coherent design**, not a hack. The crash-count scheme is a
legitimate alternative to journaling with real, measurable advantages — no replay,
no double-write, rotating commit-point wear — and the author understood exactly
where the bodies are buried: the dual-versioning of every mutable structure, the
monotone-append anode that avoids needing it, the reserved spare apages bounding
the randomized allocator's tail.

It is **not** a general-purpose successor to ext4. It is optimised for one shape of
workload:

> **many small files, create/delete-heavy, lookup-by-name, crash-tolerant, no
> recovery latency**

— build trees, package managers, mail spools, CI workspaces, ephemeral storage on
fleets that reboot into a mounted volume and cannot afford fsck. For that shape,
the fnode-in-directory + hashed-dnode + apage combination is close to optimal and
beats journaling decisively.

The design's own Achilles' heel is that its safety argument rests on ambient
validity — *"skip what isn't committed"* — while the thing that tells you whether a
structure is garbage is an 8-bit XOR sum. **The crash model is 2020s-grade; the
corruption model is 1990s-grade.** Fix the checksum, mirror the counter table, and
benchmark the hash, and the design becomes genuinely strong. Without those, a bad
cable doesn't lose you a file — it loses you the ability to tell which files you
have.
