#ifndef __SPAD__
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS	64
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#if defined(__GLIBC__) && __GLIBC__ >= 2 && !defined(__INTEL_COMPILER)
#else
#define loff_t __loff_t
#endif
#ifdef HAVE_ERROR_H
#include <error.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/sysmacros.h>
#endif
#include "../common.h"
#include "../mounted.h"
#else
#include <STDIO.H>
#include <STDLIB.H>
#include <ERRNO.H>
#include <TIME.H>
#include <SYS/STAT.H>
#include <SPAD/LIBC.H>
#define HAVE_ERROR_H
#include <ERROR.H>
#include <UNISTD.H>
#include <FCNTL.H>
#include <STRING.H>
#include <ARCH/MOV.H>
#include <SPAD/DEV.H>
#include <SPAD/IOCTL.H>
#include <SPAD/SYNC.H>
#include "../COMMON.H"
#include "../MOUNTED.H"
#endif

typedef __u64 __d_off;

#include "sfsapage.inc"

#ifndef HAVE_ERROR_H
static void
__PRINTF_ATTR__(3,4) __NORET_ATTR__
error(int x, int y, char *m, ...)
{
	va_list va;
	va_start(va, m);
	vfprintf(stderr, m, va);
	va_end(va);
	if (y) fprintf(stderr, ": %s", strerror(y));
	fprintf(stderr, "\n");
	exit(x);
}
#endif

/* FILESYSTEM LAYOUT:
	sector, no of sectors
	4000,1 (SUPERBLOCK_SECTOR) -- superblock
	4001,1 (TXBLOCK_SECTOR) -- tx block
	4002,apage_index_sectors (apage_index_0_sector) -- apage index 0
	4002+apage_index_sectors,apage_index_sectors (apage_index_1_sector) -- apage index 1
	4002+2*apage_index_sectors,n_apages*page_size (apages_sector) -- apages
	apages_sector+n_apages*page_size,512 (cct_sector) -- txc table
	cct_sector+CCT_SECTORS -- cct_crc_sector, CCT_CRC_UNITS of side CRCs
	cct_crc_sector+cct_crc_sectors -- cct_backup_sector, CCT_SECTORS (Phase 3)
	cct_backup_sector+CCT_SECTORS -- cct_backup_crc_sector, same CRC geometry
	cct_backup_crc_sector+cct_crc_sectors -- root fnode
	64 (ROOT_DIR) -- root directory
	-- free space
	superblock backup
*/

#define BUFFER_SIZE		CCT_SIZE

#define CLEAR_BUFFERS		(1 << MAX_SECTORS_PER_BLOCK_BITS)

/* CCT_SECTORS now comes from struct.h (CCT_SIZE >> SSIZE_BITS, same 512);
 * the local duplicate was removed so there is one definition of the cct's
 * sector count in the tree. */
/*#define ROOT_DIR_SECTORS	fnode_sectors*/
#define ROOT_DIR_SECTORS	block_sectors

int endian_big = 0;
static int dynamic_magic = 1;

static char truncated = 0;
static __s64 real_size = -1;
static __s64 size = -1;
static __s64 reserve = -1;
static int block_size = -1;
static int block_bits;
static int fnode_size = -1;
static int fnode_bits;
static int page_size = -1;
static int page_bits;
static int cluster_size = -1;
static int cluster_bits;
static int cluster_threshold = -1;
static __s64 group_size = -1;
static int group_bits;
static const char *metadata_groupsize_str = NULL;
static const char *smallfile_groupsize_str = NULL;

static __u64 groups;
static int metadata_groups;
static int smallfile_groups;

static int trim =
#ifdef __SPAD__
	0
#else
	1
#endif
;
static int only_trim = 0;
static int unix_names =
#ifdef __SPAD__
	0
#else
	1
#endif
;
static int dir_sizes =
#ifdef __SPAD__
	0
#else
	1
#endif
;
static int extra_space = 1;
static int no_checksums = 0;

#define MAX_PAGE_SIZE	(512 << MAX_SECTORS_PER_PAGE_BITS)
#define MAX_BLOCK_SIZE	(512 << MAX_SECTORS_PER_BLOCK_BITS)

#define block_sectors	(1 << (block_bits - 9))
#define page_sectors	(1 << (page_bits - 9))
#define fnode_sectors	(1 << (fnode_bits - 9))
#define block_align(x)	(((x) + block_sectors - 1) & ~(__u64)(block_sectors - 1))
#define page_align(x)	(((x) + page_sectors - 1) & ~(__u64)(page_sectors - 1))
#define fnode_align(x, o) ((block_align(x) & (fnode_sectors - 1)) + (o) <= fnode_sectors ? block_align(x) : ((x) + fnode_sectors - 1) & ~(__u64)(fnode_sectors - 1))

#define magic(block, magic)	((magic) ^ ((likely(dynamic_magic)) ? (__u32)(block) : 0))

static __u64 n_apages;
static int apage_index_sectors;

static __u64 superblock_sector = SUPERBLOCK_SECTOR;
static __u64 txblock_sector;
static __u64 apage_index_0_sector;
static __u64 apage_index_1_sector;
static __u64 apages_sector;
static __u64 cct_sector;
static __u64 cct_crc_sector;
static __u64 cct_crc_sectors;	/* CCT_CRC_UNITS rounded up to whole blocks */
static __u64 cct_backup_sector;	/* Phase 3: mirror of the crash-count table */
static __u64 cct_backup_crc_sector;
static __u64 root_fnode_sector;
static __u64 root_dir_sector;

