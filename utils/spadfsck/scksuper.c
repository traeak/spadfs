#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

int endian_big = 1;
__u64 super_ptr[64];
__u64 tx_ptr[64];
__u64 user_ptr[64];
unsigned block_mask;
unsigned dnode_hash_bits;
unsigned dnode_data_size;
unsigned dnode_page_sectors;

char tx_valid;

static char primary_valid;
static char backup_valid;
static char backup_accessible;
static char mark_dirty;

blk_t total_sectors_allocated = 0;

static __u64 detect_size(void)
{
	__u64 b = 1, c;
	if (__unlikely(!read_buffer(0, 1, BUFFER_NOCACHE | BUFFER_TOLERATE_ERROR))) return 0;
	while (read_buffer(b, 1, BUFFER_NOCACHE | BUFFER_TOLERATE_ERROR) && (b << 1 <= MAX_SIZE)) b <<= 1;
	b >>= 1;
	for (c = b >> 1; c; c >>= 1) if (read_buffer(b + c, 1, BUFFER_NOCACHE | BUFFER_TOLERATE_ERROR)) b |= c;
	return b + 1;
}

void write_super(void)
{
	super.checksum = 0;
	super.checksum = CHECKSUM_BASE ^ __byte_sum(&super, 512);
	write_buffer(SUPERBLOCK_SECTOR, 1, &super);
	primary_valid = 1;
	if (backup_accessible) {
		write_buffer(SPAD2CPU64_LV(&super.size), 1, &super);
		backup_valid = 1;
	}
}

void write_txblock(void)
{
	if (tx_valid) {
		CPU2SPAD32_LV(&tx.magic, magic(SPAD2CPU64_LV(&super.txblock), TXBLOCK_MAGIC));
		tx.checksum = 0;
		tx.checksum = CHECKSUM_BASE ^ __byte_sum(&tx, 512);
		write_buffer(SPAD2CPU64_LV(&super.txblock), 1, &tx);
	}
}

static void write_user(void)
{
	user.checksum = 0;
	user.checksum = CHECKSUM_BASE ^ __byte_sum(&user, 512);
	write_buffer(USERBLOCK_SECTOR, 1, &user);
}

void init_super(void)
{
	memset(&super, 0, 512);
	memset(&tx, 0, 512);
	memset(&user, 0, 512);
	block_mask = 0;
	tx_valid = 0;
	primary_valid = 0;
	backup_valid = 0;
	backup_accessible = 0;
	mark_dirty = 0;
}

static void fill_empty_txblock(void)
{
	memset(&tx, 0, 512);
	CPU2SPAD32_LV(&tx.magic, 0);
	CPU2SPAD16_LV(&tx.cc, 0xffff);
	CPU2SPAD16_LV(&tx.a_cc, 0);
	CPU2SPAD32_LV(&tx.a_txc, 0);
	CPU2SPAD32_LV(&tx.txflags, TXFLAGS_FS_ERROR);
}

static void fill_empty_userblock(void)
{
	memset(&user, 0, 512);
	CPU2SPAD32_LV(&user.magic, USERBLOCK_MAGIC);
	user.userflags = 0;
}

static int alloc_super(void)
{
	int r;
	if (!(SPAD2CPU32_LV(&super.flags_compat_none) & FLAG_COMPAT_NONE_EXTRA_SPACE)) {
		if (__unlikely(r = alloc_blocks(0, SUPERBLOCK_SECTOR + (1 << super.sectors_per_block_bits))))
			return r;
	} else {
		if (__unlikely(r = alloc_blocks(0, (BOOTLOADER_SECTORS + (1 << super.sectors_per_block_bits) - 1) & ~((1 << super.sectors_per_block_bits) - 1))))
			return r;
		if (__unlikely(r = alloc_blocks(USERBLOCK_SECTOR & ~((1 << super.sectors_per_block_bits) - 1), 1 << super.sectors_per_block_bits)))
			return r;
		if (__unlikely(r = alloc_blocks(SUPERBLOCK_SECTOR, 1 << super.sectors_per_block_bits)))
			return r;
	}
	return 0;
}

