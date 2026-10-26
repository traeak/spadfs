#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

__s32 cct[65536];

static int cct_valid;
__u16 max_cc;

int check_cct(void)
{
	int r;
	unsigned i, j;

	int ccerr, dataerr, secerr;

	cct_valid = 0;
	max_cc = 0;

	status_printf(0, "checking crash count table...");

	memset(cct, 0, 4 * 65536);
	for (i = 0; i <= SPAD2CPU16_LV(&tx.cc); i++) CPU2SPAD32_LV(&cct[i], 0x7fffffff);

	if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.cct), CCT_SIZE >> SSIZE_BITS))) {
		if (r == 1) log_printf(0, "CRASH COUNT TABLE ALLOCATION ERROR: %s", alloc_error);
		return 0;
	}

	ccerr = 0;
	dataerr = 0;
	secerr = 0;
	for (i = 0; i < CCT_SIZE / 4; i += (1 << SSIZE_BITS) / 4) {
		__u32 p[128];
		__u32 *pp;
		int invl_elem;
		print_progress(CCT_SIZE / 4, i, 0);
		if (__unlikely(!(pp = read_buffer(SPAD2CPU64_LV(&super.cct) + i / ((1 << SSIZE_BITS) / 4), 1, BUFFER_FREE_SOON)))) {
			if (ccerr < MAX_SAME_ERRORS) {
				log_printf(0, "CAN'T READ CRASH COUNT TABLE SECTOR %d (ABS %"blk_format"X): %s", i / ((1 << SSIZE_BITS) / 4), SPAD2CPU64_LV(&super.cct) + i / ((1 << SSIZE_BITS) / 4), buffer_error);
				if (++ccerr == MAX_SAME_ERRORS) log_suppress();
			}
			continue;
		}
		memcpy(p, pp, 512);
		invl_elem = 0;
		for (j = 0; j < (1 << SSIZE_BITS) / 4; j++) {
			if (__unlikely(SPAD2CPU32_LV(&p[j]) & 0x80000000U) || (__likely(i + j > SPAD2CPU16_LV(&tx.cc)) && __unlikely(SPAD2CPU32_LV(&p[j])))) {
				if (i + j <= SPAD2CPU16_LV(&tx.cc)) CPU2SPAD32_LV(&p[j], 0x7fffffff);
				else CPU2SPAD32_LV(&p[j], 0);
				invl_elem++;
			}
		}
		if (__unlikely(invl_elem)) {
			if (invl_elem <= 2) {
				if (dataerr < MAX_SAME_ERRORS) {
					log_printf(0, "INVALID ENTRIES IN CRASH COUNT TABLE SECTOR %d (ABS %"blk_format"X): %d ERRORS", i / ((1 << SSIZE_BITS) / 4), SPAD2CPU64_LV(&super.cct) + i / ((1 << SSIZE_BITS) / 4), invl_elem);
					if (++dataerr == MAX_SAME_ERRORS) log_suppress();
				}
			} else {
				if (secerr < MAX_SAME_ERRORS) {
					log_printf(0, "INVALID DATA IN CRASH COUNT TABLE SECTOR %d (ABS %"blk_format"X)", i / ((1 << SSIZE_BITS) / 4), SPAD2CPU64_LV(&super.cct) + i / ((1 << SSIZE_BITS) / 4));
					if (++secerr == MAX_SAME_ERRORS) log_suppress();
				}
				goto skip_memcpy;
			}
		}
		memcpy(cct + i, p, (1 << SSIZE_BITS));
		skip_memcpy:;
	}

	if (__unlikely(ccerr | dataerr | secerr | !tx_valid)) {
		if (__unlikely(r = free_blocks(SPAD2CPU64_LV(&super.cct), CCT_SIZE >> SSIZE_BITS))) return r;
	} else {
		cct_valid = 1;
	}

	return 0;
}