#define SIZE_ARRAY	4096
static __u64 size_array[SIZE_ARRAY];	/* used_apages entries are valid */
static int used_apages;

static const char *file = NULL;
static int h;

static char buffer_[BUFFER_SIZE + MAX_PAGE_SIZE];
static char *buffer;

#define ub ((struct userblock *)(buffer + (1 << block_bits) - 512))
#define sb ((struct superblock *)buffer)
#define txb ((struct txblock *)buffer)
#define apg ((struct apage_head *)buffer)
#define fx ((struct fixed_fnode_block *)buffer)

#ifdef __linux__
#if !defined(__GLIBC__) || __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 1)
#include <asm/unistd.h>
#ifdef __NR__llseek
#define use_llseek
static _syscall5(int, _llseek, uint, fd, ulong, hi, ulong, lo, loff_t *, res, uint, wh);
#endif
#endif
#endif

#ifdef __SPAD__
#include <KERNEL/GETHSIZE.I>
#else
#include "../gethsize.inc"
#endif

static __s64 get_number_percent(const char *a, char *num_error, __s64 total);

static void check_valid(void)
{
	char num_error;
	if (block_size == -1) {
#if defined(__SPAD__)
		IOCTLRQ io;
		io.h = h;
		io.ioctl = IOCTL_BIO_PHYSICAL_BLOCKSIZE;
		io.param = 0;
		io.v.ptr = 0;
		io.v.len = 0;
		io.v.vspace = &KERNEL$VIRTUAL;
		SYNC_IO(&io, KERNEL$IOCTL);
		if (io.status >= 0) {
			block_size = io.status;
		} else {
			block_size = 512;
		}
#else
		if (unix_names) {
			block_size = getpagesize() < 4096 ? getpagesize() : 4096;
		} else {
			block_size = 512;
		}
#ifdef BLKSSZGET
		{
			int lbs;
			if (!ioctl(h, BLKSSZGET, &lbs)) {
				if (block_size < lbs)
					block_size = lbs;
			}
		}
#endif
#endif
		if (block_size > MAX_BLOCK_SIZE) block_size = MAX_BLOCK_SIZE;
		if (block_size < 512 || block_size & (block_size - 1)) block_size = 512;
	}
	if (block_size < 512 || block_size > MAX_BLOCK_SIZE || block_size & (block_size - 1)) error(2, 0, "\rinvalid block size");
	block_bits = ffs(block_size) - 1;

	real_size = gethsize(h) >> 9;
	if (size == -1) {
		size = real_size;
		if (!size) error(1, errno, "\rcannot access %s", file);
	} else {
		size >>= 9;
		if (size > real_size) truncated = 1;
	}
	size &= ~(__u64)(block_sectors - 1);
	size -= block_sectors;
	if (size < MIN_SIZE || size > MAX_SIZE) error(2, 0, "\rbad size");

	if (reserve == -1) reserve = get_default_reserved(size);
	else reserve >>= 9;
	reserve &= ~(__u64)(block_sectors - 1);
	if (reserve > size) error(2, 0, "\rbad reserve");

	if (page_size == -1) {
#ifdef __SPAD__
		page_size = getpagesize() < 32768 ? getpagesize() : 32768;
#else
		page_size = 32768;
#endif
		if (page_size <= 0 || page_size & (page_size - 1)) page_size = 32768;
		while (page_size < block_size || page_size < 1024 || page_size < fnode_size) page_size *= 2;
	}
	if (page_size < 1024 || page_size > MAX_PAGE_SIZE || page_size & (page_size - 1)) error(2, 0, "\rinvalid page_size");
	page_bits = ffs(page_size) - 1;
	if (page_bits < block_bits) error(2, 0, "\rpage size is less than block size");

	if (fnode_size == -1) {
		fnode_size = 8192;
		if (fnode_size < block_size) fnode_size = block_size;
		if (fnode_size > page_size) fnode_size = page_size;
	}
	if (fnode_size < 512 || fnode_size > MAX_PAGE_SIZE || fnode_size & (fnode_size - 1)) error(2, 0, "\rinvalid fnode size");
	fnode_bits = ffs(fnode_size) - 1;
	if (fnode_bits < block_bits) error(2, 0, "\rfnode size is less than page size");
	if (fnode_bits > page_bits) error(2, 0, "\rfnode size is greater than page size");

	if (cluster_size == -1) {
		cluster_size = block_size;
	}
	if (cluster_size < 512 || (cluster_size > 512 << MAX_ALLOC_MASK_BITS) || cluster_size & (cluster_size - 1)) error(2, 0, "\rinvalid cluster size");
	cluster_bits = ffs(cluster_size) - 1;
	if (cluster_bits < block_bits) error(2, 0, "\rcluster size is less than block size");

	if (cluster_threshold == -1) {
		cluster_threshold = 8 * cluster_size;
		if (cluster_threshold < 262144)
			cluster_threshold = 262144;
	}
	cluster_threshold = ((unsigned)cluster_threshold + cluster_size - 1) & -cluster_size;
	if ((unsigned)cluster_threshold >= 1 << 31) error(2, 0, "\rcluster threshold is greater than 1 << 31");
	if ((unsigned)cluster_threshold >= (__u64)cluster_size << 16) error(2, 0, "\rcluster threshold is greater than cluster size * 65536");

	if (group_size == -1) {
		group_bits = get_default_group_bits(size, cluster_bits - 9) + 9;
	} else {
		group_bits = 9;
		while (((__u64)3 << group_bits) >> 1 < group_size && group_bits < 48) {
			group_bits++;
		}
	}
	group_size = (__u64)1 << group_bits;
	if (group_bits < cluster_bits) error(2, 0, "\rgroup size is less than cluster size");
	groups = (size + ((__u64)1 << (group_bits - 9)) - 1) >> (group_bits - 9);
	if (groups > 0xffff) error(2, 0, "\rmore than 65535 groups (too small group size)");

	if (!metadata_groupsize_str) {
		metadata_groups = get_default_metadata_groups(group_bits - 9, groups);
	} else {
		__s64 metadata_groupsize = get_number_percent(metadata_groupsize_str, &num_error, size << 9);
		if (num_error || metadata_groupsize < 0 || metadata_groupsize > size << 9) error(2, 0, "\rinvalid metadata group size");
		metadata_groups = (metadata_groupsize + ((__u64)1 << (group_bits - 1))) >> group_bits;
	}
	if (metadata_groups > groups) error(2, 0, "\rmetadata group size too high");
	if (!smallfile_groupsize_str) {
		smallfile_groups = get_default_smallfile_groups(group_bits - 9, groups, metadata_groups);
	} else {
		__s64 smallfile_groupsize = get_number_percent(smallfile_groupsize_str, &num_error, size << 9);
		if (num_error || smallfile_groupsize < 0 || smallfile_groupsize > size << 9) error(2, 0, "\rinvalid smallfile group size");
		smallfile_groups = (smallfile_groupsize + ((__u64)1 << (group_bits - 1))) >> group_bits;
	}
	if (smallfile_groups > groups) error(2, 0, "\rsmallfile group size too high");
	if (smallfile_groups + metadata_groups > groups) {
		if (!metadata_groupsize_str) metadata_groups = groups - smallfile_groups;
		else if (!smallfile_groupsize_str) smallfile_groups = groups - metadata_groups;
		else error(2, 0, "\rmetadata group size + smallfile group size too high");
	}

	n_apages = N_APAGES(size, page_bits, block_bits);
	if (n_apages > MAX_APAGES) {
		error(2, 0, "\rtoo many apages");
	}
	apage_index_sectors = APAGE_INDEX_SECTORS(n_apages, block_size);

	txblock_sector = block_align(superblock_sector + block_sectors);
	apage_index_0_sector = block_align(txblock_sector + block_sectors);
	apage_index_1_sector = block_align(apage_index_0_sector + apage_index_sectors);
	apages_sector = page_align(apage_index_1_sector + apage_index_sectors);
	cct_sector = block_align(apages_sector + (n_apages << (page_bits - 9)));
	cct_crc_sector = block_align(cct_sector + CCT_SECTORS);
	/*
	 * Round the CRC region up to whole blocks and start root_fnode directly
	 * after it. Leaving block_align() to round the END instead produced a
	 * short, non-block-aligned gap (3 sectors at 17093 on a 4K-block volume)
	 * which had to be freed, and fsck rejected it:
	 *   APAGE 0 (4080) HAS UNALIGNED ENTRY 0030 (42C5 - 42C8)
	 * Whole-block rounding means there is no gap to free and nothing ragged
	 * for the allocator to trip over.
	 */
	cct_crc_sectors = ((CCT_CRC_UNITS + block_sectors - 1) / block_sectors) * block_sectors;

	/*
	 * Phase 3: the cct mirror. Placed after the primary's CRC region and
	 * before the root fnode, each boundary block-aligned for the same reason
	 * the CRC region is: a short unaligned gap here becomes an unaligned
	 * apage entry that fsck rejects. root_fnode then follows the mirror's
	 * own CRC region, so the whole cct complex stays contiguous.
	 */
	cct_backup_sector = block_align(cct_crc_sector + cct_crc_sectors);
	cct_backup_crc_sector = block_align(cct_backup_sector + CCT_BACKUP_SECTORS);
	root_fnode_sector = cct_backup_crc_sector + cct_crc_sectors;
	root_dir_sector = fnode_align(root_fnode_sector + block_sectors, ROOT_DIR_SECTORS);
}

