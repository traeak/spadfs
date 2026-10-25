/*
 * Userspace CRC-32C (Castagnoli), matching the kernel's crc32c() semantics.
 *
 * Linked by both mkspadfs2 and spadfsck2. The kernel uses its own
 * hardware-accelerated crc32c(); this exists so the userspace tools compute
 * byte-identical checksums. Agreement between the two is not optional -- a
 * divergence here silently makes every volume written by mkfs unreadable by
 * the driver, or vice versa.
 *
 * Convention (from linux/crc32.h): the kernel's crc32c() does NOT invert at
 * either end. The standard CRC-32C is init ~0 with a final invert, so
 * spadfs_crc32c_std() supplies both, matching what the kernel-side inline in
 * meta_crc.h does.
 *
 *   polynomial   0x82f63b78  (reflected Castagnoli)
 *   bit order    least-significant-bit-first
 *   check        ~crc32c_raw(~0, "123456789", 9) == 0xe3069283
 */

#include <stddef.h>
#include <stdint.h>

#include "crc32c.h"

#define CRC32C_POLY_REFLECTED 0x82f63b78U

/*
 * Table generated once on first use rather than written out as 1 KiB of
 * literals: same speed after warm-up, and the generator is auditable against
 * the polynomial in a way a wall of hex is not.
 */
static uint32_t crc32c_table[256];
static int crc32c_table_ready;

static void crc32c_fill_table(void)
{
	unsigned i, j;

	for (i = 0; i < 256; i++) {
		uint32_t c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? ((c >> 1) ^ CRC32C_POLY_REFLECTED) : (c >> 1);
		crc32c_table[i] = c;
	}
	crc32c_table_ready = 1;
}

/* Raw kernel-compatible crc32c: no inversion at either end. */
uint32_t spadfs_crc32c_raw(uint32_t crc, const void *p, size_t len)
{
	const unsigned char *b = (const unsigned char *)p;

	if (!crc32c_table_ready)
		crc32c_fill_table();

	while (len--)
		crc = (crc >> 8) ^ crc32c_table[(crc ^ *b++) & 0xFF];
	return crc;
}

/* Standard CRC-32C: init ~0, final invert. Must equal the kernel's inline. */
uint32_t spadfs_crc32c_std(const void *buf, size_t len)
{
	return ~spadfs_crc32c_raw(~0U, buf, len);
}

void spadfs_meta_put_crc(uint8_t *unit, uint32_t crc)
{
	unit[0] = (uint8_t)(crc & 0xFF);
	unit[1] = (uint8_t)((crc >> 8) & 0xFF);
	unit[2] = (uint8_t)((crc >> 16) & 0xFF);
	unit[3] = (uint8_t)((crc >> 24) & 0xFF);
}

uint32_t spadfs_meta_get_crc(const uint8_t *unit)
{
	return (uint32_t)unit[0]
	     | ((uint32_t)unit[1] << 8)
	     | ((uint32_t)unit[2] << 16)
	     | ((uint32_t)unit[3] << 24);
}
