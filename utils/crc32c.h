#ifndef SPADFS_UTILS_CRC32C_H
#define SPADFS_UTILS_CRC32C_H

/*
 * Userspace CRC-32C (Castagnoli), byte-compatible with the kernel's crc32c().
 * Implemented in crc32c.c; declared here for mkspadfs2 and spadfsck2.
 *
 * See crc32c.c for the convention and the published check vector this is
 * verified against.
 */

#include <stddef.h>
#include <stdint.h>

/* Raw, kernel-compatible: no inversion at either end. */
uint32_t spadfs_crc32c_raw(uint32_t crc, const void *p, size_t len);

/* Standard CRC-32C: init ~0, final invert. Matches the kernel-side inline. */
uint32_t spadfs_crc32c_std(const void *buf, size_t len);

/* Trailer accessors: little-endian u32 at the given 4-byte position. */
void     spadfs_meta_put_crc(uint8_t *unit, uint32_t crc);
uint32_t spadfs_meta_get_crc(const uint8_t *unit);

#endif