static void print_progress(__u64 total, __u64 partial)
{
	static time_t lt = 0;
	time_t t = time(NULL);
	if (t == lt) return;
	lt = t;
	fprintf(stderr, "\r%d%%", (int)((__u64)partial * 100 / total));
}

static void zerobuf(void)
{
	memset(buffer, 0, BUFFER_SIZE);
}

static void write_blocks(void *ptr, __u64 sec, unsigned num)
{
	__s64 rs;
	int rw;
#ifdef use_llseek
	loff_t l;
#endif
	sec *= 512;
	num *= 512;
#ifdef use_llseek
	rs = _llseek(h, sec >> 32, sec, &l, SEEK_SET);
#else
	rs = lseek(h, sec, SEEK_SET);
#endif
	if (rs == -1) error(1, errno, "\rllseek %08x%08x", (unsigned)(sec >> 32), (unsigned)(sec & 0xffffffffu));
again:
	rw = write(h, ptr, num);
	if (rw == -1) error(1, errno, "\rwrite %08x%08x", (unsigned)(sec >> 32), (unsigned)(sec & 0xffffffffu));
	if (rw != num) {
		if (!rw) error(1, 0, "\rshort write %08x%08x", (unsigned)(sec >> 32), (unsigned)(sec & 0xffffffffu));
		ptr = (unsigned char *)ptr + rw;
		num -= rw;
		goto again;

	}
}

