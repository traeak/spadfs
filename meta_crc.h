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
 *   - No existing field offset moves. SIZEOF_FNODE_BLOCK stays 24, so the
 *     make_dir_off_t / dir_off_t_pos bit-packing that encodes directory
 *     positions on disk is unaffected. Widening the in-struct __u8 checksum
 *     would have shifted every subsequent field and required re-deriving those
 *     macros in lockstep, with nothing in the tree checking that it was done.
 *
 *   - The checksum needs no field at all, so it covers structures that have
 *     none. The crash-count table previously had no checksum anywhere
 *     (super.c wrote it with a plain memcpy); here it is covered by the same
 *     two calls as everything else.
 *
 *   - One helper pair serves every unit, instead of eight structs each keeping
 *     its own "flags & checksum must match" coupling.
 *
 * Cost: 4 bytes of payload per 512-byte unit (FNODE_MAX_SIZE 488 -> 484).
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
static inline int spadfs_meta_verify(const void *unit)
{
	const unsigned char *u = (const unsigned char *)unit;

	return spadfs_meta_get_crc(u + SPADFS_META_CRC_POS) ==
		       spadfs_crc32c_std(u, SPADFS_META_COVER) ? 0 : -1;
}

#endif
