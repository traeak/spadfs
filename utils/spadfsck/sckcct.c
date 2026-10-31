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
/*
 * Task 3.1/3.2 state. Set by the two check functions, consumed by
 * repair_cct_backup(). Kept as file-scope flags rather than return codes
 * because the checks run at different points in the pass and the repair has
 * to see both outcomes at once.
 */
static int cct_primary_bad_flag = 1;	/* unknown until check_cct_crc() runs */
static int cct_backup_present;	/* region exists and is block-aligned */
static int cct_backup_bad = 1;	/* present but failed verification */
static int cct_backup_usable;	/* present and fully verified good */

/* Task 3.3: the backup's epoch stamp, set by check_cct_backup() */
static int cct_backup_stamped;	/* a valid stamp that agrees with the data */
static unsigned cct_backup_stamp_cc;
static __u32 cct_backup_stamp_txc;

/* scratch copy of a whole cct, raw disk-endian words like cct[] */
static __u32 cct_scratch[CCT_SIZE / 4];

/*
 * Read a whole cct-shaped region into dst. Sector by sector, because
 * read_buffer hands back a pointer into a pool that later reads recycle.
 */
static int read_cct_region(__u64 base, __u32 *dst, const char *what)
{
	unsigned i;

	for (i = 0; i < CCT_SECTORS; i++) {
		unsigned char *b = read_buffer(base + i, 1, 0);

		if (__unlikely(!b)) {
			log_printf(0, "CANT READ %s SECTOR %u: %s", what, i, buffer_error);
			return 1;
		}
		memcpy((char *)dst + (unsigned long)i * 512, b, 512);
	}
	return 0;
}

/*
 * Write a whole cct (src, raw disk-endian) to a data region, then compute and
 * write its CRC units. With stamped, the last unit carries the epoch stamp
 * (struct.h): cc is the volume's current tx.cc and txc is src[cc], i.e. the
 * stamp describes exactly the bytes written. The kernel stamps the cc it is
 * working in, which is tx.cc - 1 while mounted and tx.cc after a clean
 * unmount; fsck stamps tx.cc, which the next mount will work in.
 *
 * Every place fsck rewrites a cct goes through here, so data and CRCs can
 * never be written for different bytes. crc == 0 means the volume has no CRC
 * region; only the data is written.
 */
static int write_cct_copy(__u64 data, __u64 crc, const __u32 *src,
			  int stamped, const char *what)
{
	static unsigned char unit[512];
	unsigned u, i;

	if (__unlikely(write_buffer(data, CCT_SECTORS, src))) {
		log_printf(0, "CANT WRITE %s AT %"blk_format"X: %s", what, (blk_t)data, buffer_error);
		return 1;
	}
	if (!crc)
		return 0;
	for (u = 0; u < CCT_CRC_UNITS; u++) {
		memset(unit, 0, sizeof unit);
		for (i = 0; i < CCT_CRC_PER_UNIT; i++) {
			unsigned sector = u * CCT_CRC_PER_UNIT + i;

			if (sector >= CCT_SECTORS)
				break;
			spadfs_meta_put_crc(unit + i * 4,
					    spadfs_crc32c_std((const char *)src + (unsigned long)sector * 512, 512));
		}
		if (stamped && u == CCT_BACKUP_STAMP_UNIT) {
			struct cct_backup_stamp *st =
				(struct cct_backup_stamp *)(unit + CCT_BACKUP_STAMP_OFF);
			unsigned cc = SPAD2CPU16_LV(&tx.cc);

			CPU2SPAD32_LV(&st->magic, CCT_BACKUP_STAMP_MAGIC);
			CPU2SPAD32_LV(&st->cc, cc);
			CPU2SPAD32_LV(&st->txc, SPAD2CPU32_LV(&src[cc]));
			CPU2SPAD32_LV(&st->reserved, 0);
		}
		spadfs_meta_seal(unit);
		if (__unlikely(write_buffer(crc + u, 1, unit))) {
			log_printf(0, "CANT WRITE %s CRC UNIT %u: %s", what, u, buffer_error);
			return 1;
		}
	}
	return 0;
}