static int do_discard(__d_off off, __d_off len)
{
#ifdef BLKDISCARD
	int er = 0;
	__d_off total = len;
	while (len > 0) {
		__u64 range[2];
		__d_off p_len = 1 << 21;
		if (p_len > len)
			p_len = len;
		print_progress(total, total - len);
		range[0] = (__u64)off << 9;
		range[1] = (__u64)p_len << 9;
		if (ioctl(h, BLKDISCARD, &range)) {
			er = -1;
			if (len == total)
				break;
		}
		len -= p_len;
		off += p_len;
	}
	return er;
#else
	errno = ENOSYS;
	return -1;
#endif
}

static void sync_cache(void)
{
	if (fsync(h)) error(1, errno, "\rcan't fsync");
}

static void write_user(void)
{
	zerobuf();
	CPU2SPAD32_LV(&ub->magic, USERBLOCK_MAGIC);
	ub->userflags = 0;
	ub->checksum = 0;	/* legacy 8-bit XOR field, now vestigial:
				 * zeroed to a deterministic value so the trailer
				 * covers it, and the userblock trailer is authoritative */
	spadfs_meta_seal(ub);
	write_blocks(buffer, USERBLOCK_SECTOR - (block_sectors - 1), block_sectors);
}

static void clear_device(void)
{
	__s64 s;
	unsigned nsec;
	zerobuf();
	write_blocks(buffer, SUPERBLOCK_SECTOR, 1);
	s = real_size - CLEAR_BUFFERS;
	if (s < 0) s = 0;
	for (; s < real_size; s += nsec) {
		nsec = BUFFER_SIZE / 512;
		if (nsec > real_size - s) nsec = real_size - s;
		write_blocks(buffer, s, nsec);
	}
}

static void trim_device(void)
{
#if defined(BLKFLSBUF) && defined(major)
	struct stat st;
	if (!fstat(h, &st)) {
		if (S_ISBLK(st.st_mode) && major(st.st_rdev) == 1) {
			ioctl(h, BLKFLSBUF, 0);	/* this discards content of linux ramdisk */
		}
	}
#endif
	if (do_discard(0, real_size) && only_trim) {
		error(1, errno, "\rthe device was not trimmed");
	}
}

static void write_super(void)
{
	const char *val;
	zerobuf();
	/*
	 * Was strcpy(), which only worked because the old signature "FS SPAD\000"
	 * was exactly 8 bytes including its terminator. spadfs2's signature is 8
	 * characters, so strcpy would write 9 bytes into __u8 signature[8].
	 * Fixed-size memcpy instead, with a compile-time guard so a future
	 * shortening of the literal fails the build rather than the volume.
	 */
	_Static_assert(sizeof SUPERBLOCK_SIGNATURE >= sizeof sb->signature,
		       "SUPERBLOCK_SIGNATURE shorter than superblock.signature field");
	memcpy(sb->signature, SUPERBLOCK_SIGNATURE, sizeof sb->signature);
	CPU2SPAD64_LV(&sb->byte_sex, __make64(0x89ABCDEF, 0x01234567));
	sb->version_major = SPADFS_VERSION_MAJOR;
	sb->version_middle = SPADFS_VERSION_MIDDLE;
	sb->version_minor = SPADFS_VERSION_MINOR;
	sb->sectors_per_block_bits = block_bits - 9;
	sb->sectors_per_fnodepage_bits = fnode_bits - 9;
	sb->sectors_per_page_bits = page_bits - 9;
	sb->sectors_per_cluster_bits = cluster_bits - 9;
	sb->sectors_per_group_bits = group_bits - 9;
	CPU2SPAD16_LV(&sb->cluster_threshold, cluster_threshold / (1 << cluster_bits));
	CPU2SPAD16_LV(&sb->small_file_group, metadata_groups);
	CPU2SPAD16_LV(&sb->large_file_group, metadata_groups + smallfile_groups);
	CPU2SPAD64_LV(&sb->size, size);
	CPU2SPAD64_LV(&sb->reserve_sectors, reserve);
	CPU2SPAD64_LV(&sb->txblock, txblock_sector);
	CPU2SPAD64_LV(&sb->apage_index[0], apage_index_0_sector);
	CPU2SPAD64_LV(&sb->apage_index[1], apage_index_1_sector);
	CPU2SPAD64_LV(&sb->cct, cct_sector);
	CPU2SPAD64_LV(&sb->cct_crc, cct_crc_sector);
	CPU2SPAD64_LV(&sb->cct_backup, cct_backup_sector);
	CPU2SPAD64_LV(&sb->cct_backup_crc, cct_backup_crc_sector);
	CPU2SPAD64_LV(&sb->root, root_fnode_sector);
	if (unix_names)
		CPU2SPAD32_LV(&sb->flags_compat_none, SPAD2CPU32_LV(&sb->flags_compat_none) | FLAG_COMPAT_NONE_UNIX_NAMES);
	if (dir_sizes)
		CPU2SPAD32_LV(&sb->flags_compat_ro, SPAD2CPU32_LV(&sb->flags_compat_ro) | FLAG_COMPAT_RO_DIRECTORY_SIZES);
	if (dynamic_magic)
		CPU2SPAD32_LV(&sb->flags_compat_none, SPAD2CPU32_LV(&sb->flags_compat_none) | FLAG_COMPAT_NONE_DYNAMIC_MAGIC);
	if (extra_space)
		CPU2SPAD32_LV(&sb->flags_compat_none, SPAD2CPU32_LV(&sb->flags_compat_none) | FLAG_COMPAT_NONE_EXTRA_SPACE);
	if (no_checksums)
		CPU2SPAD32_LV(&sb->flags_compat_fsck, SPAD2CPU32_LV(&sb->flags_compat_fsck) | FLAG_COMPAT_FSCK_NO_CHECKSUMS);
	sb->checksum = 0;	/* legacy 8-bit XOR field, now vestigial:
				 * zeroed to a deterministic value so the trailer
				 * covers it, and the superblock trailer is authoritative */
	spadfs_meta_seal(sb);
	if ((val = validate_super(sb))) {
		fprintf(stderr, "\rerror: %s\n", val);
		error(4, 0, "\rinternal error, validate_super failed. Report all arguments that you passed to " TOOL_BANNER "");
	}
	write_blocks(sb, SUPERBLOCK_SECTOR, block_sectors);
	if (!truncated) write_blocks(sb, size, block_sectors);
}

