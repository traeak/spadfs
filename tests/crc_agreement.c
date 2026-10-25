/*
 * Phase 2 gate: the kernel and userspace CRC implementations must agree, and
 * the trailer must land where every component thinks it does.
 *
 * This exists because a divergence here is silent and cross-component: mkfs
 * would write checksums the driver rejects, or worse, accept ones it should
 * not. It cannot be caught at compile time, so it is caught here instead.
 *
 * The kernel side cannot be executed without root, so this test verifies the
 * userspace implementation against the published CRC-32C check vectors and
 * against the same convention the kernel inline uses (~crc32c(~0, buf, len)).
 * tests/crc_kernel_xcheck.sh compares the kernel's own output to these values
 * when run with privileges.
 *
 * Build:  gcc -I<repo root> -o crc_agreement crc_agreement.c ../utils/crc32c.c
 */

#include <stdio.h>
#include <string.h>

#include "../meta_crc.h"

static int failures;

static void check(const char *what, unsigned got, unsigned want)
{
	int ok = got == want;

	printf("  %-46s %08x / %08x  %s\n", what, got, want,
	       ok ? "OK" : "MISMATCH");
	if (!ok)
		failures++;
}

static void check_bool(const char *what, int cond)
{
	printf("  %-46s %s\n", what, cond ? "OK" : "FAIL");
	if (!cond)
		failures++;
}

int main(void)
{
	unsigned char unit[SPADFS_META_UNIT_SIZE];
	unsigned char copy[SPADFS_META_UNIT_SIZE];
	int i;

	printf("CRC-32C published check vectors (Castagnoli, init ~0, xorout ~0)\n");
	check("\"123456789\"", spadfs_crc32c_std("123456789", 9), 0xe3069283u);
	check("\"a\"", spadfs_crc32c_std("a", 1), 0xc1d04330u);
	check("\"\" (empty)", spadfs_crc32c_std("", 0), 0x00000000u);

	printf("\nTrailer geometry\n");
	check("SPADFS_META_COVER", SPADFS_META_COVER, 508);
	check("SPADFS_META_CRC_POS", SPADFS_META_CRC_POS, 508);

	/* Deterministic non-zero fill so nothing passes by accident. */
	for (i = 0; i < SPADFS_META_UNIT_SIZE; i++)
		unit[i] = (unsigned char)(i * 7 + 1);

	printf("\nSeal / verify round-trip\n");
	spadfs_meta_seal(unit);
	check_bool("sealed unit verifies", spadfs_meta_verify(unit) == 0);

	/*
	 * Regression guard for a real bug: an earlier revision had the kernel
	 * and userspace put_crc() disagreeing on whether the argument was the
	 * unit start or the 4-byte field. seal() then wrote the CRC over payload
	 * bytes 0..3 and left the trailer untouched -- which verify() happened
	 * to still reject, so only this positional check catches it.
	 */
	check("payload byte 0 untouched by seal", unit[0], 1);
	check("payload byte 3 untouched by seal", unit[3], 22);
	check("trailer is non-zero after seal",
	    spadfs_meta_get_crc(unit + SPADFS_META_CRC_POS) != 0, 1);

	printf("\nTamper detection -- classes the old XOR checksum was blind to\n");

	memcpy(copy, unit, sizeof copy);
	copy[100] ^= 0x01;
	check_bool("single-bit flip", spadfs_meta_verify(copy) != 0);

	memcpy(copy, unit, sizeof copy);
	{
		unsigned char t = copy[20];
		copy[20] = copy[21];
		copy[21] = t;
	}
	check_bool("adjacent byte transpose (XOR-invisible by proof)",
		 spadfs_meta_verify(copy) != 0);

	memcpy(copy, unit, sizeof copy);
	{
		unsigned char t[8];
		memcpy(t, copy + 32, 8);
		memcpy(copy + 32, copy + 40, 8);
		memcpy(copy + 40, t, 8);
	}
	check_bool("word swap (XOR-invisible)", spadfs_meta_verify(copy) != 0);

	memcpy(copy, unit, sizeof copy);
	{
		/*
		 * Flip the same bit in two words: XOR of all bytes preserved,
		 * so __byte_sum could never see this.
		 */
		copy[16] ^= 0x01;
		copy[24] ^= 0x01;
	}
	check_bool("xor_cancel (XOR-preserving)", spadfs_meta_verify(copy) != 0);

	memcpy(copy, unit, sizeof copy);
	memcpy(copy + 128, copy + 64, 64);
	check_bool("block copy", spadfs_meta_verify(copy) != 0);

	/*
	 * The CRC is over bytes 0..507 only, so changing the trailer itself must
	 * be detected, and changing it to the recomputed value must NOT be --
	 * that is the restripe property Phase 2.5 relies on.
	 */
	memcpy(copy, unit, sizeof copy);
	copy[SPADFS_META_CRC_POS] ^= 0xFF;
	check_bool("trailer tamper detected", spadfs_meta_verify(copy) != 0);

	printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
	if (failures)
		printf("%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}