static int test_sb_at_sector(blk_t sector)
{
	int backup = sector != SUPERBLOCK_SECTOR;
	struct superblock *s;
	const char *m;
	if (__unlikely(!(s = read_buffer(sector, 1, 0)))) {
		log_printf(0, "UNABLE TO READ SUPER BLOCK AT %"blk_format"X: %s", sector, buffer_error);
		return 1;
	}
	memcpy(&super, s, 512);
	if (__unlikely(memcmp(super.signature, SUPERBLOCK_SIGNATURE, sizeof super.signature))) {
		if (!backup) log_printf(0, "SPADFS SUPERBLOCK NOT FOUND");
		return 1;
	}
	if (SPAD2CPU64_LV(&super.byte_sex) != __make64(0x89ABCDEF, 0x01234567)) {
		endian_big = !endian_big;
		if (SPAD2CPU64_LV(&super.byte_sex) != __make64(0x89ABCDEF, 0x01234567)) {
			log_printf(0, "SUPERBLOCK AT %"blk_format"X: UNKNOWN BYTE SEX (NEITHER BIG ENDIAN NOR LITTLE ENDIAN)", sector);
			return 1;
		}
	}
	if (__unlikely(super.version_major != SPADFS_VERSION_MAJOR)) {
		log_printf(0, "BAD %s VERSION NUMBER (DISK: %d.%d.%d, " TOOL_BANNER ": %d.%d.%d)", super.version_major != SPADFS_VERSION_MAJOR ? "MAJOR" : "MIDDLE", super.version_major, super.version_middle, super.version_minor, SPADFS_VERSION_MAJOR, SPADFS_VERSION_MIDDLE, SPADFS_VERSION_MINOR);
		return 2;
	}
	if (__unlikely(SPAD2CPU32_LV(&super.flags_compat_rw) & ~(0)) ||
	    __unlikely(SPAD2CPU32_LV(&super.flags_compat_ro) & ~(FLAG_COMPAT_RO_DIRECTORY_SIZES)) ||
	    __unlikely(SPAD2CPU32_LV(&super.flags_compat_none) & ~(FLAG_COMPAT_NONE_UNIX_NAMES | FLAG_COMPAT_NONE_DYNAMIC_MAGIC | FLAG_COMPAT_NONE_EXTRA_SPACE))) {
		log_printf(0, "INCOMPATIBLE FEATURE FLAGS IN SUPERBLOCK: %08X/%08X/%08X/%08X",
			(unsigned)SPAD2CPU32_LV(&super.flags_compat_fsck),
			(unsigned)SPAD2CPU32_LV(&super.flags_compat_rw),
			(unsigned)SPAD2CPU32_LV(&super.flags_compat_ro),
			(unsigned)SPAD2CPU32_LV(&super.flags_compat_none));
		return 2;
	}
	if (backup && SPAD2CPU64_LV(&super.size) != sector) {
		return 1;
	}
	if (__unlikely((m = validate_super(&super)) != NULL)) {
		log_printf(0, "INVALID PARAMETERS IN SUPERBLOCK AT %"blk_format"X: %s", sector, m);
		return 1;
	}
	if (__unlikely(super.sectors_per_page_bits > __BSR(BUFFER_SIZE))) {
		log_printf(0, "SECTOR_PER_PAGE_BITS %d NOT SUPPORTED (MAX SUPPORTED %d)", super.sectors_per_page_bits, __BSR(BUFFER_SIZE / 512));
		return 2;
	}
	block_mask = (1 << super.sectors_per_block_bits) - 1;
	dnode_hash_bits = SSIZE_BITS + super.sectors_per_page_bits - DNODE_PAGE_ENTRY_BITS - 1;
	dnode_data_size = DNODE_PAGE_ENTRY_SIZE << dnode_hash_bits;
	dnode_page_sectors = DNODE_ENTRY_OFFSET + (dnode_data_size << 1);
	dnode_page_sectors = (dnode_page_sectors + (1 << SSIZE_BITS << super.sectors_per_block_bits) - 1) & ~((1 << SSIZE_BITS << super.sectors_per_block_bits) - 1);
	dnode_page_sectors >>= SSIZE_BITS;
	return 0;
}