static void write_txbl(void)
{
	zerobuf();
	CPU2SPAD32_LV(&txb->magic, magic(txblock_sector, TXBLOCK_MAGIC));
	CPU2SPAD16_LV(&txb->cc, 0);
	CPU2SPAD16_LV(&txb->a_cc, 0);
	CPU2SPAD32_LV(&txb->a_txc, 0);
	CPU2SPAD32_LV(&txb->txflags, 0);
	txb->checksum = 0;	/* legacy 8-bit XOR field, now vestigial:
				 * zeroed to a deterministic value so the trailer
				 * covers it, and the txblock trailer is authoritative */
	spadfs_meta_seal(txb);
	write_blocks(sb, txblock_sector, block_sectors);
}

static void write_txct(__u64 base)
{
	int i;
	zerobuf();
	for (i = 0; i < CCT_SECTORS; i += block_sectors) {
		print_progress(CCT_SECTORS, i);
		write_blocks(buffer, base + i, block_sectors);
	}
}

/*
 * Side CRC table for the crash-count table (Phase 2.3, option A).
 *
 * The cct cannot carry the standard trailer itself. It is fully-packed live
 * __u32 crash counters with no header and no padding, and 128 slots * 512
 * sectors consumes the entire u16 range of cc. Taking the last 4 bytes of
 * each sector would make every cc == 127 (mod 128) unusable, so the
 * checksums get their own region and the cct layout is left exactly as it is.
 *
 * One CRC per cct sector, CCT_CRC_PER_UNIT of them packed into each 512-byte
 * unit, and every unit sealed with the standard trailer -- otherwise the
 * checksum of checksums would just become the new unprotected link.
 *
 * At mkfs time the cct is all zeros, so every entry is the CRC of a zeroed
 * sector. That is still recorded explicitly: what matters is that the stored
 * value matches what is on disk, not that it is interesting.
 */
static void write_cct_crc(__u64 base)
{
	static __u8 zero_sector[512];
	__u8 unit[512];
	unsigned u, i;

	for (u = 0; u < CCT_CRC_UNITS; u++) {
		memset(unit, 0, sizeof unit);
		for (i = 0; i < CCT_CRC_PER_UNIT; i++) {
			unsigned sector = u * CCT_CRC_PER_UNIT + i;
			if (sector >= CCT_SECTORS)
				break;
			spadfs_meta_put_crc(unit + i * 4,
					    spadfs_crc32c_std(zero_sector, 512));
		}
		spadfs_meta_seal(unit);
		write_blocks(unit, base + u, 1);
	}
}

static void write_root_fixed_fnode(void)
{
#define fn	((struct fnode *)fx->fnode0)
	zerobuf();
	CPU2SPAD32_LV(&fx->magic, magic(root_fnode_sector, FIXED_FNODE_BLOCK_MAGIC));
	fx->flags = FIXED_FNODE_BLOCK_CHECKSUM_VALID;
	CPU2SPAD16_LV(&fx->cc, 0);
	CPU2SPAD32_LV(&fx->txc, 0);
	CPU2SPAD64_LV(&fx->nlink0, 1);
	CPU2SPAD16_LV(&fx->hint_small, metadata_groups);	/* these two are not really used or changed ... hints in the following root fnode are appropriate */
	CPU2SPAD16_LV(&fx->hint_large, metadata_groups + smallfile_groups);

	CPU2SPAD16_LV(&fn->next, FNODE_SIZE(0, 0) & FNODE_NEXT_SIZE);
	CPU2SPAD16_LV(&fn->cc, 0);
	CPU2SPAD32_LV(&fn->txc, 0);
	CPU2SPAD64_LV(&fn->size[0], dir_sizes ? ROOT_DIR_SECTORS * 512 : 0);
	fn->ctime = fn->mtime = CPU2SPAD32(time(NULL));
	fn->run10 = MAKE_PART_0(root_dir_sector);
	fn->run11 = MAKE_PART_1(root_dir_sector);
	CPU2SPAD16_LV(&fn->run1n, metadata_groups);
	CPU2SPAD16_LV(&fn->run2n, metadata_groups + smallfile_groups);
	fn->flags = FNODE_FLAGS_DIR;
	fn->namelen = 0;
	fx->checksum = 0;	/* legacy XOR field, vestigial; trailer is authoritative */
	spadfs_meta_seal(fx);	/* fixed_fnode_block.trailer_crc sits at 508 */
	write_blocks(buffer, root_fnode_sector, block_sectors);
#undef fn
}