/*
 * Verify the crash count table against its side CRC table (Phase 2.3, A5).
 *
 * This closes the original finding. Before the side table existed the cct had
 * NO integrity coverage at all: it was copied with a plain spadfs_memcpy and
 * never checksummed, so corrupting it -- including flipping the sign that
 * CC_VALID tests -- left spadfsck reporting a clean filesystem. See
 * tests/BASELINE-cct-integrity.md.
 *
 * Two layers, and both are required:
 *
 *   1. each CRC unit's own trailer. Skip this and the CRC table becomes the
 *      new unprotected link -- corrupt it and either every sector looks bad,
 *      or a crafted table blesses a corrupt cct.
 *   2. each cct sector against its stored CRC.
 *
 * If layer 1 fails we stop rather than proceed to layer 2: comparing the cct
 * against CRCs we do not trust would produce meaningless verdicts in both
 * directions.
 *
 * Verification is deliberately offline. The kernel does NOT check this at
 * mount, because the cct is written on every commit and a mount-time check
 * would turn an ordinary crash between the cct write and the CRC write into
 * an unmountable volume.
 */
int check_cct_crc(void)
{
	static __u32 stored[CCT_SECTORS];
	unsigned u, i;
	int r;
	int bad_units = 0, bad_sectors = 0;

	status_printf(0, "checking crash count table checksums...");

	if (SPAD2CPU64_LV(&super.cct_crc) == 0) {
		log_printf(0, "NO CCT CRC REGION IN SUPERBLOCK");
		return 0;
	}

	/*
	 * Reserve the region, exactly as check_cct does for the cct itself.
	 * Without this fsck sees the CRC blocks as used-in-map but claimed by
	 * nothing, and reports them lost:
	 *   FILESYSTEM HAS 8 LOST SECTORS IN 1 RUNS
	 * The count is CCT_CRC_UNITS rounded up to whole blocks, matching what
	 * mkspadfs reserves for the region.
	 */
	{
		blk_t block_sectors = (blk_t)1 << super.sectors_per_block_bits;
		blk_t span = ((blk_t)CCT_CRC_UNITS + block_sectors - 1)
			   / block_sectors * block_sectors;
		if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.cct_crc), span))) {
			if (r == 1) log_printf(0, "CCT CRC REGION ALLOCATION ERROR: %s", alloc_error);
			return 0;
		}
	}

	for (u = 0; u < CCT_CRC_UNITS; u++) {
		unsigned char *unit = read_buffer(SPAD2CPU64_LV(&super.cct_crc) + u, 1,
						BUFFER_FREE_SOON);
		if (__unlikely(!unit)) {
			log_printf(0, "CANT READ CCT CRC UNIT %u: %s", u, buffer_error);
			return 0;
		}
		if (__unlikely(spadfs_meta_verify(unit))) {
			if (bad_units < MAX_SAME_ERRORS) {
				log_printf(0, "CCT CRC UNIT %u CHECKSUM MISMATCH", u);
				if (++bad_units == MAX_SAME_ERRORS) log_suppress();
			} else {
				bad_units++;
			}
			continue;
		}
		for (i = 0; i < CCT_CRC_PER_UNIT; i++) {
			unsigned sector = u * CCT_CRC_PER_UNIT + i;
			if (sector >= CCT_SECTORS)
				break;
			stored[sector] = spadfs_meta_get_crc(unit + i * 4);
		}
	}

	if (__unlikely(bad_units)) {
		log_printf(0, "CCT CRC TABLE UNTRUSTWORTHY (%d BAD UNIT(S)); CANNOT VERIFY CCT",
			   bad_units);
		return 1;
	}

	for (i = 0; i < CCT_SECTORS; i++) {
		unsigned char *sec = read_buffer(SPAD2CPU64_LV(&super.cct) + i, 1,
					       BUFFER_FREE_SOON);
		if (__unlikely(!sec)) {
			log_printf(0, "CANT READ CCT SECTOR %u: %s", i, buffer_error);
			return 0;
		}
		if (__unlikely(spadfs_crc32c_std(sec, 512) != stored[i])) {
			if (bad_sectors < MAX_SAME_ERRORS) {
				log_printf(0, "CCT SECTOR %u DOES NOT MATCH ITS CHECKSUM", i);
				if (++bad_sectors == MAX_SAME_ERRORS) log_suppress();
			} else {
				bad_sectors++;
			}
		}
	}

	if (__unlikely(bad_sectors)) {
		log_printf(0, "CRASH COUNT TABLE CORRUPT: %d SECTOR(S) FAIL CHECKSUM", bad_sectors);
		return 1;
	}

	return 0;
}