static void test_backup_superblock(void)
{
	struct superblock *s;
	if (__unlikely(!(s = read_buffer(SPAD2CPU64_LV(&super.size), 1, BUFFER_TOLERATE_ERROR)))) return;
	backup_accessible = 1;
	if (!memcmp(s, &super, 512)) backup_valid = 1;
}

static int check_sb1(void)
{
	__u64 ds;
	int r;
	int i;
	r = test_sb_at_sector(SUPERBLOCK_SECTOR);
	if (__unlikely(r > 1)) return r;
	if (__likely(!r)) {
		primary_valid = 1;
		test_backup_superblock();
		return 0;
	}
	ds = detect_size();
	if (ds < MIN_SIZE) {
		log_printf(0, "DEVICE IS TOO SMALL");
		return 1;
	}
	for (i = 1; i <= 1 << MAX_SECTORS_PER_BLOCK_BITS; i++) {
		__u64 s = ds - i;
		if (s < MIN_SIZE) break;
		r = test_sb_at_sector(s);
		if (!r) {
			log_printf(0, "FOUND BACKUP SUPERBLOCK AT %"blk_format"X", s);
			if (preen) {
				log_printf(1, "CAN'T REPAIR SUPERBLOCK IN AUTOMATIC MODE, RUN FSCK MANUALLY");
				return 1;
			}
			backup_valid = 1;
			backup_accessible = 1;
			return 0;
		}
		if (__unlikely(r > 1)) return r;
	}
	return 1;
}

