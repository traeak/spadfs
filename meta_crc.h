#ifndef SPADFS_META_CRC_H
#define SPADFS_META_CRC_H

/*
 * Uniform metadata integrity trailer (Phase 2, option B).
 *
 * Every 512-byte metadata unit carries its checksum in the LAST four bytes:
 *
 *   bytes   0 .. 507   payload
 *   bytes 508 .. 511   CRC-32C over bytes 0..507, little-endian
 *
 * Why a positional trailer rather than a widened per-struct field:
 *
 *   - No existing field offset moves. Widening a per-struct __u8 checksum
 *     shifts every later field. (The fnode_block is the exception: its
 *     fnode chain must tile to byte 512, so it cannot take a trailer, and its
 *     header grew from 24 to 32 bytes to hold a CRC -- see the inner-field
 *     helpers at the end of this file.)
 *
 *   - The checksum needs no field at all, so it covers structures that have
 *     none. The crash-count table previously had no checksum anywhere
 *     (super.c wrote it with a plain memcpy); here it is covered by the same
 *     two calls as everything else.
 *
 *   - One helper pair serves every unit, instead of eight structs each keeping
 *     its own "flags & checksum must match" coupling.
 *
 * Cost: 4 bytes of payload per 512-byte unit.
 *
 * The CRC is computed over raw bytes and stored little-endian, so it is
 * independent of the volume's byte_sex. That is deliberate: endianness is a
 * property of the fields, not of this trailer, and fixing the order here means
 * a big-endian volume and a little-endian one agree on what the bytes mean.
 */

#define SPADFS_META_UNIT_SIZE	512
#define SPADFS_META_CRC_BYTES	4
#define SPADFS_META_COVER	(SPADFS_META_UNIT_SIZE - SPADFS_META_CRC_BYTES)	/* 508 */
#define SPADFS_META_CRC_POS	(SPADFS_META_UNIT_SIZE - SPADFS_META_CRC_BYTES)

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/crc32c.h>
/* asm/unaligned.h was moved to linux/unaligned.h; the old path no longer
 * exists as of the kernels at this floor. */
#include <linux/unaligned.h>

/*
 * Kernel crc32c() does NOT invert at either end (see linux/crc32.h). The
 * standard CRC-32C convention is init ~0 with a final invert, so we supply
 * both. Verified against the published check vector:
 *   spadfs_crc32c_std("123456789", 9) == 0xe3069283
 */
static inline __u32 spadfs_crc32c_std(const void *buf, size_t len)
{
	return ~crc32c(~0U, buf, len);
}

/* Raw, chainable: no inversion at either end (utils/crc32c.c matches). */
static inline __u32 spadfs_crc32c_raw(__u32 crc, const void *buf, size_t len)
{
	return crc32c(crc, buf, len);
}

/*
 * Low-level trailer accessors. These take a pointer to the 4-byte FIELD
 * itself, not to the start of the unit -- the offset lives only in
 * seal/verify below.
 *
 * Both implementations must agree on this exactly. An earlier revision had the
 * kernel adding SPADFS_META_CRC_POS internally while the userspace version did
 * not, so seal() wrote the CRC over payload bytes 0..3 and left the trailer
 * untouched. That is silent, catastrophic and cross-component: caught by
 * tests/crc_agreement.c, which is why that test gates this work.
 */
static inline void spadfs_meta_put_crc(__u8 *field, __u32 crc)
{
	put_unaligned_le32(crc, field);
}

static inline __u32 spadfs_meta_get_crc(const __u8 *field)
{
	return get_unaligned_le32(field);
}

#else /* userspace: mkspadfs / spadfsck */

#include <stddef.h>
#include <stdint.h>

uint32_t spadfs_crc32c_std(const void *buf, size_t len);
uint32_t spadfs_crc32c_raw(uint32_t crc, const void *buf, size_t len);

