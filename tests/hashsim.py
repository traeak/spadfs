#!/usr/bin/env python3
"""Does the directory hash cluster? A model of the directory index, no mount.

    hashsim.py [--page 4096] [--fnode-page 4096] [-n N] [--corpus NAME ...]

A spadfs directory is an extendible hash over a 32-bit name hash, consumed
LOW BITS FIRST (dir.c, spadfs_find_leaf_page_for_hash / test_hash_bit):

  - a dnode page holds 2^D slots, D = page_bits - 4 (8 for 4 KiB pages);
    the slot is the next D bits of the hash
  - a slot points at a leaf (an fnode page) or a dnode one level down; a
    leaf can be shared by several slots and splits on the next single bit
    when an insert does not fit
  - a leaf whose slot range is a single slot, at a depth that is a multiple
    of D, gets a new dnode below it instead
  - once all 32 bits are used, an overflowing leaf is chained

With inserts only, the final shape does not depend on insertion order: a
trie node ends up split exactly when everything hashed into it does not fit
in one leaf. So the shape is computed directly, recursively, per hash.

Leaf capacity: the fnode page's 512-byte blocks hold 480 bytes of fnodes each
(SIZEOF_FNODE_BLOCK 32); an entry is align8(56 + namelen) + 32 (the
fnode header, the name, and the 32-byte UNX extended attribute every file
gets). Entries never span a block; first-fit packing per leaf.

Reported per corpus and hash function:
  leaves   leaf pages (fewer is better: fuller leaves)
  fill     entry bytes / (leaves * leaf capacity)
  reads    page reads to find a name: dnodes on its path + its leaf +
           its position in a chain; mean and max over all names
  chained  leaves beyond the first in hash-exhausted chains (32 bits used
           up, or identical 32-bit hashes)
  coll     names sharing a full 32-bit hash with another name

"ideal" is a cryptographic hash (BLAKE2b, 32 bits): what perfect spreading
looks like at this capacity, for comparison.
"""

import argparse
import hashlib
import os
import random
import sys
import uuid

MASK = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# hash functions; all fold case the way the format requires (& 0xdf per byte)
# ---------------------------------------------------------------------------

def h_spadfs(b):
    """name_len_hash, struct.h: hash = hash * 15 + (c & 0xdf)."""
    h = 0
    for c in b:
        h = (h * 15 + (c & 0xDF)) & MASK
    return h


def fmix32(h):
    """murmur3's finaliser: full avalanche, cheap, invertible."""
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & MASK
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & MASK
    h ^= h >> 16
    return h


def h_spadfs_fmix(b):
    return fmix32(h_spadfs(b))


def h_fnv1a(b):
    h = 0x811C9DC5
    for c in b:
        h ^= c & 0xDF
        h = (h * 0x01000193) & MASK
    return h


def h_ideal(b):
    return int.from_bytes(hashlib.blake2b(bytes(c & 0xDF for c in b),
                                          digest_size=4).digest(), "little")


HASHES = [("spadfs", h_spadfs), ("spadfs+fmix32", h_spadfs_fmix),
          ("fnv1a", h_fnv1a), ("ideal", h_ideal)]


# ---------------------------------------------------------------------------
# the directory model
# ---------------------------------------------------------------------------

BLOCK_PAYLOAD = 480        # 512 - SIZEOF_FNODE_BLOCK
FNODE_HEAD = 56            # sizeof(struct fnode)
EA_UNX = 32                # sizeof(struct ea_unx)


def entry_size(namelen):
    return ((FNODE_HEAD + namelen + 7) & ~7) + EA_UNX


def fits(sizes, blocks):
    """First-fit pack into `blocks` blocks of BLOCK_PAYLOAD bytes."""
    free = [BLOCK_PAYLOAD] * blocks
    for s in sizes:
        for i in range(blocks):
            if free[i] >= s:
                free[i] -= s
                break
        else:
            return False
    return True


def chain_len(sizes, blocks):
    """Leaves needed when the hash is exhausted and leaves are chained."""
    n, cur = 0, []
    for s in sizes:
        if not fits(cur + [s], blocks):
            n += 1
            cur = []
        cur.append(s)
    return n + 1