static void write_root_dir(void)
{
	int i;
	zerobuf();
	for (i = 0; i < ROOT_DIR_SECTORS * 512; i += FNODE_BLOCK_SIZE) {
		struct fnode_block *b = (void *)&buffer[i];
		struct fnode *f;
		CPU2SPAD32_LV(&b->magic, magic(root_dir_sector + i / FNODE_BLOCK_SIZE, FNODE_BLOCK_MAGIC));
		b->flags = FNODE_BLOCK_CHECKSUM_VALID | FNODE_BLOCK_FIRST * !i | FNODE_BLOCK_LAST * (i + FNODE_BLOCK_SIZE >= ROOT_DIR_SECTORS * 512);
		CPU2SPAD32_LV(&b->prev0, 0);
		CPU2SPAD16_LV(&b->prev1, 0);
		CPU2SPAD32_LV(&b->txc, TXC_INVALID(0));
		CPU2SPAD16_LV(&b->cc, 0);
		f = b->fnodes;
		CPU2SPAD16_LV(&f->next, (FNODE_MAX_SIZE & FNODE_NEXT_SIZE) | FNODE_NEXT_FREE);
		CPU2SPAD16_LV(&f->cc, 0);
		CPU2SPAD32_LV(&f->txc, 0);
		b->checksum = CHECKSUM_BASE ^ __byte_sum(b, FNODE_BLOCK_SIZE);
		/* fnode blocks stay on the XOR fold -- see struct.h FNODE_MAX_SIZE */
	}
	write_blocks(buffer, root_dir_sector, ROOT_DIR_SECTORS);
}

static void write_apage_index(__u64 ai)
{
	struct apage_index_entry *aind = NULL;	/* warning go away */
	int i;
	__u64 ap = apages_sector + (n_apages - 1) * page_sectors;
	for (i = 0; i < n_apages; i++) {
		print_progress(n_apages, i);
		if (!(i * sizeof(struct apage_index_entry) & ((1 << block_bits) - 1))) {
			if (i) write_blocks(buffer, ai, block_sectors), ai += block_sectors;
			zerobuf();
			aind = (void *)buffer;
		}
		CPU2SPAD64_LV(&aind->apage, ap);
		ap -= page_sectors;
		CPU2SPAD64_LV(&aind->end_sector, i < used_apages ? size_array[i] : 0);
		aind++;
	}
	write_blocks(buffer, ai, block_sectors);
}

static int free_range(struct apage_head *a, __d_off off, __u32 len)
{
	/*do_discard(off, len);*/
	if (!len) return 0;
	return SPAD_APAGE_FREE(a, off, len);
}

static void write_apages(void)
{
	__u32 s;
	__u64 ptr;
	int r;
	int a_sector, n_e;
	int wrote_apages;
	a_sector = apages_sector;
	wrote_apages = 0;
	while (wrote_apages < n_apages) {
		print_progress(n_apages, wrote_apages++);
		zerobuf();
		CPU2SPAD16_LV(&apg->magic, APAGE_MAGIC);
		CPU2SPAD16_LV(&apg->cc, 0);
		CPU2SPAD32_LV(&apg->txc, 0);
		write_blocks(buffer, a_sector, page_sectors);
		/*write_blocks(buffer, a_sector, block_sectors);*/
		a_sector += page_sectors;
	}
	a_sector = apages_sector + (n_apages - 1) * page_sectors;
	n_e = (1 << page_bits) / 2 / sizeof(struct aentry);
	used_apages = 0;
	zerobuf();
	*APAGE_ERROR = 0;
	if (SPAD_APAGE_MAKE(apg, n_e, block_bits - 9)) goto err;
	if (extra_space) {
		unsigned extra_free_from = (BOOTLOADER_SECTORS + block_sectors - 1) & ~(block_sectors - 1);
		unsigned extra_free_to = USERBLOCK_SECTOR & ~(block_sectors - 1);
		if (free_range(apg, extra_free_from, extra_free_to - extra_free_from)) goto err;
	}
	if (free_range(apg, apage_index_1_sector + apage_index_sectors, apages_sector - (apage_index_1_sector + apage_index_sectors))) goto err;
	if (free_range(apg, root_fnode_sector + block_sectors, root_dir_sector - (root_fnode_sector + block_sectors))) goto err;
	ptr = root_dir_sector + ROOT_DIR_SECTORS;
	next_block:
	if (ptr > size) error(2, 0, "\rtoo small filesystem");
	s = ((__u64)1 << 32) - block_size / 512;
	if (ptr + s > size) s = size - ptr;
	r = free_range(apg, ptr, s);
	if (r < 0) goto err;
	if (!r) {
		ptr += s;
		if (ptr < size) goto next_block;
	}
	CPU2SPAD16_LV(&apg->magic, APAGE_MAGIC);
	apg->s.u.l.flags |= APAGE_CHECKSUM_VALID;
	apg->s.u.l.checksum = CHECKSUM_BASE ^ __byte_sum(&apg->s, sizeof(struct apage_subhead) + sizeof(struct aentry) * (n_e - 1));
	write_blocks(buffer, a_sector, page_sectors / 2 < block_sectors ? block_sectors : page_sectors / 2);
	size_array[used_apages++] = ptr;
	if (ptr < size) {
		if (used_apages >= SIZE_ARRAY) error(4, 0, "\rINTERNAL ERROR: SIZE_ARRAY OVERFLOW");
		a_sector -= page_sectors;
		if (a_sector < apages_sector)
			error(4, 0, "\rINTERNAL ERROR: APAGE OVERFLOW");
		zerobuf();
		if (SPAD_APAGE_MAKE(apg, n_e, block_bits - 9)) goto err;
		goto next_block;
	}
	return;
	err:
	error(4, 0, "\rINTERNAL ERROR IN APAGE OPERATION: %s", APAGE_ERROR);
}