/* Same field-pointer convention as the kernel inlines above. */
void     spadfs_meta_put_crc(uint8_t *field, uint32_t crc);
uint32_t spadfs_meta_get_crc(const uint8_t *field);

#endif

/*
 * Seal a unit: compute the CRC over the payload and store it in the trailer.
 * The trailer is excluded from the computation, so its prior contents do not
 * matter -- no need to zero it first.
 */
static inline void spadfs_meta_seal(void *unit)
{
	unsigned char *u = (unsigned char *)unit;

	spadfs_meta_put_crc(u + SPADFS_META_CRC_POS,
			   spadfs_crc32c_std(u, SPADFS_META_COVER));
}

/* Verify a unit. Returns 0 when intact, non-zero when corrupt. */
/*
 * Cross-branch alias for the CRC type. The kernel branch has __u32 and the
 * userspace branch has uint32_t; the common section below needs to name the
 * type for the reporting helpers, so it is aliased once here rather than
 * duplicated per branch. Placed after the #endif so both sides have already
 * pulled in their own definition.
 */
#ifdef __KERNEL__
typedef __u32 spadfs_crc_t;
#else
typedef uint32_t spadfs_crc_t;
#endif

/*
 * The two values that disagreed, for error messages. Splitting these out keeps
 * call sites readable -- a bare "bad checksum: 00" from the old XOR fold told
 * you almost nothing; stored-vs-computed tells you whether the trailer was
 * zeroed, garbage, or stale.
 */
static inline spadfs_crc_t spadfs_meta_stored_crc(const void *unit)
{
	const unsigned char *u = (const unsigned char *)unit;

	return spadfs_meta_get_crc(u + SPADFS_META_CRC_POS);
}

static inline spadfs_crc_t spadfs_meta_computed_crc(const void *unit)
{
	const unsigned char *u = (const unsigned char *)unit;

	return spadfs_crc32c_std(u, SPADFS_META_COVER);
}

static inline int spadfs_meta_verify(const void *unit)
{
	const unsigned char *u = (const unsigned char *)unit;

	return spadfs_meta_get_crc(u + SPADFS_META_CRC_POS) ==
		       spadfs_crc32c_std(u, SPADFS_META_COVER) ? 0 : -1;
}

/*
 * A CRC field INSIDE the unit rather than at its end, for the fnode_block:
 * its chain of fnodes must tile to byte 512, so the CRC lives in the header.
 * The CRC-32C covers every byte of the unit except the 4-byte field itself,
 * i.e. [0, pos) and [pos + 4, 512), in that order -- the same as a CRC over
 * the unit with the field removed. Standard convention (init ~0, final
 * invert), stored little-endian like the trailer.
 */
static inline spadfs_crc_t spadfs_meta_inner_computed_crc(const void *unit,
							  unsigned pos)
{
	const unsigned char *u = (const unsigned char *)unit;
	spadfs_crc_t crc = spadfs_crc32c_raw(~0U, u, pos);

	crc = spadfs_crc32c_raw(crc, u + pos + SPADFS_META_CRC_BYTES,
				SPADFS_META_UNIT_SIZE - pos - SPADFS_META_CRC_BYTES);
	return ~crc;
}

static inline spadfs_crc_t spadfs_meta_inner_stored_crc(const void *unit,
							unsigned pos)
{
	return spadfs_meta_get_crc((const unsigned char *)unit + pos);
}

static inline void spadfs_meta_inner_seal(void *unit, unsigned pos)
{
	spadfs_meta_put_crc((unsigned char *)unit + pos,
			    spadfs_meta_inner_computed_crc(unit, pos));
}

/* Returns 0 when intact, non-zero when corrupt. */
static inline int spadfs_meta_inner_verify(const void *unit, unsigned pos)
{
	return spadfs_meta_inner_stored_crc(unit, pos) ==
		       spadfs_meta_inner_computed_crc(unit, pos) ? 0 : -1;
}

#endif
