/*
 * The directory name hash (name_hash.h) against the published SipHash-2-4
 * test vectors, plus the properties the directory relies on.
 *
 * The driver, mkspadfs and spadfsck all compile name_hash.h, so this one
 * userspace build checks the code all three run. If it drifted from
 * SipHash-2-4, nothing else would notice: every reader would agree with every
 * other, wrongly, and the keyed hash would lose the analysis it is chosen
 * for.
 *
 * Vectors: key 00 01 .. 0f, message 00 01 .. (len - 1), from the SipHash
 * paper and its reference vectors.h.
 */

#include <stdio.h>
#include <string.h>

#include "../name_hash.h"

static int failures;

static void check(const char *what, unsigned long long got, unsigned long long want)
{
	int ok = got == want;
	printf("  %-46s %s", what, ok ? "OK" : "FAIL");
	if (!ok) {
		printf(" (got %016llx want %016llx)", got, want);
		failures++;
	}
	printf("\n");
}

static void check_bool(const char *what, int cond)
{
	printf("  %-46s %s\n", what, cond ? "OK" : "FAIL");
	if (!cond)
		failures++;
}

int main(void)
{
	static const unsigned long long want[] = {
		0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL,
		0x0d6c8009d9a94f5aULL, 0x85676696d7fb7e2dULL,
		0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL,
		0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL,
		0x93f5f5799a932462ULL, 0x9e0082df0ba9e4b0ULL,
	};
	__u8 seed[16], msg[16];
	__u64 key[2], key2[2];
	unsigned i;
	char name[64];

	for (i = 0; i < 16; i++)
		seed[i] = msg[i] = i;
	spadfs_hash_key(seed, key);

	printf("SipHash-2-4 published vectors (key 00..0f, message 00..len-1)\n");
	for (i = 0; i < sizeof want / sizeof want[0]; i++) {
		char what[32];
		snprintf(what, sizeof what, "len %u", i);
		check(what, spadfs_siphash24(key, msg, i, 0xff), want[i]);
	}
	check("len 15 (paper)", spadfs_siphash24(key, msg, 15, 0xff), 0xa129ca6149be45e5ULL);

	printf("\nDirectory properties\n");
	check_bool("case folds: \"Makefile\" == \"MAKEFILE\"",
		   spadfs_name_hash(key, "Makefile", 8) == spadfs_name_hash(key, "MAKEFILE", 8));
	check_bool("the linear collision is gone: \"aP\" != \"bA\"",
		   spadfs_name_hash(key, "aP", 2) != spadfs_name_hash(key, "bA", 2));
	memcpy(seed, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x11", 16);
	spadfs_hash_key(seed, key2);
	strcpy(name, "some-file-name.txt");
	check_bool("the seed changes the hash",
		   spadfs_name_hash(key, name, strlen(name)) != spadfs_name_hash(key2, name, strlen(name)));

	printf("\n%s\n", failures ? "FAILURES" : "ALL PASS");
	return failures ? 1 : 0;
}