/*
 * Is the backup from the crash epoch the volume is in now?
 *
 * Within an epoch only cct[cc] moves, and only upward, one commit at a time.
 * A backup a commit or two behind therefore holds exactly an earlier commit
 * point, and restoring it is a clean rollback -- what a crash does anyway.
 * A backup from an older epoch is missing whole epochs' worth of entries;
 * restoring it makes everything written since read INVALID and reverts it.
 *
 * Current means stamp.cc is tx.cc (clean unmount, or fsck stamped it) or
 * tx.cc - 1 (the kernel's working cc while mounted, or after a crash).
 */
static int cct_backup_epoch_current(void)
{
	unsigned cc = SPAD2CPU16_LV(&tx.cc);

	return cct_backup_stamped && tx_valid &&
	       cct_backup_stamp_cc <= cc && cct_backup_stamp_cc + 1 >= cc;
}

/* Mark the mirror as just rewritten from a known-good cct. */
static void cct_backup_now_current(const __u32 *src)
{
	unsigned cc = SPAD2CPU16_LV(&tx.cc);

	cct_backup_present = 1;
	cct_backup_bad = 0;
	cct_backup_usable = 1;
	cct_backup_stamped = 1;
	cct_backup_stamp_cc = cc;
	cct_backup_stamp_txc = SPAD2CPU32_LV(&src[cc]);
}

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
		cct_primary_bad_flag = 1;
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
		cct_primary_bad_flag = 1;
		return 1;
	}

	cct_primary_bad_flag = 0;
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
/*
 * Phase 3.1 + 3.2: the crash-count table backup mirror.
 *
 * Full two-layer verification, mirroring check_cct_crc():
 *
 *   layer 1  the mirror's CRC units must themselves carry a valid seal
 *   layer 2  every mirror sector must match its stored CRC
 *
 * Layer 1 alone is not enough -- a unit can be perfectly sealed around a set
 * of CRCs that no longer describe the data. Both layers have to pass before
 * the mirror is eligible to restore from.
 *
 * A volume whose superblock carries no cct_backup pointer is simply one
 * formatted without the feature; that is not an error.
 */