#ifndef __SPAD__

/*
static __u64 xstrtoull(char *a, char **b)
{
	__u64 x = 0;
	while (*a >= '0' && *a <= '9') {
		__u64 y = x * 10 + *a - '0';
		if (x != (y - (*a - '0')) / 10) break;
		x = y;
		a++;
	}
	*b = a;
	return x;
}
*/

static __s64 get_number(const char *a, char *num_error)
{
	int m;
	__s64 n, mult;
	*num_error = 0;
	if (!a || !*a) goto err;
#if !defined(__linux__) || defined(__GLIBC__)
	n = strtoll(a, (char **)&a, 10);
#else
	n = strtod(a, (char **)&a);
#endif
	m = 0;
	if (*a == 'k' || *a == 'K') m = 1, a++;
	else if (*a == 'm' || *a == 'M') m = 2, a++;
	else if (*a == 'g' || *a == 'G') m = 3, a++;
	else if (*a == 't' || *a == 'T') m = 4, a++;
	if (*a == 'i') {
		mult = (__s64)1 << (m * 10);
		a++;
	} else {
		int i;
		mult = 1;
		for (i = 0; i < m; i++) mult *= 1000;
	}
	if (*a == 'b' || *a == 'B') a++;
	if (*a) goto err;
	if ((__s64)((__u64)n * mult) / mult != n) goto err;
	return (__u64)n * mult;
	err:
	*num_error = 1;
	return 0;
}

#endif

static __s64 get_number_percent(const char *a, char *num_error, __s64 total)
{
	if (a[0] && a[1] && strchr(a, 0)[-1] == '%') {
		unsigned long n = strtoul(a, (char **)&a, 10);
		*num_error = n > 100 || strcmp(a, "%");
		return total * n / 100;
	} else {
#ifndef __SPAD__
		return get_number(a, num_error);
#else
		__s64 s = 0;
		*num_error = __get_64_number(a, strchr(a, 0), 1, &s);
		return s;
#endif
	}
}

#ifdef __SPAD__
static const struct __param_table params[20] = {
	"", __PARAM_STRING, 1, __MAX_STR_LEN,
	"", __PARAM_INT64, MIN_SIZE << 9, 1,
	"UNIX_NAMES", __PARAM_BOOL, ~0, 1,
	"SPAD_NAMES", __PARAM_BOOL, ~0, 0,
	"BIG_ENDIAN", __PARAM_BOOL, ~0, 1,
	"LITTLE_ENDIAN", __PARAM_BOOL, ~0, 0,
	"DYNAMIC_MAGIC", __PARAM_BOOL, ~0, 1,
	"STATIC_MAGIC", __PARAM_BOOL, ~0, 0,
	"RESERVED_BEGINNING", __PARAM_BOOL, ~0, 0,
	"NO_RESERVED_BEGINNING", __PARAM_BOOL, ~0, 1,
	"DIRECTORY_SIZES", __PARAM_BOOL, ~0, 1,
	"NO_DIRECTORY_SIZES", __PARAM_BOOL, ~0, 0,
	"NO_CHECKSUMS", __PARAM_BOOL, ~0, 1,
	"CHECKSUMS", __PARAM_BOOL, ~0, 0,
	"RESERVE", __PARAM_INT64, 0, 1,
	"BLOCK_SIZE", __PARAM_INT, 512, MAX_BLOCK_SIZE + 1,
	"FNODE_SIZE", __PARAM_INT, 512, MAX_PAGE_SIZE + 1,
	"PAGE_SIZE", __PARAM_INT, 1024, MAX_PAGE_SIZE + 1,
	"CLUSTER_SIZE", __PARAM_INT, 512, 512 << MAX_ALLOC_MASK_BITS,
	"GROUP_SIZE", __PARAM_INT64, 512, 1,
	"METADATA_GROUP_SIZE", __PARAM_STRING, 1, __MAX_STR_LEN,
	"SMALLFILE_GROUP_SIZE", __PARAM_STRING, 1, __MAX_STR_LEN,
	"CLUSTER_THRESHOLD", __PARAM_INT, 0, (1U << 31) - 511,
	NULL, 0, 0, 0,
};
static void *const vars[20] = {
	&file,
	&size,
	&unix_names,
	&unix_names,
	&endian_big,
	&endian_big,
	&dynamic_magic,
	&dynamic_magic,
	&extra_space,
	&extra_space,
	&dir_sizes,
	&dir_sizes,
	&no_checksums,
	&no_checksums,
	&reserve,
	&block_size,
	&fnode_size,
	&page_size,
	&cluster_size,
	&group_size,
	&metadata_groupsize_str,
	&smallfile_groupsize_str,
	&cluster_threshold,
	NULL,
};
#endif