static int check_sb2(void)
{
	int r;
	int did_not_fix = 0;
	if (__unlikely(__byte_sum(&super, 512) != CHECKSUM_BASE)) {
		log_printf(0, "BAD CHECKSUM ON SUPERBLOCK: %02X", __byte_sum(&super, 512) ^ CHECKSUM_BASE);
		if (query(qprefix 0, "FIX?")) {
			write_super();
			mark_dirty = 1;
		} else {
			did_not_fix = 1;
		}
		force = 1;
	}
	if (__unlikely(!primary_valid)) {
		log_printf(0, "PRIMARY SUPERBLOCK IS NOT VALID");
		if (query(qprefix 0, "RESTORE?")) {
			write_super();
			mark_dirty = 1;
		}
		force = 1;
	}
	if (__likely(!did_not_fix) && __likely(backup_accessible) && __unlikely(!backup_valid)) {
		if (query(qprefix Q_NOFIX, "CREATE BACKUP SUPERBLOCK?")) {
			write_super();
		}
	}
	if (__unlikely((SPAD2CPU64_LV(&super.size) << SSIZE_BITS << 1) != (io_t)(SPAD2CPU64_LV(&super.size) << SSIZE_BITS << 1))) {
		bad_system:
		log_printf(0, "YOUR SYSTEM HAS IO OFFSET LIMITED TO 32 BITS. CAN'T CHECK FILESYSTEM OF THIS SIZE");
		return 2;
	}
	if (__unlikely(SPAD2CPU64_LV(&super.size) != (blk_t)SPAD2CPU64_LV(&super.size))) {
		recompile:
		log_printf(0, "TOO BIG SIZE, RECOMPILE WITH 64-BIT BLK_T");
		return 2;
	}
	if (st_valid && S_ISREG(st.st_mode)) goto skip_sizetest;
	if (__unlikely(!read_buffer(SPAD2CPU64_LV(&super.size) - 1, 1, BUFFER_NOCACHE))) {
		__u64 ds;
		log_printf(0, "CAN'T READ END OF FILE SYSTEM: %s", buffer_error);
		ds = detect_size();
		if (__unlikely((ds << SSIZE_BITS << 1) != (io_t)(ds << SSIZE_BITS << 1))) goto bad_system;
		if (__unlikely(ds != (blk_t)ds)) goto recompile;
		log_printf(0, "SUPERBLOCK HAS SIZE %"blk_format"X, DETECTED SIZE %"blk_format"X", (blk_t)SPAD2CPU64_LV(&super.size), (blk_t)ds);
		if (ds < SPAD2CPU64_LV(&super.size)) {
			blk_t ss = SPAD2CPU64_LV(&super.size);
			CPU2SPAD64_LV(&super.size, (ds - (block_mask + 1)) & ~(blk_t)block_mask);
			if (__unlikely(validate_super(&super) != NULL)) {
				CPU2SPAD64_LV(&super.size, ss);
				log_printf(0, "DEVICE SIZE IS TOO SMALL - CAN'T RECOVER");
				return 1;
			}
#ifdef __linux__
			if (ss - ds < getpagesize() / 512)
				log_printf(1, "THIS ERROR CAN HAPPEN DUE TO BUG IN BUFFER CACHE IN OLDER LINUX KERNELS. DO NOT FIX IT AND TERMINATE " TOOL_BANNER " OR YOU MIGHT LOSE SOME DATA !!!");
#endif
			if (query(qprefix Q_IMPORTANT, "TRUNCATE? (WARNING!!! THIS OPERATION WILL DESTROY DATA IN INACCESSIBLE AREA OF DEVICE)")) {
				CPU2SPAD64_LV(&super.reserve_sectors, get_default_reserved(SPAD2CPU64_LV(&super.size)) & ~(blk_t)block_mask);
				backup_accessible = 1;
				write_super();
				force = 1;
				mark_dirty = 1;
			} else {
				CPU2SPAD64_LV(&super.size, ss);
				if (y) return 1;
				log_printf(1, "FURTHER ERRORS MIGHT BE CAUSED BY INACCESSIBLE AREA. IT IS NOT RECOMMENDED TO FIX THEM");
			}
		}
	}
	skip_sizetest:
	if (extend) {
		__u64 ds;
		unsigned old_apages, old_apage_index_sectors, new_apages, new_apage_index_sectors;
		__u64 needed_sectors;
		ds = detect_size();
		if (__unlikely(ds != (blk_t)ds)) goto recompile;
		if (ds < block_mask + 1) goto log_skip_extend;
		ds = (ds - (block_mask + 1)) & ~(blk_t)block_mask;
		if (ds <= SPAD2CPU64_LV(&super.size)) {
			log_skip_extend:
			log_printf(0, "NOT EXTENDING BECAUSE DEVICE SIZE WASN'T INCREASED");
			goto skip_extend;
		}
		old_apages = N_APAGES(SPAD2CPU64_LV(&super.size), super.sectors_per_page_bits + SSIZE_BITS, super.sectors_per_block_bits + SSIZE_BITS);
		old_apage_index_sectors = APAGE_INDEX_SECTORS(old_apages, 1 << SSIZE_BITS << super.sectors_per_block_bits);
		new_apages = N_APAGES(ds, super.sectors_per_page_bits + SSIZE_BITS, super.sectors_per_block_bits + SSIZE_BITS);
		new_apage_index_sectors = APAGE_INDEX_SECTORS(new_apages, 1 << SSIZE_BITS << super.sectors_per_block_bits);
		if (new_apages >= MAX_APAGES) {
			log_printf(0, "NOT EXTENDING BECAUSE DEVICE SIZE IS TOO BIG");
			goto skip_extend;
		}
		needed_sectors = (__u64)(new_apage_index_sectors - old_apage_index_sectors) << super.sectors_per_page_bits;
		if (needed_sectors)
			needed_sectors += (1 << super.sectors_per_page_bits) - (1 << super.sectors_per_block_bits);
		if (old_apage_index_sectors != new_apage_index_sectors)
			needed_sectors = 2 * (__u64)new_apage_index_sectors;
		if (ds - SPAD2CPU64_LV(&super.size) < needed_sectors) {
			log_printf(0, "NOT EXTENDING BECAUSE THE INCREASED SPACE IS TOO SMALL (%"blk_format"X SECTORS)", (blk_t)(ds - SPAD2CPU64_LV(&super.size)));
			goto skip_extend;
		}
		log_printf(0, "EXTENDING THE FILESYSTEM FROM %"blk_format"X TO %"blk_format"X SECTORS", (blk_t)SPAD2CPU64_LV(&super.size), (blk_t)ds);
		CPU2SPAD64_LV(&super.size, ds);
		backup_accessible = 1;
	}
	if (0)
		skip_extend: extend = 0;
	total_sectors_allocated = SPAD2CPU64_LV(&super.size);
	if (__unlikely(reserve >= 0)) {
		reserve >>= 9;
		reserve &= ~(__u64)((1 << super.sectors_per_block_bits) - 1);
		if (__unlikely(reserve > SPAD2CPU64_LV(&super.size))) {
			log_printf(0, "REQUESTING TOO MANY RESERVED SECTORS, IGNORED");
		} else {
			log_printf(0, "CHANGING THE NUMBER OF RESERVERD SECTORS FROM %"blk_format"X TO %"blk_format"X", (blk_t)SPAD2CPU64_LV(&super.reserve_sectors), (blk_t)reserve);
			CPU2SPAD64_LV(&super.reserve_sectors, reserve);
			write_super();
		}
		reserve = -1;
	}
	if (__unlikely(SPAD2CPU64_LV(&super.reserve_sectors) > SPAD2CPU64_LV(&super.size))) {
		log_printf(0, "INVALID NUMBER OF RESERVED SECTORS (%"blk_format"X) < SIZE (%"blk_format"X)", SPAD2CPU64_LV(&super.reserve_sectors), SPAD2CPU64_LV(&super.size));
		if (query(qprefix 0, "FIX?")) {
			CPU2SPAD64_LV(&super.reserve_sectors, get_default_reserved(SPAD2CPU64_LV(&super.size)) & ~(blk_t)block_mask);
			write_super();
			mark_dirty = 1;
		}
		force = 1;
	}
	if (__unlikely(r = init_swapfile())) return r;
	if (__unlikely(r = alloc_super())) {
		if (r == 1) log_printf(0, "SUPERBLOCK ALLOCATION ERROR: %s", alloc_error);
		return r;
	}
	return 0;
}