int check_cct_backup(void)
{
	static __u32 stored[CCT_BACKUP_SECTORS];
	blk_t block_sectors = (blk_t)1 << super.sectors_per_block_bits;
	blk_t crc_span = ((blk_t)CCT_BACKUP_CRC_UNITS + block_sectors - 1)
			 / block_sectors * block_sectors;
	__u64 backup = SPAD2CPU64_LV(&super.cct_backup);
	__u64 backup_crc = SPAD2CPU64_LV(&super.cct_backup_crc);
	unsigned u, i;
	int r;
	int bad_units = 0, bad_sectors = 0;

	status_printf(0, "checking crash count table backup...");

	cct_backup_present = 0;
	cct_backup_bad = 1;
	cct_backup_usable = 0;
	cct_backup_stamped = 0;

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
	cct_backup_present = 1;

	if (__unlikely(r = alloc_blocks(backup, CCT_BACKUP_SECTORS))) {
		if (r == 1) log_printf(0, "CCT BACKUP REGION ALLOCATION ERROR: %s", alloc_error);
		return 0;
	}
	if (__unlikely(r = alloc_blocks(backup_crc, crc_span))) {
		if (r == 1) log_printf(0, "CCT BACKUP CRC REGION ALLOCATION ERROR: %s", alloc_error);
		return 0;
	}

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
			continue;
		}
		for (i = 0; i < CCT_CRC_PER_UNIT; i++) {
			unsigned sector = u * CCT_CRC_PER_UNIT + i;

			if (sector >= CCT_BACKUP_SECTORS)
				break;
			stored[sector] = spadfs_meta_get_crc(unit + i * 4);
		}
		/* Task 3.3: the stamp is sealed by this unit's trailer */
		if (u == CCT_BACKUP_STAMP_UNIT) {
			struct cct_backup_stamp *st =
				(struct cct_backup_stamp *)(unit + CCT_BACKUP_STAMP_OFF);

			if (SPAD2CPU32_LV(&st->magic) == CCT_BACKUP_STAMP_MAGIC &&
			    !SPAD2CPU32_LV(&st->reserved) &&
			    SPAD2CPU32_LV(&st->cc) < CCT_SIZE / 4) {
				cct_backup_stamped = 1;
				cct_backup_stamp_cc = SPAD2CPU32_LV(&st->cc);
				cct_backup_stamp_txc = SPAD2CPU32_LV(&st->txc);
			}
		}
	}

	if (__unlikely(bad_units)) {
		cct_backup_stamped = 0;
		log_printf(0, "CCT BACKUP CRC TABLE UNTRUSTWORTHY (%d BAD UNIT(S)); CANNOT VERIFY BACKUP",
			   bad_units);
		return 1;
	}

	for (i = 0; i < CCT_BACKUP_SECTORS; i++) {
		unsigned char *sec = read_buffer(backup + i, 1, BUFFER_FREE_SOON);

		if (__unlikely(!sec)) {
			log_printf(0, "CANT READ CCT BACKUP SECTOR %u: %s", i, buffer_error);
			return 0;
		}
		if (__unlikely(spadfs_crc32c_std(sec, 512) != stored[i])) {
			if (bad_sectors < MAX_SAME_ERRORS) {
				log_printf(0, "CCT BACKUP SECTOR %u DOES NOT MATCH ITS CHECKSUM", i);
				if (++bad_sectors == MAX_SAME_ERRORS) log_suppress();
			} else {
				bad_sectors++;
			}
		}
	}

	if (__unlikely(bad_sectors)) {
		cct_backup_stamped = 0;
		log_printf(0, "CCT BACKUP CORRUPT: %d SECTOR(S) FAIL CHECKSUM", bad_sectors);
		return 1;
	}

	/*
	 * The stamp must describe the data it sits next to: stamp.txc is the
	 * value the writer put in cct[stamp.cc]. Both are sealed, so a mismatch
	 * is not bit rot: it is a mirror update torn by a crash (on 512-byte
	 * blocks the stamp unit is a separate write from the data's CRC unit)
	 * or a writer bug. Either way the backup's age is unknown.
	 */
	if (cct_backup_stamped) {
		unsigned char *sec = read_buffer(backup + cct_backup_stamp_cc / 128, 1,
						 BUFFER_FREE_SOON);

		if (__unlikely(!sec)) {
			log_printf(0, "CANT READ CCT BACKUP SECTOR %u: %s",
				   cct_backup_stamp_cc / 128, buffer_error);
			return 0;
		}
		if (__unlikely(SPAD2CPU32_LV(&((__u32 *)sec)[cct_backup_stamp_cc % 128]) !=
			       cct_backup_stamp_txc)) {
			log_printf(0, "CCT BACKUP STAMP DOES NOT MATCH ITS DATA (CC %X: STAMP %08X, DATA %08X)",
				   cct_backup_stamp_cc, (unsigned)cct_backup_stamp_txc,
				   (unsigned)SPAD2CPU32_LV(&((__u32 *)sec)[cct_backup_stamp_cc % 128]));
			cct_backup_stamped = 0;
		}
	}

	cct_backup_bad = 0;
	cct_backup_usable = 1;
	return 0;
}

/*
 * Task 3.2 + 3.3: the recovery decision matrix.
 *
 * primary | backup             | action
 * --------+--------------------+------------------------------------------
 * good    | good, current      | nothing
 * good    | good, out of date  | refresh the mirror from the primary
 * good    | bad                | recreate the mirror from the primary
 * good    | absent             | create the mirror
 * bad     | good, same epoch   | restore (auto-answered by -y)
 * bad     | good, older/no stamp | restore only on an explicit YES
 * bad     | bad                | no non-destructive recovery; fall back to reset
 * bad     | absent             | same -- reset
 *
 * WHY THE STAMP DECIDES WHETHER -y MAY RESTORE
 *
 * CC_VALID(cc, txc) is (s32)(cct[cc] - txc) >= 0. Restoring an older cct
 * makes every structure written since read INVALID and promotes its older
 * alternate: files created since vanish, deleted files return, renames
 * unwind. How much is lost depends entirely on how old the backup is.
 *
 * The kernel keeps the mirror one flush behind the primary (Task 3.3), so a
 * backup from the current epoch is at most the commit in progress behind --
 * restoring it is the same rollback a crash performs, and is safe to do
 * unattended. A backup from an older epoch, or one with no stamp, is the
 * silent-reversion case and stays Q_IMPORTANT: a human must say YES.
 *
 * The common way to get here is a crash during a commit, between the cct
 * write and its CRC landing. That commit was never acknowledged, so falling
 * back to the backup loses nothing that was promised.
 */