int main(int argc, const char * const argv[])
{
	const char * const *arg = argv;
#ifdef __SPAD__
	int state = 0;
	if (unlikely(__parse_params(&arg, &state, params, vars, NULL, NULL, NULL))) goto syn;
#else
	int skip = 0;
	while (*++arg) {
		__s64 num;
		char num_error;
		if (skip) goto is_file;
		if (0) {
		} else if (!strcasecmp(*arg, "--trim")) {
			trim = 1;
		} else if (!strcasecmp(*arg, "--no-trim")) {
			trim = 0;
			only_trim = 0;
		} else if (!strcasecmp(*arg, "--only-trim")) {
			trim = 1;
			only_trim = 1;
		} else if (!strcasecmp(*arg, "--unix-names")) {
			unix_names = 1;
		} else if (!strcasecmp(*arg, "--spad-names")) {
			unix_names = 0;
		} else if (!strcasecmp(*arg, "--big-endian")) {
			endian_big = 1;
		} else if (!strcasecmp(*arg, "--little-endian")) {
			endian_big = 0;
		} else if (!strcasecmp(*arg, "--dynamic-magic")) {
			dynamic_magic = 1;
		} else if (!strcasecmp(*arg, "--static-magic")) {
			dynamic_magic = 0;
		} else if (!strcasecmp(*arg, "--reserved-beginning")) {
			extra_space = 0;
		} else if (!strcasecmp(*arg, "--no-reserved-beginning")) {
			extra_space = 1;
		} else if (!strcasecmp(*arg, "--directory-sizes")) {
			dir_sizes = 1;
		} else if (!strcasecmp(*arg, "--no-directory-sizes")) {
			dir_sizes = 0;
		} else if (!strcasecmp(*arg, "--no-checksums")) {
			no_checksums = 1;
		} else if (!strcasecmp(*arg, "--checksums")) {
			no_checksums = 0;
		} else if (!strcasecmp(*arg, "--reserve")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 0) goto syn;
			reserve = num;
		} else if (!strcasecmp(*arg, "--block-size")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 512 || num > MAX_BLOCK_SIZE || num & (num - 1)) goto syn;
			block_size = num;
		} else if (!strcasecmp(*arg, "--fnode-size")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 512 || num > MAX_PAGE_SIZE || num & (num - 1)) goto syn;
			fnode_size = num;
		} else if (!strcasecmp(*arg, "--page-size")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 1024 || num > MAX_PAGE_SIZE || num & (num - 1)) goto syn;
			page_size = num;
		} else if (!strcasecmp(*arg, "--cluster-size")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 512 || num >= 512 << MAX_ALLOC_MASK_BITS || num & (num - 1)) goto syn;
			cluster_size = num;
		} else if (!strcasecmp(*arg, "--group-size")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 512) goto syn;
			group_size = num;
		} else if (!strcasecmp(*arg, "--metadata-group-size")) {
			if (!(metadata_groupsize_str = *++arg)) goto syn;
		} else if (!strcasecmp(*arg, "--smallfile-group-size")) {
			if (!(smallfile_groupsize_str = *++arg)) goto syn;
		} else if (!strcasecmp(*arg, "--cluster-threshold")) {
			num = get_number(*++arg, &num_error);
			if (num_error || num < 0 || num > (1U << 31) - 511) goto syn;
			cluster_threshold = num;
		} else if (!strcasecmp(*arg, "--")) {
			skip = 1;
		} else if (**arg == '-') {
			goto syn;
		} else {
			is_file:
			if (!file) {
				file = *arg;
			} else if (size == -1) {
				num = get_number(*arg, &num_error);
				if (num_error || num < MIN_SIZE << 9) goto syn;
				size = num;
			} else {
				goto syn;
			}
		}
	}
#endif
	if (!file) {
		syn:
		error(2, 0, "\rsyntax error");
	}

	if (!spadfs_struct_check_correctness()) {
		error(16, 0, "\r" TOOL_BANNER ": BAD STRUCTURE ALIGNMENT, THE PROGRAM WAS COMPILED WITH WRONG ABI");
	}

	buffer = (char *)((unsigned long)(buffer_ + MAX_PAGE_SIZE - 1) & ~(unsigned long)(MAX_PAGE_SIZE - 1));

	h = open(file, O_RDWR | O_EXCL);
	if (h < 0) error(1, errno, "\rerror opening %s", file);
	if (is_mounted(file, h)) error(1, errno, "\rdevice %s is mounted", file);
	check_valid();
	fprintf(stderr, "\rclearing previous state...\n");
	clear_device();
	fprintf(stderr, "\rsynchronizing cache...\n");
	sync();
	sync_cache();
	if (trim) {
		fprintf(stderr, "\rtrimming the device...\n");
		trim_device();
		if (only_trim) goto end;
	}
	fprintf(stderr, "\rwriting user block...\n");
	write_user();
	fprintf(stderr, "\rwriting tx block...\n");
	write_txbl();
	fprintf(stderr, "\rwriting apages...\n");
	write_apages();
	fprintf(stderr, "\rwriting apage index...\n");
	write_apage_index(apage_index_0_sector);
	/*no need to write index 1 */
	/*write_apage_index(apage_index_1_sector);*/
	fprintf(stderr, "\rwriting crash count table...\n");
	write_txct(cct_sector);
	write_cct_crc(cct_crc_sector);
	/*
	 * Phase 3: the mirror is written from the same zero-filled buffers as the
	 * primary, so at mkfs the two are byte-identical and both are sealed.
	 * A backup that is not CRC-valid from birth cannot be trusted later, and
	 * an unsealed backup region would just move the unprotected link.
	 */
	write_txct(cct_backup_sector);
	write_cct_crc(cct_backup_crc_sector);
	fprintf(stderr, "\rwriting root fixed fnode...\n");
	write_root_fixed_fnode();
	fprintf(stderr, "\rwriting root directory...\n");
	write_root_dir();
	fprintf(stderr, "\rsynchronizing cache...\n");
	sync_cache();
	fprintf(stderr, "\rwriting super block...\n");
	write_super();
	fprintf(stderr, "\rsynchronizing cache...\n");
	sync_cache();
end:
	close(h);
	fprintf(stderr, "\rdone.\n");
	return 0;
}
