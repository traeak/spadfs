#ifndef _SPADFS_NAME_HASH_H
#define _SPADFS_NAME_HASH_H

/*
 * The directory name hash: keyed SipHash-2-4 of the case-folded name.
 *
 * A directory is an extendible hash over a 32-bit name hash, consumed low
 * bits first (dir.c). v1.0.22 used hash = hash * 15 + (c & 0xdf): it spreads
 * real names as well as a cryptographic hash does (tests/hashsim.py), but it
 * is linear and unkeyed, so names with identical hashes are trivial to build
 * ("aP" and "bA" hash alike, and such pairs combine). 16 384 of them put a
 * directory into one leaf chain: hundreds of page reads per lookup instead
 * of three. Anyone who chooses file names in a shared directory could do it.
 *
 * Keyed with a random per-volume seed (struct superblock, hash_seed), the
 * hash cannot be predicted without the seed, so colliding names cannot be
 * computed in advance. SipHash-2-4 is the standard keyed hash for exactly
 * this (the kernel uses it for its own hash tables). It is implemented here,
 * not taken from <linux/siphash.h>, so that the driver, mkspadfs and
 * spadfsck compile the same code; tests/hash_agreement.c checks it against
 * the published test vectors.
 *
 * Case folding (& 0xdf per byte) is unchanged: names that differ only in
 * case still hash alike, which case-insensitive lookups need. The 64-bit
 * result is folded to 32 bits (hi ^ lo).
 *
 * The seed is 16 raw bytes read little-endian into the two key words, so the
 * hash does not depend on the volume's byte_sex.
 */

#include <linux/types.h>

#define SPADFS_HASH_SEED_SIZE	16

static inline __u64 spadfs_hash_rotl(__u64 x, int b)
{
	return (x << b) | (x >> (64 - b));
}

static inline __u64 spadfs_hash_le64(const __u8 *p, __u8 fold)
{
	__u64 v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | (__u8)(p[i] & fold);
	return v;
}

static inline void spadfs_hash_key(const __u8 *seed, __u64 key[2])
{
	key[0] = spadfs_hash_le64(seed, 0xff);
	key[1] = spadfs_hash_le64(seed + 8, 0xff);
}

#define SPADFS_SIPROUND(v0, v1, v2, v3)					\
do {									\
	v0 += v1; v1 = spadfs_hash_rotl(v1, 13); v1 ^= v0;		\
	v0 = spadfs_hash_rotl(v0, 32);					\
	v2 += v3; v3 = spadfs_hash_rotl(v3, 16); v3 ^= v2;		\
	v0 += v3; v3 = spadfs_hash_rotl(v3, 21); v3 ^= v0;		\
	v2 += v1; v1 = spadfs_hash_rotl(v1, 17); v1 ^= v2;		\
	v2 = spadfs_hash_rotl(v2, 32);					\
} while (0)

/*
 * SipHash-2-4 of len bytes, each ANDed with fold first: 0xdf folds case for
 * names, 0xff hashes the bytes as they are (the test vectors).
 */
static inline __u64 spadfs_siphash24(const __u64 key[2], const __u8 *p,
				     unsigned len, __u8 fold)
{
	__u64 v0 = 0x736f6d6570736575ULL ^ key[0];
	__u64 v1 = 0x646f72616e646f6dULL ^ key[1];
	__u64 v2 = 0x6c7967656e657261ULL ^ key[0];
	__u64 v3 = 0x7465646279746573ULL ^ key[1];
	__u64 b = (__u64)len << 56;
	unsigned i;

	for (; len >= 8; len -= 8, p += 8) {
		__u64 m = spadfs_hash_le64(p, fold);

		v3 ^= m;
		SPADFS_SIPROUND(v0, v1, v2, v3);
		SPADFS_SIPROUND(v0, v1, v2, v3);
		v0 ^= m;
	}
	for (i = 0; i < len; i++)
		b |= (__u64)(__u8)(p[i] & fold) << (8 * i);
	v3 ^= b;
	SPADFS_SIPROUND(v0, v1, v2, v3);
	SPADFS_SIPROUND(v0, v1, v2, v3);
	v0 ^= b;
	v2 ^= 0xff;
	SPADFS_SIPROUND(v0, v1, v2, v3);
	SPADFS_SIPROUND(v0, v1, v2, v3);
	SPADFS_SIPROUND(v0, v1, v2, v3);
	SPADFS_SIPROUND(v0, v1, v2, v3);
	return v0 ^ v1 ^ v2 ^ v3;
}

/* the 32-bit directory hash of a name */
static inline __u32 spadfs_name_hash(const __u64 key[2], const char *name,
				     unsigned len)
{
	__u64 h = spadfs_siphash24(key, (const __u8 *)name, len, 0xdf);

	return (__u32)(h ^ (h >> 32));
}

#endif