int repair_cct_backup(void)
{
	int r;
	unsigned cc = SPAD2CPU16_LV(&tx.cc);

	if (!cct_primary_bad_flag && cct_backup_usable) {
		/*
		 * Both verify. Verification proves each copy is internally
		 * consistent, never that the mirror is current, so compare.
		 */
		static __u32 backup_copy[CCT_SIZE / 4];

		if (read_cct_region(SPAD2CPU64_LV(&super.cct), cct_scratch, "CCT"))
			return 0;
		if (read_cct_region(SPAD2CPU64_LV(&super.cct_backup), backup_copy, "CCT BACKUP"))
			return 0;
		if (__likely(!memcmp(cct_scratch, backup_copy, CCT_SIZE)) &&
		    __likely(cct_backup_epoch_current()))
			return 0;
		if (cct_backup_stamped)
			log_printf(0, "CCT BACKUP IS OUT OF DATE (BACKUP CC %X TXC %08X, VOLUME CC %X TXC %08X)",
				   cct_backup_stamp_cc, (unsigned)cct_backup_stamp_txc,
				   cc, (unsigned)SPAD2CPU32_LV(&cct_scratch[cc]));
		else
			log_printf(0, "CCT BACKUP IS OUT OF DATE (NO EPOCH STAMP)");
		if (query(qprefix Q_NOFIX, "REFRESH CCT BACKUP FROM THE INTACT PRIMARY?")) {
			if (write_cct_copy(SPAD2CPU64_LV(&super.cct_backup), SPAD2CPU64_LV(&super.cct_backup_crc),
					   cct_scratch, 1, "CCT BACKUP"))
				return 0;
			cct_backup_now_current(cct_scratch);
			log_printf(0, "CCT BACKUP REFRESHED");
		}
		return 0;
	}

	if (__unlikely(cct_primary_bad_flag && cct_backup_bad)) {
		log_printf(0, "BOTH THE CRASH COUNT TABLE AND ITS BACKUP ARE DAMAGED");
		log_printf(0, "NO NON-DESTRUCTIVE RECOVERY IS AVAILABLE");
		log_printf(0, "FALLING BACK TO A FULL TABLE RESET, WHICH DISCARDS THE");
		log_printf(0, "CRASH-COUNT EPOCH MAPPING FOR EVERY MOUNT SINCE THE BACKUP");
		return 0;
	}

	if (cct_primary_bad_flag && cct_backup_usable) {
		int yes;

		if (cct_backup_epoch_current()) {
			log_printf(0, "CRASH COUNT TABLE IS DAMAGED BUT ITS BACKUP VERIFIES");
			log_printf(0, "THE BACKUP IS FROM THE CURRENT CRASH EPOCH (CC %X TXC %08X, VOLUME CC %X)",
				   cct_backup_stamp_cc, (unsigned)cct_backup_stamp_txc, cc);
			log_printf(0, "RESTORING IT ROLLS BACK AT MOST THE COMMIT IN PROGRESS");
			yes = query(qprefix 0, "RESTORE CRASH COUNT TABLE FROM BACKUP?");
		} else {
			log_printf(0, "CRASH COUNT TABLE IS DAMAGED BUT ITS BACKUP VERIFIES");
			if (!cct_backup_stamped)
				log_printf(0, "THE BACKUP HAS NO VALID EPOCH STAMP; ITS AGE CANNOT BE PROVED");
			else
				log_printf(0, "THE BACKUP IS FROM CRASH COUNT %X BUT THE VOLUME IS AT %X",
					   cct_backup_stamp_cc, cc);
			log_printf(0, "RESTORING AN OLDER BACKUP CAN SILENTLY REVERT FILES TO AN OLDER STATE");
			yes = query(qprefix Q_IMPORTANT, "RESTORE CRASH COUNT TABLE FROM BACKUP?");
		}
		if (!yes)
			return 0;

		/*
		 * Write the primary from the verified mirror, with CRCs computed
		 * from the same bytes, so the two halves cannot describe
		 * different data. The primary carries no stamp.
		 */
		if (read_cct_region(SPAD2CPU64_LV(&super.cct_backup), cct_scratch, "CCT BACKUP"))
			return 0;
		if (write_cct_copy(SPAD2CPU64_LV(&super.cct), SPAD2CPU64_LV(&super.cct_crc),
				   cct_scratch, 0, "CRASH COUNT TABLE"))
			return 0;

		/*
		 * Reload the in-memory table. check_cct() filled cct[] from the
		 * damaged primary, and everything downstream -- cc_check(),
		 * fix_txblock(), the directory walk -- reads it. cct[] holds raw
		 * disk-endian words, as does cct_scratch.
		 */
		memcpy(cct, cct_scratch, CCT_SIZE);
		/*
		 * check_cct() frees the cct's blocks in the allocation map when
		 * it rejects the table. Claim them back, or fix_apages() writes
		 * the cct region out as FREE and the kernel can later allocate
		 * over it. (Present since Task 3.2: the next pass reported
		 * "rebuilding apages" after every restore.)
		 */
		if (!cct_valid) {
			if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.cct), CCT_SIZE >> SSIZE_BITS))) {
				if (r == 1) log_printf(0, "CRASH COUNT TABLE ALLOCATION ERROR: %s", alloc_error);
				return r == 1 ? 0 : r;
			}
		}
		cct_valid = 1;
		cct_primary_bad_flag = 0;
		log_printf(0, "CRASH COUNT TABLE RESTORED FROM BACKUP");
		return 0;
	}

	if (!cct_primary_bad_flag && cct_backup_present && cct_backup_bad) {
		if (query(qprefix Q_NOFIX, "RECREATE CCT BACKUP FROM THE INTACT PRIMARY?")) {
			if (read_cct_region(SPAD2CPU64_LV(&super.cct), cct_scratch, "CCT"))
				return 0;
			if (write_cct_copy(SPAD2CPU64_LV(&super.cct_backup), SPAD2CPU64_LV(&super.cct_backup_crc),
					   cct_scratch, 1, "CCT BACKUP"))
				return 0;
			cct_backup_now_current(cct_scratch);
			log_printf(0, "CCT BACKUP RECREATED");
		}
		return 0;
	}

	if (!cct_primary_bad_flag && !cct_backup_present) {
		if (query(qprefix Q_NOFIX, "CREATE CCT BACKUP?")) {
			blk_t block_sectors = (blk_t)1 << super.sectors_per_block_bits;
			blk_t crc_span = ((blk_t)CCT_BACKUP_CRC_UNITS + block_sectors - 1)
					 / block_sectors * block_sectors;
			__u64 b, bc;

			if (read_cct_region(SPAD2CPU64_LV(&super.cct), cct_scratch, "CCT"))
				return 0;
			if (__unlikely(r = alloc_space(1, "CCT BACKUP", CCT_BACKUP_SECTORS, 0, 1, &b)))
				return r;
			if (__unlikely(r = alloc_space(1, "CCT BACKUP CRC", crc_span, 0, 1, &bc)))
				return r;
			/* data before the pointers: never point at an unwritten region */
			if (write_cct_copy(b, bc, cct_scratch, 1, "NEW CCT BACKUP"))
				return 0;
			CPU2SPAD64_LV(&super.cct_backup, b);
			CPU2SPAD64_LV(&super.cct_backup_crc, bc);
			write_super();
			/*
			 * Record that the backup now exists. Without this a second
			 * call saw "absent" again and allocated another one, leaking
			 * the first: CCT BACKUP CREATED twice, then LOST SECTORS.
			 */
			cct_backup_now_current(cct_scratch);
			log_printf(0, "CCT BACKUP CREATED AT %"blk_format"X", b);
		}
	}

	return 0;
}