class Model:
    def __init__(self, page, fnode_page):
        self.D = page.bit_length() - 1 - 4           # dnode_hash_bits
        self.blocks = fnode_page // 512
        self.cap = self.blocks * BLOCK_PAYLOAD

    def shape(self, items):
        """items: list of (hash, size). Returns stats dict."""
        st = {"leaves": 0, "chained": 0, "dnodes": 0, "bytes": 0,
              "reads": [], "maxdepth": 0}

        def walk(items, depth, dnodes_above):
            sizes = [s for _, s in items]
            if fits(sizes, self.blocks):
                st["leaves"] += 1
                st["bytes"] += sum(sizes)
                st["reads"].append((len(items), dnodes_above + 1))
                st["maxdepth"] = max(st["maxdepth"], depth)
                return
            if depth >= 32:
                n = chain_len(sizes, self.blocks)
                st["leaves"] += n
                st["chained"] += n - 1
                st["bytes"] += sum(sizes)
                # position in the chain: spread evenly for the mean
                per = len(items) / n
                for k in range(n):
                    st["reads"].append((per, dnodes_above + 1 + k))
                return
            if depth % self.D == 0:
                st["dnodes"] += 1
                dnodes_above += 1
            zero = [it for it in items if not (it[0] >> depth) & 1]
            one = [it for it in items if (it[0] >> depth) & 1]
            for part in (zero, one):
                if part:
                    walk(part, depth + 1, dnodes_above)

        walk(items, 0, 0)
        n = sum(c for c, _ in st["reads"])
        st["mean_reads"] = sum(c * r for c, r in st["reads"]) / n
        st["max_reads"] = max(r for _, r in st["reads"])
        st["fill"] = st["bytes"] / (st["leaves"] * self.cap)
        return st


# ---------------------------------------------------------------------------
# name corpora
# ---------------------------------------------------------------------------

def corpus(name, n, rng):
    if name == "numbered":          # file0000000 ...
        return ["file%07d" % i for i in range(n)]
    if name == "camera":            # IMG_0001.JPG, several prefixes
        out = []
        for p in ("IMG_", "DSC", "DSCN", "P10"):
            out += ["%s%05d.JPG" % (p, i) for i in range(n // 4)]
        return out
    if name == "hex":               # git object / content-hash names
        return ["%038x" % rng.getrandbits(152) for _ in range(n)]
    if name == "uuid":
        return [str(uuid.UUID(int=rng.getrandbits(128), version=4)) for _ in range(n)]
    if name == "maildir":           # time.Mseq_Ppid.host,S=size:2,flags
        t0 = 1700000000
        return ["%d.M%dP%d.mailhost,S=%d:2,%s" % (t0 + i // 7, rng.randrange(10**6),
                rng.randrange(1, 99999), rng.randrange(500, 90000), rng.choice(["S", "RS", ""]))
                for i in range(n)]
    if name == "prefix-suffix":     # long shared prefix and suffix, number in the middle
        return ["libexample-component-%d-x86_64-linux-gnu.so.1" % i for i in range(n)]
    if name == "collide":           # crafted: identical spadfs hash for every name
        # The hash is linear: raising one byte by 1 and lowering the next by
        # 15 leaves it unchanged ("aP" and "bA" hash alike). k independent
        # such pairs give 2^k names with one hash -- anyone who chooses file
        # names (uploads, shared /tmp) can build them.
        k = max(1, min(20, (n - 1).bit_length()))
        return ["x" + "".join("bA" if (i >> j) & 1 else "aP" for j in range(k)) + ".dat"
                for i in range(min(n, 1 << k))]
    if name == "system":            # real basenames from /usr
        seen = set()
        for root, dirs, files in os.walk("/usr"):
            for f in files + dirs:
                seen.add(f)
                if len(seen) >= n:
                    break
            if len(seen) >= n:
                break
        return sorted(seen)
    raise ValueError(name)


CORPORA = ["system", "numbered", "camera", "hex", "uuid", "maildir", "prefix-suffix", "collide"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--page", type=int, default=4096, help="page size (dnode slots)")
    ap.add_argument("--fnode-page", type=int, default=4096, help="leaf (fnode page) size")
    ap.add_argument("-n", type=int, default=100000, help="names per directory")
    ap.add_argument("--corpus", nargs="*", default=CORPORA)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    m = Model(a.page, a.fnode_page)
    rng = random.Random(a.seed)
    print("page %d (D = %d bits per dnode), leaf %d bytes usable, up to %d names per corpus\n"
          % (a.page, m.D, m.cap, a.n))
    print("%-14s %-14s %7s %7s %6s %6s %5s %7s %6s" %
          ("corpus", "hash", "names", "leaves", "fill", "reads", "max", "chained", "coll"))
    for c in a.corpus:
        names = [s.encode("utf-8", "surrogateescape") for s in corpus(c, a.n, rng)]
        for hn, hf in HASHES:
            hs = [hf(b) for b in names]
            items = list(zip(hs, (entry_size(len(b)) for b in names)))
            st = m.shape(items)
            coll = len(hs) - len(set(hs))
            print("%-14s %-14s %7d %7d %5.0f%% %6.2f %5d %7d %6d" %
                  (c, hn, len(names), st["leaves"], 100 * st["fill"], st["mean_reads"],
                   st["max_reads"], st["chained"], coll))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