static int check_txb(void)
{
	int r;
	struct txblock *t;
	if (__unlikely(!(t = read_buffer(SPAD2CPU64_LV(&super.txblock), 1, 0)))) {
		log_printf(0, "UNABLE TO READ TX BLOCK %"blk_format"X: %s", SPAD2CPU64_LV(&super.txblock), buffer_error);
		tx_error:
		force = 1;
		fill_empty_txblock();
		goto return_ok;
	}
	if (__unlikely(SPAD2CPU32_LV(&t->magic) != magic(SPAD2CPU64_LV(&super.txblock), TXBLOCK_MAGIC))) {
		log_printf(0, "BAD MAGIC ON TX BLOCK %"blk_format"X", SPAD2CPU64_LV(&super.txblock));
		goto tx_error;
	}
	if (__unlikely(__byte_sum(t, 512) != CHECKSUM_BASE)) {
		log_printf(0, "BAD CHECKSUM ON TX BLOCK %"blk_format"X: %02X", SPAD2CPU64_LV(&super.txblock), __byte_sum(t, 512) ^ CHECKSUM_BASE);
		goto tx_error;
	}
	if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.txblock), (1 << super.sectors_per_block_bits)))) {
		if (r == 2) return 2;
		log_printf(0, "TX BLOCK ALLOCATION ERROR: %s", alloc_error);
		goto tx_error;
	}
	memcpy(&tx, t, 512);
	if (SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_DIRTY) log_printf(0, "FILESYSTEM HAS DIRTY FLAG");
	if (SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_IO_READ_ERROR) log_printf(0, "FILESYSTEM HAS I/O READ ERROR FLAG");
	if (SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_IO_WRITE_ERROR) log_printf(0, "FILESYSTEM HAS I/O WRITE ERROR FLAG");
	if (SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_FS_ERROR) log_printf(0, "FILESYSTEM HAS STRUCTURE ERROR FLAG");
	if (SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_CHECKSUM_ERROR) log_printf(0, "FILESYSTEM HAS CHECKSUM ERROR FLAG");
	if (SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_EA_ERROR) log_printf(0, "FILESYSTEM HAS EXTENDED ATTRIBUTE ERROR FLAG");
	if (SPAD2CPU32_LV(&tx.txflags) & ~(TXFLAGS_DIRTY | TXFLAGS_IO_READ_ERROR | TXFLAGS_IO_WRITE_ERROR | TXFLAGS_FS_ERROR | TXFLAGS_CHECKSUM_ERROR | TXFLAGS_EA_ERROR)) log_printf(0, "FILESYSTEM HAS UNKNOWN ERROR FLAGS %08X", (unsigned)SPAD2CPU32_LV(&tx.txflags));
	if (SPAD2CPU16_LV(&tx.cc) >= MAX_CC) {
		log_printf(0, "HIGH CRASH COUNT REACHED (%04X)", SPAD2CPU16_LV(&tx.cc));
		if (!reset_cc && query(qprefix Q_NOFIX, "RESET?")) {
			reset_cc = 1;
		}
	}
	tx_valid = 1;
	if (mark_dirty | extend | rebuild_apages | reset_cc | undelete) {
		CPU2SPAD32_LV(&tx.txflags, SPAD2CPU32_LV(&tx.txflags) | TXFLAGS_DIRTY);
		write_txblock();
		r = flush_cache();
		if (__unlikely(r)) return r;
	}

	return_ok:
	if (extend) write_super();
	return 0;
}