/*
 * Keep the CRCs and the mirror in step with a cct fsck itself rewrote
 * (fix_cct: reset, overwrite or reallocation). Writing the data alone left
 * the primary failing its own CRCs on the next run -- e.g. after
 * --reset-crash-counts: CCT SECTOR 0 DOES NOT MATCH ITS CHECKSUM -- and the
 * mirror describing the table from before the rewrite.
 */
static void write_cct_everywhere(void)
{
	if (write_cct_copy(SPAD2CPU64_LV(&super.cct), SPAD2CPU64_LV(&super.cct_crc),
			   (const __u32 *)cct, 0, "CRASH COUNT TABLE"))
		return;
	cct_primary_bad_flag = 0;
	if (SPAD2CPU64_LV(&super.cct_backup) && SPAD2CPU64_LV(&super.cct_backup_crc)) {
		if (write_cct_copy(SPAD2CPU64_LV(&super.cct_backup), SPAD2CPU64_LV(&super.cct_backup_crc),
				   (const __u32 *)cct, 1, "CCT BACKUP"))
			return;
		cct_backup_now_current((const __u32 *)cct);
	}
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
		write_cct_everywhere();
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


/* Phase 3.2: verdict of check_cct_crc(), exposed for sck.c's control flow. */
int cct_primary_bad(void)
{
	return cct_primary_bad_flag;
}