/*
 * Phase 3.1: the crash-count table backup mirror.
 *
 * Two jobs, both of which must be done before anything tries to use the
 * mirror:
 *
 *  1. Reserve it. Same trap the primary CRC region taught us -- blocks that
 *     are used in the allocation map but claimed by nothing get reported as
 *       FILESYSTEM HAS 8 LOST SECTORS IN 1 RUNS
 *     so both the mirror and its CRC region must be claimed explicitly.
 *
 *  2. Prove the mirror's own CRC units are sealed. A backup that cannot be
 *     validated is not a backup, and discovering that at restore time is the
 *     wrong moment. Full restore logic is Task 3.2; this establishes that the
 *     region is present, aligned, and self-describing.
 *
 * A volume whose superblock carries no cct_backup pointer is simply one
 * formatted without the feature; that is not an error, so return clean.
 */
int check_cct_backup(void)
{
	blk_t block_sectors = (blk_t)1 << super.sectors_per_block_bits;
	blk_t crc_span = ((blk_t)CCT_BACKUP_CRC_UNITS + block_sectors - 1)
			 / block_sectors * block_sectors;
	__u64 backup = SPAD2CPU64_LV(&super.cct_backup);
	__u64 backup_crc = SPAD2CPU64_LV(&super.cct_backup_crc);
	unsigned u;
	int r;
	int bad_units = 0;

	status_printf(0, "checking crash count table backup...");

	if (!backup || !backup_crc) {
		log_printf(0, "NO CCT BACKUP REGION IN SUPERBLOCK");
		return 0;
	}

	/*
	 * Block alignment is a hard requirement, not a preference. An unaligned
	 * region boundary is what produced
	 *   APAGE 0 (4080) HAS UNALIGNED ENTRY 0030 (42C5 - 42C8)
	 * back in Phase 2. Catch it here rather than let the allocator trip on it.
	 */
	if (__unlikely(backup & (block_sectors - 1) ||
		     backup_crc & (block_sectors - 1))) {
		log_printf(0, "CCT BACKUP REGION IS NOT BLOCK ALIGNED (%"blk_format"X, %"blk_format"X)",
			   backup, backup_crc);
		return 1;
	}

	if (__unlikely(r = alloc_blocks(backup, CCT_BACKUP_SECTORS))) {
		if (r == 1) log_printf(0, "CCT BACKUP REGION ALLOCATION ERROR: %s", alloc_error);
		return 0;
	}
	if (__unlikely(r = alloc_blocks(backup_crc, crc_span))) {
		if (r == 1) log_printf(0, "CCT BACKUP CRC REGION ALLOCATION ERROR: %s", alloc_error);
		return 0;
	}

	/*
	 * Verify the mirror's CRC units carry a valid seal. We deliberately do
	 * not compare the mirror against the primary here -- that is Task 3.2's
	 * restore decision, and it has to reason about how far the two have
	 * diverged, which is a different question from "is this region sound".
	 */
	for (u = 0; u < CCT_BACKUP_CRC_UNITS; u++) {
		unsigned char *unit = read_buffer(backup_crc + u, 1, BUFFER_FREE_SOON);

		if (__unlikely(!unit)) {
			log_printf(0, "CANT READ CCT BACKUP CRC UNIT %u: %s", u, buffer_error);
			return 0;
		}
		if (__unlikely(spadfs_meta_verify(unit))) {
			if (bad_units < MAX_SAME_ERRORS) {
				log_printf(0, "CCT BACKUP CRC UNIT %u CHECKSUM MISMATCH", u);
				if (++bad_units == MAX_SAME_ERRORS) log_suppress();
			} else {
				bad_units++;
			}
		}
	}

	if (__unlikely(bad_units)) {
		log_printf(0, "CCT BACKUP CRC TABLE UNTRUSTWORTHY (%d BAD UNIT(S))", bad_units);
		return 1;
	}

	return 0;
}

int recover_cct_allocs(void)
{
	if (cct_valid) {
		int r;
		if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.cct), CCT_SIZE >> SSIZE_BITS))) {
			if (r == 1) log_printf(1, "FAILED TO RECOVER CRASH COUNT TABLE ALLOCATION: %s", alloc_error);
			return 2;
		}
	}
	return 0;
}