static int check_ub(void)
{
	int ask_rewrite = 0;
	struct userblock *u;
	fill_empty_userblock();
	if (__unlikely(!(u = read_buffer(USERBLOCK_SECTOR, 1, 0)))) {
		log_printf(0, "UNABLE TO READ USER BLOCK: %s", buffer_error);
		ask_rewrite = 1;
		goto skip_ub;
	}
	if (__unlikely(u->magic != CPU2SPAD32(USERBLOCK_MAGIC)))
		goto skip_ub;

	if (__unlikely(__byte_sum(u, 512) != CHECKSUM_BASE)) {
		log_printf(0, "BAD CHECKSUM ON USERBLOCK: %02X", __byte_sum(u, 512) ^ CHECKSUM_BASE);
		ask_rewrite = 1;
		goto skip_ub;
	}
	memcpy(&user, u, 512);

	if (!ro && !mark_fsck && __unlikely(user.userflags & USERFLAGS_NEED_FSCK)) {
		log_printf(0, "FSCK REQUESTED BY THE USER");
		if (user.userflags & USERFLAGS_REBUILD_APAGES) {
			log_printf(0, "REBUILD APAGES REQUESTED BY THE USER");
			rebuild_apages = 1;
		}
		if (user.userflags & USERFLAGS_RESET_CC) {
			log_printf(0, "RESET CRASH COUNTS REQUESTED BY THE USER");
			reset_cc = 1;
		}
		if (user.userflags & USERFLAGS_EXTEND) {
			log_printf(0, "FILESYSTEM EXTENSION REQUESTED BY THE USER");
			extend = 1;
		}
		if (user.userflags & USERFLAGS_SET_RESERVE) {
			log_printf(0, "CHAGING THE NUMBER OF RESERVED SECTORS REQUESTED BY THE USER");
			reserve = SPAD2CPU64_LV(&user.reserve);
		}
		if (user.userflags & USERFLAGS_ZERO_FILE_TAILS) {
			log_printf(0, "ZEROING FILE TAILS REQUESTED BY THE USER");
			zero_file_tails = 1;
		}
		if (!(user.userflags & (USERFLAGS_REBUILD_APAGES | USERFLAGS_RESET_CC | USERFLAGS_EXTEND | USERFLAGS_SET_RESERVE)))
			force = 1;
		user.userflags &= ~(USERFLAGS_NEED_FSCK | USERFLAGS_REBUILD_APAGES | USERFLAGS_RESET_CC | USERFLAGS_EXTEND | USERFLAGS_SET_RESERVE);
		write_user();
	}

	skip_ub:

	if (mark_fsck) {
		if (mark_fsck == 2) {
			status_printf(0, "filesystem is mounted.");
			if (!query(qprefix Q_NOFIX, "MARK FOR FSCK?"))
				return 0;
		}
		user.userflags |= USERFLAGS_NEED_FSCK;
		if (rebuild_apages) user.userflags |= USERFLAGS_REBUILD_APAGES;
		if (reset_cc) user.userflags |= USERFLAGS_RESET_CC;
		if (extend) user.userflags |= USERFLAGS_EXTEND;
		if (reserve >= 0) {
			user.userflags |= USERFLAGS_SET_RESERVE;
			CPU2SPAD64_LV(&user.reserve, reserve);
		}
		if (zero_file_tails) user.userflags |= USERFLAGS_ZERO_FILE_TAILS;
		write_user();
		if (mark_fsck == 2)
			status_printf(0, "mounted filesystem was marked for fsck.");
		else
			status_printf(0, "filesystem was marked for fsck.");
		return 0;
	}

	if (ask_rewrite) {
		if (query(qprefix 0, "REWRITE?")) {
			write_user();
		}
	}

	return 0;
}

int check_super(void)
{
	int r;
	status_printf(0, "checking super block...");
	if (__unlikely(r = check_sb1())) return r;
	if (__unlikely(r = check_ub())) return r;
	if (mark_fsck) return 0;
	if (__unlikely(r = check_sb2())) return r;
	if (__unlikely(r = check_txb())) return r;
	return 0;
}

int recover_super_allocs(void)
{
	int r;
	if (__unlikely(r = alloc_super())) {
		if (r == 1) log_printf(1, "FAILED TO RECOVER SUPERBLOCK ALLOCATION: %s", alloc_error);
		return 2;
	}
	if (tx_valid) {
		if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.txblock), (1 << super.sectors_per_block_bits)))) {
			if (r == 1) log_printf(1, "FAILED TO RECOVER TX BLOCK ALLOCATION: %s", alloc_error);
			return 2;
		}
	}
	return 0;
}

int fix_txblock(void)
{
	int r;
	int msg = 0;
	__u64 max_ino = ino_get_max();
	if (__likely(tx_valid)) {
		if (SPAD2CPU64_LV(&tx.ino) < max_ino) {
			if (!ro && !msg) status_printf(0, "fixing tx block..."), msg = 1;
			log_printf(0, "TOO LOW MAXIMUM INODE NUMBER IN TXBLOCK %"__64_format"X, SHOULD BE %"__64_format"X", SPAD2CPU64_LV(&tx.ino), max_ino);
			if (query(qprefix 0, "FIX?")) {
				CPU2SPAD64_LV(&tx.ino, max_ino);
				write_txblock();
			}
		}
		return 0;
	}
	CPU2SPAD64_LV(&tx.ino, max_ino);
	if (!ro && !msg) status_printf(0, "fixing tx block..."), msg = 1;
	r = alloc_try(SPAD2CPU64_LV(&super.txblock), 1);
	if (!r) {
		if (!query(qprefix 0, "WRITE TX BLOCK?")) return 0;
	} else {
		blk_t new_tx;
		if (__unlikely(r > 0)) return r;
		if (!query(qprefix 0, "ALLOC NEW TX BLOCK?")) return 0;
		if (__unlikely(r = alloc_space(1, "TX BLOCK", 1, 0, 1, &new_tx))) return r;
		CPU2SPAD64_LV(&super.txblock, new_tx);
		write_super();
	}
	tx_valid = 1;
	write_txblock();
	return 0;
}

int fix_errorflag(void)
{
	if (__likely(tx_valid) && SPAD2CPU32_LV(&tx.txflags) && !answered_no) {
		if (!ro) {
			int r;
			status_printf(0, "synchronizing cache...");
			r = flush_cache();
			if (__unlikely(r)) return r;
			status_printf(0, "clearing error flags...");
			CPU2SPAD32_LV(&tx.txflags, 0);
			write_txblock();
		} else {
			answered_no = 1;
		}
	}
	return 0;
}

void done_super(void)
{
}