int fix_cct(void)
{
	if (__unlikely(!cct_valid)) {
		int r;
		if (!tx_valid) {
			int i;
			CPU2SPAD16_LV(&tx.cc, max_cc < 0xffff ? max_cc + 1 : 0xffff);
			for (i = SPAD2CPU16(tx.cc) + 1; i < CCT_SIZE / 4; i++) CPU2SPAD32_LV(&cct[i], 0);
		}
		if (!ro) status_printf(0, "fixing crash count table...");
		r = alloc_try(SPAD2CPU64_LV(&super.cct), CCT_SIZE >> SSIZE_BITS);
		if (!r) {
			if (!query(qprefix 0, "OVERWRITE CRASH COUNT TABLE?")) return 0;
		} else {
			blk_t new_cct;
			if (__unlikely(r > 0)) return r;
			if (!query(qprefix 0, "ALLOC NEW CRASH COUNT TABLE?")) return 0;
			if (__unlikely(r = alloc_space(1, "CRASH COUNT TABLE", CCT_SIZE >> SSIZE_BITS, 0, 1, &new_cct))) return r;
			CPU2SPAD64_LV(&super.cct, new_cct);
			write_super();
		}
		cct_valid = 1;
		if (reset_cc) goto do_reset;
		goto do_write;
	}
	if (reset_cc) {
		status_printf(0, "resetting crash count table...");
		do_reset:
		memset(cct, 0, CCT_SIZE);
		CPU2SPAD16_LV(&tx.cc, 0);
		write_txblock();
		do_write:
		write_buffer(SPAD2CPU64_LV(&super.cct), CCT_SIZE >> SSIZE_BITS, cct);
	}
	return 0;
}

__finline__ int cc_valid(__u16 cc, __s32 txc)
{
	cc = SPAD2CPU16(cc);
	txc = SPAD2CPU32(txc);
	return (__s32)((__u32)SPAD2CPU32_LV(&cct[cc]) - (__u32)txc) >= 0;
}

int cc_check(__u16 *cc, __s32 *txc, char *str1, const char *(*get_filename)(void *p), void *p, blk_t blk)
{
	const char *msg;
	if (__unlikely(SPAD2CPU16_LV(&*cc) > SPAD2CPU16_LV(&tx.cc))) {
		const char *fn;
		msg = "CRASH COUNT IS POINTING FORWARD";
		err:
		fn = NULL;
		if (get_filename) fn = get_filename(p);
		if (!fn) fn = "";
		log_printf(0, "%s (TXC=%08X, CC=%04X, CCT[CC]=%08X, TXC.CC=%04X) FOR %s%s%s, BLOCK %"blk_format"X", msg, SPAD2CPU32_LV(&*txc), SPAD2CPU16_LV(&*cc), SPAD2CPU32_LV(&cct[SPAD2CPU16_LV(&*cc)]), SPAD2CPU16_LV(&tx.cc), str1, *fn ? " " : "", fn, blk);
		if (query(qprefix 0, "FIX?")) goto fix_it;
		goto ret_z;
	}
	if (__unlikely((__u32)(SPAD2CPU32_LV(&*txc) & 0x7fffffff) > (__u32)SPAD2CPU32_LV(&cct[SPAD2CPU16_LV(&*cc)]) + (SPAD2CPU16_LV(&*cc) < SPAD2CPU16_LV(&tx.cc)))) {
		msg = "TRANSACTION COUNT IS TOO HIGH";
		goto err;
	}
	if (__likely(!reset_cc)) {
		__u16 new_max_cc = SPAD2CPU16_LV(&*cc);
		if (__unlikely(new_max_cc > max_cc)) {
			if (!cct_valid && new_max_cc >= 0x8000) {
				msg = "CRASH COUNT IS TOO HIGH";
				goto err;
			}
			max_cc = new_max_cc;
		}
		goto ret_z;
	}
	if (!(SPAD2CPU16_LV(&*cc) | (SPAD2CPU32_LV(&*txc) & 0x7fffffff))) return 0;
	CPU2SPAD32_LV(txc, (~(__u32)cc_valid(*cc, *txc)) << 31);
	CPU2SPAD16_LV(cc, 0);
	return 1;
	fix_it:
	/* in case of error, it is more likely that crash count table has been
	   damaged (for example overwritten with zeros). Act as if there was no
	   crash count table */
	CPU2SPAD32_LV(&*txc, SPAD2CPU32_LV(&*txc) & 0x80000000U);
	CPU2SPAD16_LV(&*cc, 0);
	return 1;

	ret_z:
	return 0;
}

