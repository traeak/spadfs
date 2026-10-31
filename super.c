#include "spadfs.h"

__cold static void spadfs_free_super(struct super_block *s);

static
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct super_operations spadfs_sops;
static spadfs_cache_t *spadfs_inode_cachep = NULL;
static spadfs_cache_t *spadfs_ea_cachep = NULL;
spadfs_cache_t *spadfs_extent_cachep = NULL;

/* Slab contructor */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
static void fnode_ctor(void *fn_, spadfs_cache_t *cachep, unsigned long flags)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
static void fnode_ctor(spadfs_cache_t *cachep, void *fn_)
#else
static void fnode_ctor(void *fn_)
#endif
{
	SPADFNODE *fn = fn_;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
	if (likely((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR))
#endif
	{
		inode_init_once(inode(fn));
		fn->res.len = 0;
		INIT_LIST_HEAD(&fn->clear_entry);
		fn->ea = fn->ea_inline;
		if (spadfs_unlocked_extent_cache)
			fn->extent_cache_seq = 0;
	}
}

/*
 * Allocate/free inode. The list is walked and searched for
 * parent_fnode_block/parent_fnode_pos, so we'd better initialize them.
 */

static struct inode *spadfs_alloc_inode(struct super_block *s)
{
	SPADFS *fs = spadfs(s);
	SPADFNODE *fn;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
	fn = alloc_inode_sb(s, spadfs_inode_cachep, GFP_NOFS);
#else
	fn = kmem_cache_alloc(spadfs_inode_cachep, GFP_NOFS);
#endif
	if (unlikely(!fn))
		return NULL;

	fn->fs = fs;
	fn->parent_fnode_block = 0;
	fn->parent_fnode_pos = 0;
	fn->target_blocks = 0;
	fn->target_blocks_exact = 0;
	fn->dont_truncate_prealloc = 0;
	fn->ea_size = 0;

	mutex_init(&fn->file_lock);

#if defined(SPADFS_QUOTA) && SPADFS_QUOTA >= 2
	memset(&fn->i_dquot, 0, sizeof fn->i_dquot);
#endif

	return inode(fn);
}

static void spadfs_destroy_inode(struct inode *inode)
{
	SPADFNODE *fn = spadfnode(inode);

	if (fn->parent_fnode_block)
		spadfs_set_parent_fnode(fn, 0, 0);

	if (unlikely(fn->ea != fn->ea_inline)) {
		kmem_cache_free(spadfs_ea_cachep, fn->ea);
		fn->ea = fn->ea_inline;
	}

	mutex_destroy(&fn->file_lock);

	kmem_cache_free(spadfs_inode_cachep, fn);
}

int spadfs_ea_alloc(SPADFNODE *f, unsigned ea_size)
{
	u8 *ea;
	BUG_ON(ea_size > FNODE_MAX_EA_SIZE);
	if (likely(f->ea != f->ea_inline))
		return 0;
	ea = kmem_cache_alloc(spadfs_ea_cachep, GFP_NOFS);
	if (unlikely(!ea))
		return -ENOMEM;
	spadfs_memcpy(ea, f->ea_inline, f->ea_size);
	f->ea = ea;
	spadfs_find_ea_unx(f);
	return 0;
}

static void set_prealloc_part(SPADFS *fs, unsigned prealloc_part)
{
	fs->prealloc_part = prealloc_part;
	if (!(fs->prealloc_part & (fs->prealloc_part - 1)))
		fs->prealloc_part_bits = ffs(fs->prealloc_part) - 1;
	else
		fs->prealloc_part_bits = -1;
}

enum {
	Opt_help, Opt_uid, Opt_gid, Opt_umask,
	Opt_prealloc_part, Opt_prealloc_min, Opt_prealloc_max,
	Opt_xfer_size, Opt_buffer_size, Opt_prefetch, Opt_sync_time,
	Opt_no_checksums, Opt_checksums,
	Opt_ino64,
	Opt_usrquota, Opt_grpquota,
	Opt_err,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
static match_table_t tokens = {
	{Opt_help, "help"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_umask, "umask=%o"},
	{Opt_prealloc_part, "prealloc_part=%u"},
	{Opt_prealloc_min, "prealloc_min=%u"},
	{Opt_prealloc_max, "prealloc_max=%u"},
	{Opt_xfer_size, "xfer_size=%u"},
	{Opt_buffer_size, "buffer_size=%u"},
	{Opt_prefetch, "prefetch=%u"},
	{Opt_sync_time, "sync_time=%u"},
	{Opt_no_checksums, "no_checksums"},
	{Opt_checksums, "checksums"},
	{Opt_ino64, "ino64=%s"},
	{Opt_usrquota, "usrquota"},
	{Opt_grpquota, "grpquota"},
	{Opt_err, NULL},
};

__cold static int parse_opts(SPADFS *fs, char *opts, int remount)
{
	char *p;
	while ((p = strsep(&opts, ","))) {
		substring_t args[MAX_OPT_ARGS];
		int token, option;
		char str[7];

		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
			case Opt_help:
				return 2;
			case Opt_uid:
				if (match_int(args, &option))
					return 0;
				if (remount) {
					if (fs->uid != option)
						return 0;
					break;
				}
				fs->uid = option;
				break;
			case Opt_gid:
				if (match_int(args, &option))
					return 0;
				if (remount) {
					if (fs->gid != option)
						return 0;
					break;
				}
				fs->gid = option;
				break;
			case Opt_umask:
				if (match_octal(args, &option))
					return 0;
				if (option < 0 || option > 0777)
					return 0;
				if (remount) {
					if (fs->mode != (0777 & ~option))
						return 0;
					break;
				}
				fs->mode = 0777 & ~option;
				break;
			case Opt_prealloc_part:
				if (match_int(args, &option))
					return 0;
				if (option <= 0) return 0;
				set_prealloc_part(fs, option);
				break;
			case Opt_prealloc_min:
				if (match_int(args, &option))
					return 0;
				if (option < 0) return 0;
				fs->min_prealloc = option;
				break;
			case Opt_prealloc_max:
				if (match_int(args, &option))
					return 0;
				if (option < 0) return 0;
				fs->max_prealloc = option;
				break;
			case Opt_xfer_size:
				if (match_int(args, &option))
					return 0;
				if (option <= 0) return 0;
				fs->xfer_size = option;
				break;
			case Opt_buffer_size:
				if (match_int(args, &option))
					return 0;
				if (option < 512 || option > PAGE_SIZE || (option & (option - 1)))
					return 0;
				if (remount) {
					if (option != 512U << fs->sectors_per_buffer_bits)
						return 0;
					break;
				}
				fs->buffer_size = option;
				break;
			case Opt_prefetch:
				if (match_int(args, &option))
					return 0;
				if (option < 0) return 0;
				fs->metadata_prefetch = option >> 9;
				if (fs->metadata_prefetch & (fs->metadata_prefetch - 1))
					fs->metadata_prefetch = 1U << (fls(fs->metadata_prefetch) - 1);
				break;
			case Opt_sync_time:
				if (match_int(args, &option))
					return 0;
				if (option <= 0 ||
				    option >= (1UL << (BITS_PER_LONG - 1)) / HZ)
					return 0;
				fs->spadfs_sync_time = (unsigned long)option * HZ;
				break;
			case Opt_checksums:
				fs->mount_flags |=
					MOUNT_FLAGS_CHECKSUMS_OVERRIDE;
				fs->mount_flags |=
					MOUNT_FLAGS_CHECKSUMS;
				break;
			case Opt_no_checksums:
				fs->mount_flags |=
					MOUNT_FLAGS_CHECKSUMS_OVERRIDE;
				fs->mount_flags &=
					~MOUNT_FLAGS_CHECKSUMS;
				break;
			case Opt_ino64:
				match_strlcpy(str, args, sizeof str);
				if (!strcmp(str, "no")) {
					fs->mount_flags &=
						~(MOUNT_FLAGS_64BIT_INO |
						MOUNT_FLAGS_64BIT_INO_FORCE);
				} else if (!strcmp(str, "yes")) {
					fs->mount_flags |=
						MOUNT_FLAGS_64BIT_INO;
					fs->mount_flags &=
						~MOUNT_FLAGS_64BIT_INO_FORCE;
				} else if (!strcmp(str, "force")) {
					fs->mount_flags |=
						MOUNT_FLAGS_64BIT_INO |
						MOUNT_FLAGS_64BIT_INO_FORCE;
				} else {
					return 0;
				}
				break;
#ifdef SPADFS_QUOTA
			case Opt_usrquota:
				fs->mount_flags |= MOUNT_FLAGS_USRQUOTA;
				break;
			case Opt_grpquota:
				fs->mount_flags |= MOUNT_FLAGS_GRPQUOTA;
				break;
#endif
			default:
				return 0;
		}
	}
	return 1;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
static int spadfs_show_options(struct seq_file *seq, struct vfsmount *vfs)
{
	struct super_block *s = vfs->mnt_sb;
#else
static int spadfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *s = root->d_sb;
#endif
	SPADFS *fs = spadfs(s);

	if (fs->uid)
		seq_printf(seq, ",uid=%u", (unsigned)fs->uid);
	if (fs->gid)
		seq_printf(seq, ",gid=%u", (unsigned)fs->gid);
	seq_printf(seq, ",umask=%03o", (~fs->mode & 0777));
	seq_printf(seq, ",prealloc_part=%u", fs->prealloc_part);
	seq_printf(seq, ",prealloc_min=%u", fs->min_prealloc);
	seq_printf(seq, ",prealloc_max=%u", fs->max_prealloc);
	seq_printf(seq, ",xfer_size=%u", fs->xfer_size);
	seq_printf(seq, ",buffer_size=%u", 512U << fs->sectors_per_buffer_bits);
	seq_printf(seq, ",prefetch=%u", fs->metadata_prefetch << 9);
	seq_printf(seq, ",sync_time=%lu", fs->spadfs_sync_time / HZ);
	if (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS)
		seq_printf(seq, ",checksums");
	else
		seq_printf(seq, ",no_checksums");
	if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO))
		seq_printf(seq, ",ino64=no");
	else if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO_FORCE))
		seq_printf(seq, ",ino64=yes");
	else
		seq_printf(seq, ",ino64=force");
#ifdef SPADFS_QUOTA
	if (fs->mount_flags & MOUNT_FLAGS_USRQUOTA)
		seq_printf(seq, ",usrquota");
	if (fs->mount_flags & MOUNT_FLAGS_GRPQUOTA)
		seq_printf(seq, ",grpquota");
#endif

	return 0;
}

static void spadfs_help(void)
{
	printk("\n\
SPADFS filesystem options:\n\
        help                    display this text\n\
        uid=xxx                 set default uid of files\n\
        gid=xxx                 set default gid of files\n\
        umask=xxx               set default mode of files\n\
        prealloc_part=xxx       prealloc this part of existing file size\n\
                                (i.e. 8 means prealloc 1/8 of file size)\n\
        prealloc_min=xxx        minimum preallocation in bytes\n\
        prealloc_max=xxx        maximum preallocation in bytes\n\
        xfer_size=xxx           optimal request size reported in st_blksize\n\
        buffer_size=xxx         set kernel buffer size\n\
        prefetch=xxx            metadata prefetch in bytes\n\
        sync_time=xxx           sync after this interval in seconds\n\
        no_checksums            do not check and make checksums\n\
        checksums               do check and make checksums\n\
        ino64=no,yes,force      use 64-bit inode numbers\n\
        usrquota                user quota\n\
        grpquota                group quota\n\
\n");
}

/* Report error. flags is the mask of TXFLAGS_* that will be set in txblock */

__cold void spadfs_error_(SPADFS *fs, unsigned flags, const char *msg, ...)
{
	unsigned long irqstate;
	if (msg) {
		va_list va;
		va_start(va, msg);
		vprintk(msg, va);
		va_end(va);
		dump_stack();
	}
	spin_lock_irqsave(&fs->txflags_new_lock, irqstate);
	fs->txflags_new |= flags;
	spin_unlock_irqrestore(&fs->txflags_new_lock, irqstate);
}

__cold static void *spadfs_alloc(size_t n_elements, size_t element_size, size_t extra)
{
	size_t size;
	if (element_size && n_elements > ((size_t)-1 - extra) / element_size)
		return NULL;
	size = n_elements * element_size + extra;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,12,0) && !TEST_RHEL_VERSION(7,5)
	if (size <= PAGE_SIZE)
		return kmalloc(size, GFP_KERNEL);
	if (size <= KMALLOC_MAX_SIZE) {
		void *ptr = kmalloc(size, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
		if (ptr)
			return ptr;
	}
	return vmalloc(size);
#else
	return kvmalloc(size, GFP_KERNEL);
#endif
}

#ifndef SIMPLE_SYNC_LOCK

__cold static void spadfs_do_done_sync_lock(SPADFS *fs);

__cold static int spadfs_do_init_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++) {
		const unsigned size = max((unsigned)sizeof(struct rw_semaphore),
					  (unsigned)L1_CACHE_BYTES);
		int node = cpu_possible(i) ? cpu_to_node(i) : numa_node_id();
		/* (unsigned) kills warning */
		fs->sync_locks[i] = kmalloc_node(size, GFP_KERNEL, node);
		if (!fs->sync_locks[i]) {
			spadfs_do_done_sync_lock(fs);
			return 1;
		}
		init_rwsem(fs->sync_locks[i]);
	}
	return 0;
}

__cold static void spadfs_do_done_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++) {
		kfree(fs->sync_locks[i]);
		fs->sync_locks[i] = NULL;
	}
}

void spadfs_do_down_read_sync_lock(SPADFS *fs, unsigned *cpu)
{
	/*
	 * Use raw_smp_processor_id() instead of smp_processor_id() to
	 * suppress warning message about using it with preempt enabled.
	 * Preempt doesn't really matter here, locking the lock on
	 * a different CPU will cause a small performance impact but
	 * no race condition.
	 */
	unsigned cp = raw_smp_processor_id();
	*cpu = cp;
	down_read(fs->sync_locks[cp]);
}

void spadfs_do_up_read_sync_lock(SPADFS *fs, unsigned cpu)
{
	up_read(fs->sync_locks[cpu]);
}

void spadfs_do_down_write_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
		down_write_nested(fs->sync_locks[i], i);
#else
		down_write(fs->sync_locks[i]);
#endif
	}
}

void spadfs_do_up_write_sync_lock(SPADFS *fs)
{
	unsigned i;
	for (i = 0; i < spadfs_nr_cpus; i++)
		up_write(fs->sync_locks[i]);
}

#endif

int spadfs_stop_cycles(SPADFS *fs, sector_t key, sector_t (*c)[2],
		       const char *msg)
{
	if (unlikely((*c)[0] == key && unlikely((*c)[1] != 0))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"cycle detected on key %Lx in %s",
			(unsigned long long)key, msg);
		return 1;
	}
	(*c)[1]++;
	if (likely(!(((*c)[1] - 1) & (*c)[1])))
		(*c)[0] = key;
	return 0;
}

int spadfs_issue_flush(SPADFS *fs)
{
	int r;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,9)
	r = 0;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	r = blkdev_issue_flush(fs->s->s_bdev, NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
	r = blkdev_issue_flush(fs->s->s_bdev, GFP_NOFS, NULL, BLKDEV_IFL_WAIT);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,8,0) && !TEST_RHEL_VERSION(8,4)
	r = blkdev_issue_flush(fs->s->s_bdev, GFP_NOFS, NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,12,0) && !TEST_RHEL_VERSION(8,6)
	r = blkdev_issue_flush(fs->s->s_bdev, GFP_NOFS);
#else
	r = blkdev_issue_flush(fs->s->s_bdev);
#endif
	if (unlikely(r == -EOPNOTSUPP))
		r = 0;
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR, "flush error: %d", r);
	return r;
}

void spadfs_tx_block_checksum(struct txblock *tb)
{
	/*
	 * Was the incremental XOR trick: tb->checksum ^= CHECKSUM_BASE ^
	 * __byte_sum(tb, 512). That cannot survive a real CRC. XOR is its own
	 * inverse, so the old trick could fold a single byte into a running
	 * total; crc32c has no such inverse, and the trailer sits outside the
	 * covered range, so a self-referential update would fight it.
	 *
	 * The legacy field is zeroed to a deterministic value -- leaving it at
	 * whatever was there would make the CRC depend on stale bytes -- and is
	 * then covered by the trailer, which is recomputed over the whole unit.
	 */
	tb->checksum = 0;
	spadfs_meta_seal(tb);
}

static int spadfs_increase_cc(SPADFS *fs, int inc)
{
	struct txblock *tb;
	struct buffer_head *bh;
	int r, rr = 0;

	tb = spadfs_read_tx_block(fs, &bh, "spadfs_increase_cc");
	if (unlikely(IS_ERR(tb)))
		return PTR_ERR(tb);

	start_atomic_buffer_modify(fs, bh);
	if (likely(inc == 1)) {
		u16 cc = SPAD2CPU16_LV(&tb->cc);
		if (cc == 0xffff) {
			/* !!! FIXME: process wrap-around */
			spadfs_error(fs, TXFLAGS_FS_ERROR, "crash count wrap-around, remounting read-only");
			rr = 1;
			goto skip_inc;
		}
		cc += inc;
		CPU2SPAD16_LV(&tb->cc, cc);
		if (unlikely(SPAD2CPU32_LV(&fs->cct[cc]))) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"crash count table is damaged (non-zero at %04x: %08x)",
				(unsigned)SPAD2CPU16_LV(&tb->cc),
				(unsigned)SPAD2CPU32_LV(&fs->cct[cc]));
			rr = -EFSERROR;
		}
		fs->cc = cc - 1;
		fs->txc = SPAD2CPU32_LV(&fs->cct[fs->cc]);
	} else if (unlikely(inc == -1)) {
		CPU2SPAD16_LV(&tb->cc, SPAD2CPU16_LV(&tb->cc) + inc);
		/* If unmounting, clean up preallocated inodes */
		spin_lock(&fs->stable_ino_lock);
		if (fs->stable_ino) {
			CPU2SPAD64_LV(&tb->ino, fs->stable_ino);
			fs->stable_ino = 0;
		}
		spin_unlock(&fs->stable_ino_lock);
	}

skip_inc:
	spin_lock_irq(&fs->txflags_new_lock);
	fs->txflags |= fs->txflags_new;
	spin_unlock_irq(&fs->txflags_new_lock);

	CPU2SPAD32_LV(&tb->txflags, fs->txflags);
	spadfs_tx_block_checksum(tb);
	end_atomic_buffer_modify(fs, bh);

	r = spadfs_sync_dirty_buffer(bh);
	if (likely(!r))
		r = spadfs_issue_flush(fs);
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
			"write error at tx block: %d", r);

	spadfs_brelse(fs, bh);
	return unlikely(r) ? r : rr;
}

static int spadfs_update_error_flags(SPADFS *fs)
{
	/*
	 * Don't lock when reading txflags_new --- if we miss an update here,
	 * it would be updated later.
	 */
	if (unlikely(fs->txflags_new & ~fs->txflags)) {
		if (sb_rdonly(fs->s))
			return 0;
		return spadfs_increase_cc(fs, 0);
	}
	return 0;
}

static noinline int spadfs_new_stable_ino_slow(SPADFS *fs, u64 *stable_ino)
{
	int r;
	struct txblock *tb;
	struct buffer_head *bh;
	u64 next_stable_ino;

	/*sync_lock_decl*/
	down_write_sync_lock(fs);

	spin_lock(&fs->stable_ino_lock);
	*stable_ino = fs->stable_ino;
	if (unlikely((*stable_ino & (SPADFS_INO_BATCH - 1)) != 0)) {
		fs->stable_ino++;
		spin_unlock(&fs->stable_ino_lock);
		r = 0;
		goto unlock_ret_r;
	}
	spin_unlock(&fs->stable_ino_lock);

	tb = spadfs_read_tx_block(fs, &bh, "spadfs_new_stable_ino_slow");
	if (unlikely(IS_ERR(tb))) {
		r = PTR_ERR(tb);
		goto unlock_ret_r;
	}

	start_atomic_buffer_modify(fs, bh);

	*stable_ino = SPAD2CPU64_LV(&tb->ino);
	if (unlikely(!*stable_ino))
		*stable_ino = SPADFS_INO_ROOT + 1;
	next_stable_ino = (*stable_ino | (SPADFS_INO_BATCH - 1)) + 1;
	if (unlikely(next_stable_ino == SPADFS_INO_INITIAL_REGION))
		next_stable_ino = (u64)SPADFS_INO_INITIAL_REGION * 2;
	if (unlikely(!(next_stable_ino & 0xFFFFFFFF)))
		next_stable_ino |= (u64)SPADFS_INO_INITIAL_REGION * 2;
	CPU2SPAD64_LV(&tb->ino, next_stable_ino);

	spin_lock(&fs->stable_ino_lock);
	fs->stable_ino = *stable_ino + 1;
	spin_unlock(&fs->stable_ino_lock);

	spadfs_tx_block_checksum(tb);

	end_atomic_buffer_modify(fs, bh);
	spadfs_brelse(fs, bh);

	r = 0;

unlock_ret_r:
	up_write_sync_lock(fs);
	return r;
}

int spadfs_new_stable_ino(SPADFS *fs, u64 *stable_ino)
{
	if (unlikely(sb_rdonly(fs->s)))
		return -EROFS;
	spin_lock(&fs->stable_ino_lock);
	if (unlikely(!((*stable_ino = fs->stable_ino) &
		       (SPADFS_INO_BATCH - 1)))) {
		spin_unlock(&fs->stable_ino_lock);
		return spadfs_new_stable_ino_slow(fs, stable_ino);
	}
	fs->stable_ino++;
	spin_unlock(&fs->stable_ino_lock);
	return 0;
}

static int spadfs_start_new_tx(SPADFS *fs)
{
	int r;
	if (unlikely(!fs->max_allocation))
		fs->max_allocation = 1U << fs->sectors_per_disk_block_bits;
	if (unlikely(SPAD2CPU32_LV(&fs->cct[fs->cc]) == 0x7fffffff)) {
		if (unlikely(r = spadfs_increase_cc(fs, 1))) {
			if (r > 0)
				return 0;	/* it is read-only now */
			return r;
		}
	} else {
		if (unlikely(r = spadfs_update_error_flags(fs)))
			return r;
	}
	CPU2SPAD32_LV(&fs->cct[fs->cc], SPAD2CPU32_LV(&fs->cct[fs->cc]) + 1);
	fs->txc = SPAD2CPU32_LV(&fs->cct[fs->cc]);
	return 0;
}

/*
 * Mirror the cct sectors just written into the side CRC region (Phase 2.3, A5).
 *
 * Only the unit(s) covering [first, last] are rewritten. With 127 CRCs per
 * unit and a buffer of a power-of-two sector count, a commit normally touches
 * one unit; the loop covers the straddle case rather than assuming it away.
 *
 * The unit is fully packed -- 127 * 4 = 508 = SPADFS_META_COVER -- so the
 * trailer lands exactly at the end with no gap.
 *
 * BLOCK GRANULARITY, NOT SECTOR GRANULARITY.
 *
 * spadfs_get_new_sector() hands out a whole block (buffer.c:224 does
 * sb_getblk(fs->s, secno >> sectors_per_buffer_bits)) and does not read the
 * existing contents first. So writing one 512-byte unit writes every sector
 * in that unit's block -- and the other sectors in that block hold the other
 * units. A unit-by-unit write therefore destroys its neighbours.
 *
 * Observed after a real mount + write session, where only unit 0 is ever
 * updated (cc is small, so the touched cct sector is 0):
 *
 *   CCT CRC UNIT 1 CHECKSUM MISMATCH
 *   CCT CRC UNIT 2 CHECKSUM MISMATCH
 *   CCT CRC UNIT 3 CHECKSUM MISMATCH
 *   CCT CRC UNIT 4 CHECKSUM MISMATCH
 *
 * -- units 1..4 zeroed by unit 0's block write, so their trailer reads 0
 * against a non-zero crc32c of zeros. Unit 0 verified fine. mkspadfs does
 * not hit this because it writes with its own sector-granular write_blocks,
 * which is why every unprivileged test passed.
 *
 * Fix: operate per block. Every unit sharing a block with a unit we must
 * change is regenerated from the in-RAM cct_crc[] mirror, so no sector in
 * the written block is left to whatever the page cache happened to hold.
 */
/*
 * The same writer serves the primary CRC region and the backup's (Task 3.3):
 * base is the region start, [first_unit, last_unit] the units that must be
 * rewritten, and stamped puts the backup epoch stamp (struct.h) into the
 * slack of the last unit before it is sealed. The primary leaves that slack
 * zero. base must be block-aligned; validate_super() guarantees it, and an
 * unaligned base would make blk_first_unit wrap.
 */
static int spadfs_write_cct_crc(SPADFS *fs, sector_t base,
				unsigned first_unit, unsigned last_unit,
				int stamped)
{
	unsigned nsec = 1U << fs->sectors_per_buffer_bits;
	unsigned b, s, i;
	int rr = 0;

	if (last_unit >= CCT_CRC_UNITS)
		last_unit = CCT_CRC_UNITS - 1;

	/* the blocks holding the units in range */
	for (b = (base + first_unit) >> fs->sectors_per_buffer_bits;
	     b <= (base + last_unit) >> fs->sectors_per_buffer_bits;
	     b++) {
		struct buffer_head *cbh;
		sector_t blk_first_unit =
			((sector_t)b << fs->sectors_per_buffer_bits) - base;
		__u8 *page = spadfs_get_new_sector(fs,
					b << fs->sectors_per_buffer_bits,
					&cbh, "spadfs_write_cct_crc");

		if (unlikely(IS_ERR(page)))
			return PTR_ERR(page);

		start_atomic_buffer_modify(fs, cbh);
		/*
		 * Zero first: the units do not fill the last block
		 * (CCT_CRC_UNITS is not a multiple of block_sectors), and a
		 * deterministic zero is better than stale page-cache bytes in
		 * the padding.
		 */
		memset(page, 0, nsec * 512);

		for (s = 0; s < nsec; s++) {
			unsigned u = (unsigned)(blk_first_unit + s);
			__u8 *unit = page + s * 512;

			if (u >= CCT_CRC_UNITS)
				continue;
			for (i = 0; i < CCT_CRC_PER_UNIT; i++) {
				unsigned sector = u * CCT_CRC_PER_UNIT + i;

				spadfs_meta_put_crc(unit + i * 4,
						    sector < CCT_SECTORS ?
						    fs->cct_crc[sector] : 0);
			}
			if (stamped && u == CCT_BACKUP_STAMP_UNIT) {
				struct cct_backup_stamp *st = (struct cct_backup_stamp *)
					(unit + CCT_BACKUP_STAMP_OFF);

				/*
				 * fs->cct[fs->cc] is still the value written to
				 * disk: callers run before spadfs_start_new_tx.
				 */
				CPU2SPAD32_LV(&st->magic, CCT_BACKUP_STAMP_MAGIC);
				CPU2SPAD32_LV(&st->cc, fs->cc);
				CPU2SPAD32_LV(&st->txc, SPAD2CPU32_LV(&fs->cct[fs->cc]));
				CPU2SPAD32_LV(&st->reserved, 0);
			}
			spadfs_meta_seal(unit);
		}
		end_atomic_buffer_modify(fs, cbh);

		{
			int r = spadfs_sync_dirty_buffer(cbh);

			spadfs_brelse(fs, cbh);
			if (unlikely(r))
				rr = r;
		}
	}
	return rr;
}

/*
 * The cct backup mirror (Task 3.3).
 *
 * The mirror is only worth restoring if it is current, so the kernel keeps
 * it so: one full copy when the volume goes read-write, then on every commit
 * the same cct block the primary got, plus the stamped CRC unit.
 *
 * A mirror failure never fails a commit or a mount. Losing the backup loses
 * a recovery option; failing the commit would lose the filesystem. On any
 * mirror error the mirror stops being updated for the rest of the mount, and
 * because the stamp is then left behind, fsck will refuse to restore from it
 * without asking.
 */
/*
 * Clear the backup's epoch stamp before the primary moves on without it.
 *
 * A valid stamp promises the mirror is at most the commit in progress behind
 * (struct.h). Once we stop mirroring, that promise is false, and a stamp left
 * in place would let fsck restore an arbitrarily old mirror unattended.
 * Clearing it makes fsck ask a human instead.
 *
 * Read-modify-write of the one stamp unit: unlike spadfs_write_cct_crc this
 * must not regenerate the other units from fs->cct_crc[], which describes
 * the PRIMARY -- the mirror's CRCs are only equal to it while the mirror is
 * live. A unit that does not verify is left alone: it already makes fsck
 * reject the mirror, and resealing it would only hide that.
 */
static void spadfs_cct_backup_unstamp(SPADFS *fs)
{
	struct buffer_head *bh;
	__u8 *unit;
	struct cct_backup_stamp *st;
	int r;

	if (!fs->cct_backup_sec)
		return;
	unit = spadfs_read_sector(fs, fs->cct_backup_crc_sec + CCT_BACKUP_STAMP_UNIT,
				  &bh, 1, "spadfs_cct_backup_unstamp");
	if (unlikely(IS_ERR(unit))) {
		printk(KERN_WARNING "spadfs: cannot read the crash count table backup stamp (%ld); run spadfsck before trusting the backup\n",
		       PTR_ERR(unit));
		return;
	}
	st = (struct cct_backup_stamp *)(unit + CCT_BACKUP_STAMP_OFF);
	if (spadfs_meta_verify(unit) ||
	    SPAD2CPU32_LV(&st->magic) != CCT_BACKUP_STAMP_MAGIC) {
		spadfs_brelse(fs, bh);
		return;
	}
	start_atomic_buffer_modify(fs, bh);
	memset(st, 0, sizeof(*st));
	spadfs_meta_seal(unit);
	end_atomic_buffer_modify(fs, bh);
	r = spadfs_sync_dirty_buffer(bh);
	spadfs_brelse(fs, bh);
	if (likely(!r))
		r = spadfs_issue_flush(fs);
	if (unlikely(r))
		printk(KERN_WARNING "spadfs: cannot clear the crash count table backup stamp (%d); run spadfsck before trusting the backup\n",
		       r);
}

static void spadfs_cct_backup_failed(SPADFS *fs, int r)
{
	printk(KERN_WARNING "spadfs: write error %d in the crash count table backup; not updating it again this mount, run spadfsck to refresh it\n",
	       r);
	fs->cct_backup_live = 0;
	spadfs_cct_backup_unstamp(fs);
}

/* Write cct sectors [first, first + n) to the mirror, n a whole buffer count. */
static int spadfs_cct_backup_write_data(SPADFS *fs, unsigned first, unsigned n,
					int sync_each)
{
	unsigned nsec = 1U << fs->sectors_per_buffer_bits;
	unsigned i;

	for (i = first; i < first + n; i += nsec) {
		struct buffer_head *bh;
		void *data = spadfs_get_new_sector(fs, fs->cct_backup_sec + i,
						   &bh, "spadfs_cct_backup");

		if (unlikely(IS_ERR(data)))
			return PTR_ERR(data);
		start_atomic_buffer_modify(fs, bh);
		spadfs_memcpy(data, (char *)fs->cct + (i << 9), nsec * 512);
		end_atomic_buffer_modify(fs, bh);
		if (sync_each) {
			int r = spadfs_sync_dirty_buffer(bh);

			spadfs_brelse(fs, bh);
			if (unlikely(r))
				return r;
		} else {
			spadfs_brelse(fs, bh);
		}
	}
	return 0;
}

/*
 * Make the mirror a full copy of the cct, stamped with this mount's cc.
 * Called before spadfs_increase_cc(fs, 1) when the volume goes read-write, so
 * fs->cct is still exactly the on-disk primary and fs->cc is the crash count
 * this mount will work in. Stamping before the tx block moves on means fsck
 * sees the stamp as current whether a crash lands before or after it does.
 *
 * Refuses unless the primary matched every stored CRC at mount: copying a
 * damaged primary over the backup would destroy the copy fsck needs.
 */
static void spadfs_cct_backup_start(SPADFS *fs)
{
	int r;

	if (fs->cct_backup_live || !fs->cct_backup_sec)
		return;
	if (!fs->cct_verified) {
		printk(KERN_WARNING "spadfs: crash count table did not verify at mount; leaving its backup untouched, run spadfsck\n");
		/* the primary will move on without it: withdraw the stamp */
		spadfs_cct_backup_unstamp(fs);
		return;
	}
	r = spadfs_cct_backup_write_data(fs, 0, CCT_SECTORS, 0);
	if (likely(!r))
		r = sync_blockdev(fs->s->s_bdev);
	if (likely(!r))
		r = spadfs_write_cct_crc(fs, fs->cct_backup_crc_sec,
					 0, CCT_BACKUP_CRC_UNITS - 1, 1);
	if (likely(!r))
		r = spadfs_issue_flush(fs);
	if (unlikely(r)) {
		spadfs_cct_backup_failed(fs, r);
		return;
	}
	fs->cct_backup_live = 1;
}

/*
 * Mirror one commit: the cct block the primary just got, its CRC unit(s),
 * and the stamp unit (which holds the new txc even when the touched sector
 * lives in another unit). Runs after the primary is durable, so a crash here
 * leaves a good primary and, at worst, a mirror fsck must recreate.
 */
static void spadfs_cct_backup_commit(SPADFS *fs, unsigned first,
				     unsigned last)
{
	unsigned first_unit = first / CCT_CRC_PER_UNIT;
	unsigned last_unit = last / CCT_CRC_PER_UNIT;
	int r;

	r = spadfs_cct_backup_write_data(fs, first, last - first + 1, 1);
	if (likely(!r))
		r = spadfs_write_cct_crc(fs, fs->cct_backup_crc_sec,
					 first_unit, last_unit, 1);
	/* the stamp unit, unless the write above already covered its block */
	if (likely(!r) &&
	    (fs->cct_backup_crc_sec + CCT_BACKUP_STAMP_UNIT) >> fs->sectors_per_buffer_bits !=
	    (fs->cct_backup_crc_sec + last_unit) >> fs->sectors_per_buffer_bits)
		r = spadfs_write_cct_crc(fs, fs->cct_backup_crc_sec,
					 CCT_BACKUP_STAMP_UNIT,
					 CCT_BACKUP_STAMP_UNIT, 1);
	if (likely(!r))
		r = spadfs_issue_flush(fs);
	if (unlikely(r))
		spadfs_cct_backup_failed(fs, r);
}

int spadfs_commit_unlocked(SPADFS *fs)
{
	int r, rr;
	unsigned sector;
	void *cct_data;
	struct buffer_head *bh;

	assert_write_sync_lock(fs);

	fs->commit_sequence++;

	if (unlikely(sb_rdonly(fs->s)))
		return -EFSERROR;

	while (unlikely(!list_empty(&fs->clear_list))) {
		SPADFNODE *f = list_entry(fs->clear_list.prev, SPADFNODE, clear_entry);
		spadfs_clear_last_block(&f->vfs_inode);
	}

	mutex_lock(&fs->alloc_lock);
#ifdef SPADFS_META_PREALLOC
	spadfs_prealloc_discard_unlocked(fs, &fs->meta_prealloc);
#endif
	spadfs_prealloc_discard_unlocked(fs, &fs->small_prealloc);
	if (unlikely(fs->max_freed_run > fs->max_allocation))
		fs->max_allocation = fs->max_freed_run;
	fs->max_freed_run = 0;
	mutex_unlock(&fs->alloc_lock);

#ifdef SPADFS_QUOTA
	mutex_lock(&fs->quota_alloc_lock);
#endif

	r = sync_blockdev(fs->s->s_bdev);
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR, "write error: %d", r);

	sector = fs->cc / (512 / 4);
	sector &= ~((1U << fs->sectors_per_buffer_bits) - 1);
	cct_data = spadfs_get_new_sector(fs, fs->cct_sec + sector, &bh,
					 "spadfs_commit_unlocked");
	if (unlikely(IS_ERR(cct_data))) {
		if (!r)
			r = PTR_ERR(cct_data);
		goto unlock_ret;
	}

	start_atomic_buffer_modify(fs, bh);
	spadfs_memcpy(cct_data, (char *)fs->cct + (sector << 9), 512U << fs->sectors_per_buffer_bits);
	end_atomic_buffer_modify(fs, bh);

	rr = spadfs_sync_dirty_buffer(bh);
	spadfs_brelse(fs, bh);

	if (unlikely(rr != 0)) {
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
			"write error in crash count table: %d", rr);
		spadfs_update_error_flags(fs);
		if (!r)
			r = rr;
		goto unlock_ret;
	}

	/*
	 * Compute and write the side CRCs for the cct sectors we just pushed,
	 * then flush both together. That flush is the commit point.
	 *
	 * The CRCs MUST be computed before spadfs_start_new_tx: that call
	 * increments fs->cct[fs->cc], and a CRC taken after the increment would
	 * describe bytes that are not the bytes on disk. Computing from the same
	 * snapshot the memcpy used is what keeps the pair meaningful.
	 *
	 * Data and CRC go out under ONE flush. Before Task 3.3 the CRC was
	 * written after the flush and never flushed itself, so a crash after the
	 * commit returned -- i.e. after fsync promised durability -- could leave
	 * a durable cct with a stale CRC. fsck then calls the primary damaged
	 * and the only fix is the backup, which is one commit older: an
	 * acknowledged commit lost. Now, once this flush completes, both halves
	 * are durable; a crash before it can leave them disagreeing, but that
	 * commit was never acknowledged, so falling back to the backup (the
	 * previous commit) is a correct outcome.
	 */
	{
		unsigned nsec = 1U << fs->sectors_per_buffer_bits;
		unsigned last = sector + nsec - 1;
		unsigned sc;

		if (last >= CCT_SECTORS)
			last = CCT_SECTORS - 1;
		for (sc = sector; sc <= last; sc++)
			fs->cct_crc[sc] = spadfs_crc32c_std((char *)fs->cct + (sc << 9), 512);

		if (likely(fs->cct_crc_sec)) {
			rr = spadfs_write_cct_crc(fs, fs->cct_crc_sec,
						  sector / CCT_CRC_PER_UNIT,
						  last / CCT_CRC_PER_UNIT, 0);
			if (unlikely(rr)) {
				spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
					"write error in cct crc table: %d", rr);
				spadfs_update_error_flags(fs);
				if (!r)
					r = rr;
				goto unlock_ret;
			}
		}

		rr = spadfs_issue_flush(fs);
		if (unlikely(rr)) {
			spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
				"write error in crash count table: %d", rr);
			spadfs_update_error_flags(fs);
			if (!r)
				r = rr;
			goto unlock_ret;
		}

		/*
		 * The primary is durable; now bring the mirror to the same
		 * state. Its own flush means the mirror is never left behind
		 * by more than the commit in progress. Never fails the commit.
		 */
		if (fs->cct_backup_live)
			spadfs_cct_backup_commit(fs, sector, last);
	}

	rr = spadfs_start_new_tx(fs);
	if (unlikely(rr)) {
		if (!r)
			r = rr;
	}

	if (likely(!r))
		fs->need_background_sync = 0;

unlock_ret:

#ifdef SPADFS_QUOTA
	mutex_unlock(&fs->quota_alloc_lock);
#endif

	return r;
}

int spadfs_commit(SPADFS *fs)
{
	/*sync_lock_decl*/
	int r, rr;

	if (unlikely(sb_rdonly(fs->s)))
		return 0;

	/*
	 * Improves performance very much --- do most of the syncing
	 * outside the lock.
	 */
	r = sync_blockdev(fs->s->s_bdev);
	if (unlikely(r))
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR, "write error: %d", r);

	down_write_sync_lock(fs);
	rr = spadfs_commit_unlocked(fs);
	if (unlikely(rr))
		r = rr;
	up_write_sync_lock(fs);

	return r;
}

static int spadfs_sync_fs(struct super_block *s, int wait)
{
	SPADFS *fs = spadfs(s);
	if (!READ_ONCE(fs->need_background_sync))
		return 0;
	else if (!wait)
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,10,0)
		return filemap_fdatawrite(s->s_bdev->bd_inode->i_mapping);
#else
		return filemap_fdatawrite(s->s_bdev->bd_mapping);
#endif
	else
		return spadfs_commit(spadfs(s));
}

#ifndef NEW_WORKQUEUE
static void spadfs_sync_work(void *fs_)
{
	SPADFS *fs = fs_;
#else
static void spadfs_sync_work(struct work_struct *w)
{
	SPADFS *fs = container_of(w, SPADFS, spadfs_sync_work.work);
#endif
	spadfs_reclaim_max_allocation(fs);
	if (READ_ONCE(fs->need_background_sync)) {
		spadfs_commit(fs);
	}
	queue_delayed_work(fs->spadfs_syncer, &fs->spadfs_sync_work,
			   fs->spadfs_sync_time);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
static int spadfs_statfs(struct dentry *dentry, struct kstatfs *ks)
{
	struct super_block *s = dentry->d_sb;
#else
static int spadfs_statfs(struct super_block *s, struct kstatfs *ks)
{
#endif
	SPADFS *fs = spadfs(s);
	sector_t freespace;

	mutex_lock(&fs->alloc_lock);

	ks->f_type = s->s_magic;
	ks->f_bsize = 512U << fs->sectors_per_disk_block_bits;
	ks->f_blocks = fs->size >> fs->sectors_per_disk_block_bits;

	freespace = spadfs_get_freespace(fs);
	if (likely(freespace >= fs->alloc_mem_sectors))
		freespace -= fs->alloc_mem_sectors;
	else
		freespace = 0;

	ks->f_bfree = freespace >> fs->sectors_per_disk_block_bits;
	ks->f_bavail = likely(freespace >= fs->reserve_sectors) ?
		(freespace - fs->reserve_sectors) >> fs->sectors_per_disk_block_bits :
		0;

	ks->f_files = (u64)fs->zones[0].grp_n << (fs->sectors_per_group_bits - fs->sectors_per_disk_block_bits);
	if (unlikely(ks->f_files > ks->f_blocks))
		ks->f_files = ks->f_blocks;
	ks->f_ffree = fs->zones[0].freespace >> fs->sectors_per_disk_block_bits;

	ks->f_namelen = MAX_NAME_LEN;

	mutex_unlock(&fs->alloc_lock);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
struct spadfs_fc_context {
	unsigned uid;
	unsigned gid;
	umode_t umask;
	unsigned prealloc_part;
	unsigned prealloc_min;
	unsigned prealloc_max;
	unsigned xfer_size;
	unsigned buffer_size;
	unsigned prefetch;
	unsigned long sync_time;
	bool no_checksums;
	bool checksums;
	int ino64;
#ifdef SPADFS_QUOTA
	bool usrquota;
	bool grpquota;
#endif
};

static void spadfs_setup_params(SPADFS *fs, struct fs_context *fc)
{
	struct spadfs_fc_context *ctx = fc->fs_private;
	fs->uid = ctx->uid;
	fs->gid = ctx->gid;
	fs->mode = 0777 & ~ctx->umask;
	set_prealloc_part(fs, ctx->prealloc_part);
	fs->min_prealloc = ctx->prealloc_min;
	fs->max_prealloc = ctx->prealloc_max;
	fs->xfer_size = ctx->xfer_size;
	fs->buffer_size = ctx->buffer_size;
	fs->metadata_prefetch = ctx->prefetch;
	fs->spadfs_sync_time = ctx->sync_time;
	if (ctx->no_checksums) {
		fs->mount_flags |= MOUNT_FLAGS_CHECKSUMS_OVERRIDE;
		fs->mount_flags &= ~MOUNT_FLAGS_CHECKSUMS;
	} else if (ctx->checksums) {
		fs->mount_flags |= MOUNT_FLAGS_CHECKSUMS_OVERRIDE;
		fs->mount_flags |= MOUNT_FLAGS_CHECKSUMS;
	}
	if (ctx->ino64 == 0) {
		fs->mount_flags &= ~(MOUNT_FLAGS_64BIT_INO | MOUNT_FLAGS_64BIT_INO_FORCE);
	} else if (ctx->ino64 == 1) {
		fs->mount_flags |= MOUNT_FLAGS_64BIT_INO;
		fs->mount_flags &= ~MOUNT_FLAGS_64BIT_INO_FORCE;
	} else if (ctx->ino64 == 2) {
		fs->mount_flags |= MOUNT_FLAGS_64BIT_INO | MOUNT_FLAGS_64BIT_INO_FORCE;
	}
#ifdef SPADFS_QUOTA
	if (ctx->usrquota)
		fs->mount_flags |= MOUNT_FLAGS_USRQUOTA;
	if (ctx->grpquota)
		fs->mount_flags |= MOUNT_FLAGS_GRPQUOTA;
#endif
}

static int spadfs_reconfigure(struct fs_context *fc)
{
	int r;
	struct super_block *s = fc->root->d_sb;
	SPADFS *fs = spadfs(s);
	bool remount_rw = !(fc->sb_flags & SB_RDONLY) && sb_rdonly(s);
	bool remount_ro = (fc->sb_flags & SB_RDONLY) && !sb_rdonly(s);

	down_write_sync_lock(fs);

	if (remount_ro) {
#ifdef SPADFS_QUOTA
		if ((r = dquot_suspend(s, -1))) {
			goto unlock_ret_r;
		}
#endif
		if ((r = spadfs_commit_unlocked(fs)))
			goto unlock_ret_r;
		if ((r = spadfs_increase_cc(fs, -1)))
			goto unlock_ret_r;
		CPU2SPAD32_LV(&fs->cct[fs->cc],
			      SPAD2CPU32_LV(&fs->cct[fs->cc]) - 1);
	}
	spadfs_setup_params(fs, fc);
	if (remount_rw) {
		spadfs_cct_backup_start(fs);
		if ((r = spadfs_increase_cc(fs, 1))) {
			if (r < 0)
				goto unlock_ret_r;
		}
		if ((r = spadfs_start_new_tx(fs)))
			goto unlock_ret_r;
#ifdef SPADFS_QUOTA
		dquot_resume(s, -1);
#endif
	}
	r = 0;

unlock_ret_r:
	up_write_sync_lock(fs);
	return r;
}
#else
__cold static int spadfs_remount_fs(struct super_block *s, int *flags, char *data)
{
	int r, o;
	SPADFS *fs = spadfs(s);
	/*sync_lock_decl*/

	down_write_sync_lock(fs);

	o = parse_opts(fs, data, 1);
	if (!o) {
		r = -EINVAL;
		goto unlock_ret_r;
	}
	if (o == 2) {
		spadfs_help();
		r = -EAGAIN;
		goto unlock_ret_r;
	}

	if ((*flags ^ s->s_flags) & SB_RDONLY) {
		if (*flags & SB_RDONLY) {
#ifdef SPADFS_QUOTA
			if ((r = dquot_suspend(s, -1))) {
				*flags &= ~SB_RDONLY;
				goto unlock_ret_r;
			}
#endif
			if ((r = spadfs_commit_unlocked(fs)))
				goto unlock_ret_r;
			if ((r = spadfs_increase_cc(fs, -1)))
				goto unlock_ret_r;
			CPU2SPAD32_LV(&fs->cct[fs->cc],
				      SPAD2CPU32_LV(&fs->cct[fs->cc]) - 1);
		} else {
			spadfs_cct_backup_start(fs);
			if ((r = spadfs_increase_cc(fs, 1))) {
				if (r < 0)
					goto unlock_ret_r;
				*flags |= SB_RDONLY;
			}
			if ((r = spadfs_start_new_tx(fs)))
				goto unlock_ret_r;
#ifdef SPADFS_QUOTA
			dquot_resume(s, -1);
#endif
		}
	}

	r = 0;

unlock_ret_r:
	up_write_sync_lock(fs);
	return r;
}
#endif

#if defined(SPADFS_QUOTA) && SPADFS_QUOTA >= 3
static const struct quotactl_ops spadfs_quotactl_ops = {
	.quota_on = dquot_quota_on,
	.quota_off = dquot_quota_off,
	.quota_sync = dquot_quota_sync,
	.get_state = dquot_get_state,
	.set_info = dquot_set_dqinfo,
	.get_dqblk = dquot_get_dqblk,
	.set_dqblk = dquot_set_dqblk,
	.get_nextdqblk = dquot_get_next_dqblk,
};
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
__cold static int spadfs_fill_super(struct super_block *s, struct fs_context *fc)
#else
__cold static int spadfs_fill_super(struct super_block *s, void *options, int silent)
#endif
{
	SPADFS *fs;
	const char *wq_name =
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
		"spadfssync";
#else
		"spadfs_syncer";
#endif
	int blocksize;
	int r;
	unsigned i, n;
	sector_t sec;
	struct buffer_head *bh;
	struct superblock *sb;
	struct txblock *tb;
	const char *msg;
	int cycle;
	struct inode *root;
	size_t sizeof_spadfs = sizeof(SPADFS)
#ifndef SIMPLE_SYNC_LOCK
		+ spadfs_nr_cpus * sizeof(struct rw_semaphore *)
#endif
		;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
	int silent = !!(fc->sb_flags & SB_SILENT);
#endif

	sb = kmalloc(512, GFP_KERNEL);
	if (unlikely(!sb)) {
		r = -ENOMEM;
		goto err0;
	}

	if (unlikely(!(fs = spadfs_alloc(1, sizeof_spadfs, 0)))) {
		r = -ENOMEM;
		goto err1;
	}

	s->s_fs_info = fs;
	s->s_flags |= SB_NOATIME;
	s->s_magic = MEMORY_SUPER_MAGIC;
	s->s_op = &spadfs_sops;
#ifdef SPADFS_QUOTA
	s->dq_op = &dquot_operations;
#if SPADFS_QUOTA >= 3
	s->s_qcop = &spadfs_quotactl_ops;
#endif
#if SPADFS_QUOTA >= 2
	s->s_quota_types = QTYPE_MASK_USR | QTYPE_MASK_GRP;
#endif
#endif
#ifdef SPADFS_XATTR
	s->s_xattr =
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
		(struct xattr_handler **)
#endif
		spadfs_xattr_handlers;
#endif
	if (sizeof(sector_t) == 4)
		s->s_maxbytes = ((u64)512 << 32) - 512;
	else
		s->s_maxbytes = ((u64)512 << 48) - 512;
	if (s->s_maxbytes > MAX_LFS_FILESIZE)
		s->s_maxbytes = MAX_LFS_FILESIZE;
	memset(fs, 0, sizeof_spadfs);
	fs->s = s;
	mutex_init(&fs->alloc_lock);
	spadfs_allocmem_init(fs);
	fs->alloc_reservations = RB_ROOT;
	mutex_init(&fs->inode_list_lock);
	INIT_LIST_HEAD(&fs->apage_lru);
#ifdef SPADFS_QUOTA
	mutex_init(&fs->quota_alloc_lock);
#endif
	spin_lock_init(&fs->stable_ino_lock);
	spin_lock_init(&fs->txflags_new_lock);
	fs->commit_sequence = 1;
	mutex_init(&fs->trim_lock);

	spin_lock_init(&fs->clear_lock);
	INIT_LIST_HEAD(&fs->clear_list);

	spadfs_buffer_leaks_init(fs);

	init_sync_lock(fs, r = -ENOMEM; goto err2;);

	fs->uid = get_current_uid();
	fs->gid = get_current_gid();
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
	fs->mode = 0777 & ~current->fs->umask;
#else
	fs->mode = 0777 & ~current_umask();
#endif
	set_prealloc_part(fs, SPADFS_DEFAULT_PREALLOC_PART);
	fs->min_prealloc = SPADFS_DEFAULT_MIN_PREALLOC;
	fs->max_prealloc = SPADFS_DEFAULT_MAX_PREALLOC;
	fs->xfer_size = 0;
	fs->metadata_prefetch = SPADFS_DEFAULT_METADATA_PREFETCH >> 9;
	fs->spadfs_sync_time = SPADFS_DEFAULT_SYNC_TIME;

#ifdef CHECK_ALLOCMEM
	/*
	 * Unit test for allocmem. It is normally not used much, so we
	 * must stress-test it here.
	 */
	r = spadfs_allocmem_unit_test(fs);
	if (r)
		goto err2;
#endif

	fs->inode_list = spadfs_alloc(SPADFS_INODE_HASH_SIZE, sizeof(struct hlist_node), 0);
	if (!fs->inode_list) {
		r = -ENOMEM;
		goto err2;
	}
	for (i = 0; i < SPADFS_INODE_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&fs->inode_list[i]);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
	spadfs_setup_params(fs, fc);
#else
	{
		int o = parse_opts(fs, options, 0);
		if (!o) {
			r = -EINVAL;
			goto err2;
		}
		if (o == 2) {
			spadfs_help();
			r = -EAGAIN;
			goto err2;
		}
	}
#endif

	if (!fs->buffer_size) {
		blocksize = sb_min_blocksize(s, 512);
		if (unlikely(!blocksize)) {
			if (!silent)
				printk(KERN_ERR "spadfs: unable to set blocksize\n");
			r = -EIO;
			goto err2;
		}
	} else {
		if (unlikely(!sb_set_blocksize(s, fs->buffer_size))) {
			printk(KERN_ERR "spadfs: can't set blocksize %d\n", fs->buffer_size);
			r = -EINVAL;
			goto err2;
		}
		blocksize = fs->buffer_size;
	}

	cycle = 0;

read_again:
	if (unlikely(blocksize & (blocksize - 1)) || unlikely(blocksize < 512))
		panic("spadfs: wrong blocksize: %d", blocksize);
	fs->sectors_per_buffer_bits = ffs(blocksize) - 1 - 9;
	fs->size = (sector_t)-1;

	bh = sb_bread(s, SUPERBLOCK_SECTOR >> fs->sectors_per_buffer_bits);
	if (!bh) {
		if (!silent)
			printk(KERN_ERR "spadfs: unable to read super block\n");
		r = -EIO;
		goto err2;
	}
	spadfs_memcpy(sb, spadfs_buffer_data(fs, SUPERBLOCK_SECTOR, bh), 512);
	__brelse(bh);

	if (unlikely(memcmp(sb->signature, SUPERBLOCK_SIGNATURE, sizeof sb->signature))) {
		if (!silent)
			printk(KERN_ERR "spadfs: superblock not found\n");
		r = -EINVAL;
		goto err2;
	}
	if (SPAD2CPU64_LV(&sb->byte_sex) != 0x0123456789ABCDEFLL) {
		if (!silent)
			printk(KERN_ERR "spadfs: byte sex does not match: %16llx\n",
				(unsigned long long)SPAD2CPU64_LV(&sb->byte_sex));
		r = -EINVAL;
		goto err2;
	}
	if (unlikely(sb->version_major != SPADFS_VERSION_MAJOR)) {
		if (!silent)
			printk(KERN_ERR "spadfs: bad major version number (disk %d.%d.%d, driver %d.%d.%d)\n",
				sb->version_major,
				sb->version_middle,
				sb->version_minor,
				SPADFS_VERSION_MAJOR,
				SPADFS_VERSION_MIDDLE,
				SPADFS_VERSION_MINOR);
		r = -EINVAL;
		goto err2;
	}
	if (unlikely(SPAD2CPU32_LV(&sb->flags_compat_rw) & ~(0)) ||
	    unlikely(SPAD2CPU32_LV(&sb->flags_compat_ro) & ~(
				FLAG_COMPAT_RO_DIRECTORY_SIZES)) ||
	    unlikely(SPAD2CPU32_LV(&sb->flags_compat_none) & ~(
				FLAG_COMPAT_NONE_UNIX_NAMES |
				FLAG_COMPAT_NONE_DYNAMIC_MAGIC |
				FLAG_COMPAT_NONE_EXTRA_SPACE))) {
		if (!silent)
			printk(KERN_ERR "spadfs: incompatible feature flags in superblock: %08x/%08x/%08x/%08x\n",
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_fsck),
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_rw),
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_ro),
				(unsigned)SPAD2CPU32_LV(&sb->flags_compat_none));
		r = -EINVAL;
		goto err2;
	}
	if (unlikely(spadfs_meta_verify(sb))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"bad checksum on superblock: stored %08x computed %08x",
			spadfs_meta_stored_crc(sb), spadfs_meta_computed_crc(sb));
		r = -EFSERROR;
		goto err2;
	}
	if (unlikely((u64)(sector_t)SPAD2CPU64_LV(&sb->size) != SPAD2CPU64_LV(&sb->size))) {
		if (!silent)
			printk(KERN_ERR "spadfs: 48-bit filesystem not supported by vfs layer - enable it in kernel configuration\n");
		r = -EINVAL;
		goto err2;
	}
	if (unlikely((msg = validate_super(sb)) != NULL)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR, "invalid superblock: %s", msg);
		r = -EFSERROR;
		goto err2;
	}
	fs->sectors_per_disk_block_bits = sb->sectors_per_block_bits;
	if (!fs->buffer_size) {
		unsigned char wanted_sectors_per_buffer_bits = min(fs->sectors_per_disk_block_bits, (unsigned char)(PAGE_SHIFT - 9));
		if (unlikely(fs->sectors_per_buffer_bits != wanted_sectors_per_buffer_bits)) {
			if (unlikely(cycle)) {
				if (!silent)
					printk(KERN_ERR "spadfs: superblock changed while setting device size\n");
				r = -EIO;
				goto err2;
			}
			blocksize = 512U << wanted_sectors_per_buffer_bits;
			if (unlikely(!sb_set_blocksize(s, blocksize))) {
				if (!silent)
					printk(KERN_ERR "spadfs: can't set blocksize %d\n", blocksize);
				r = -EINVAL;
				goto err2;
			}
			cycle = 1;
			goto read_again;
		}
	} else {
		if (fs->sectors_per_disk_block_bits < fs->sectors_per_buffer_bits) {
			if (!silent)
				printk(KERN_ERR "spadfs: buffer size (%u) is greater than block size (%u)\n",
					512U << fs->sectors_per_buffer_bits, 512U << fs->sectors_per_disk_block_bits);
			r = -EINVAL;
			goto err2;
		}
	}

	fs->sectors_per_fnodepage_bits = sb->sectors_per_fnodepage_bits;
	fs->sectors_per_page_bits = sb->sectors_per_page_bits;
	fs->pagesize_bits = fs->sectors_per_page_bits + 9;
	fs->apage_size = 1U << (fs->pagesize_bits - 1);
	fs->dnode_hash_bits = fs->pagesize_bits - DNODE_PAGE_ENTRY_BITS - 1;
	fs->dnode_data_size = DNODE_PAGE_ENTRY_SIZE << fs->dnode_hash_bits;
	fs->dnode_page_sectors = spadfs_roundup_blocksize(fs,
			DNODE_ENTRY_OFFSET + (fs->dnode_data_size << 1));
	fs->dnode_page_sectors >>= 9;
	fs->sectors_per_cluster_bits = sb->sectors_per_cluster_bits;
	fs->cluster_threshold = (512U << fs->sectors_per_cluster_bits) *
				SPAD2CPU16_LV(&sb->cluster_threshold);
	fs->sectors_per_group_bits = sb->sectors_per_group_bits;
	if (fs->sectors_per_group_bits > sizeof(sector_t) * 8 - 1)
		fs->sectors_per_group_bits = sizeof(sector_t) * 8 - 1;
	fs->group_mask = ((sector_t)1 << fs->sectors_per_group_bits) - 1;
	fs->n_apage_mappings = fs->apage_size >> (fs->sectors_per_buffer_bits + 9);
	if (unlikely(!fs->n_apage_mappings))
		fs->n_apage_mappings = 1;

	fs->txb_sec = SPAD2CPU64_LV(&sb->txblock);
	fs->apage_index0_sec = SPAD2CPU64_LV(&sb->apage_index[0]);
	fs->apage_index1_sec = SPAD2CPU64_LV(&sb->apage_index[1]);
	fs->cct_sec = SPAD2CPU64_LV(&sb->cct);
	fs->cct_crc_sec = SPAD2CPU64_LV(&sb->cct_crc);
	/* validate_super() checked these: 0 = absent, else a real region */
	fs->cct_backup_sec = SPAD2CPU64_LV(&sb->cct_backup);
	fs->cct_backup_crc_sec = SPAD2CPU64_LV(&sb->cct_backup_crc);
	if (!fs->cct_backup_sec || !fs->cct_backup_crc_sec)
		fs->cct_backup_sec = fs->cct_backup_crc_sec = 0;
	fs->root_sec = SPAD2CPU64_LV(&sb->root);
	fs->size = SPAD2CPU64_LV(&sb->size);
	fs->reserve_sectors = SPAD2CPU64_LV(&sb->reserve_sectors);

	fs->flags_compat_fsck = SPAD2CPU32_LV(&sb->flags_compat_fsck);
	fs->flags_compat_rw = SPAD2CPU32_LV(&sb->flags_compat_rw);
	fs->flags_compat_ro = SPAD2CPU32_LV(&sb->flags_compat_ro);
	fs->flags_compat_none = SPAD2CPU32_LV(&sb->flags_compat_none);

	if (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS_OVERRIDE) {
		if (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS)
			fs->flags_compat_fsck &= ~FLAG_COMPAT_FSCK_NO_CHECKSUMS;
		else
			fs->flags_compat_fsck |= FLAG_COMPAT_FSCK_NO_CHECKSUMS;
	}

	if (!fs->xfer_size) {
		fs->xfer_size = fs->cluster_threshold;
		if (fs->xfer_size > SPADFS_DEFAULT_MAX_XFER_SIZE)
			fs->xfer_size = SPADFS_DEFAULT_MAX_XFER_SIZE;
	}
	for (i = 512; i < INT_MAX / 2; i <<= 1)
		if (i >= fs->xfer_size)
			break;
	fs->xfer_size = i;

	if (unlikely(IS_ERR(tb = spadfs_read_tx_block(fs, &bh,
						"spadfs_fill_super 1")))) {
		r = PTR_ERR(tb);
		goto err2;
	}
	fs->cc = SPAD2CPU16_LV(&tb->cc);
	fs->a_cc = tb->a_cc;  /* fs->a_cc and fs->a_txc are in disk-endianity */
	fs->a_txc = tb->a_txc;
	fs->txflags = SPAD2CPU32_LV(&tb->txflags);

	if (unlikely(fs->txflags & TXFLAGS_DIRTY))
		spadfs_error(fs, 0,
		     "filesystem was not cleanly unmounted");
	if (unlikely(fs->txflags & TXFLAGS_IO_READ_ERROR))
		spadfs_error(fs, 0,
		     "filesystem has encountered read i/o error");
	if (unlikely(fs->txflags & TXFLAGS_IO_WRITE_ERROR))
		spadfs_error(fs, 0,
		     "filesystem has encountered write i/o error");
	if (unlikely(fs->txflags & TXFLAGS_FS_ERROR))
		spadfs_error(fs, 0,
		     "filesystem has errors in structures");
	if (unlikely(fs->txflags & TXFLAGS_CHECKSUM_ERROR))
		spadfs_error(fs, 0,
		     "filesystem structures have bad checksums");
	if (unlikely(fs->txflags & TXFLAGS_EA_ERROR))
		spadfs_error(fs, 0,
		     "filesystem structures has errors in extended attributes");
	if (unlikely(fs->txflags & ~(
		TXFLAGS_DIRTY |
		TXFLAGS_IO_READ_ERROR |
		TXFLAGS_IO_WRITE_ERROR |
		TXFLAGS_FS_ERROR |
		TXFLAGS_CHECKSUM_ERROR |
		TXFLAGS_EA_ERROR)))
			spadfs_error(fs, 0,
				"filesystem has unknown error flags %08x",
				fs->txflags);
	if (unlikely(fs->txflags) && !sb_rdonly(s))
		spadfs_error(fs, 0, "running spadfsck is recommended");

	spadfs_brelse(fs, bh);

	if (unlikely(!(fs->cct = spadfs_alloc(1, CCT_SIZE, 0)))) {
		r = -ENOMEM;
		goto err3;
	}

	for (i = 0; i < CCT_SIZE / 512; i += 1U << fs->sectors_per_buffer_bits) {
		void *p = spadfs_read_sector(fs, fs->cct_sec + i, &bh,
				CCT_SIZE / 512 - i, "spadfs_fill_super 2");
		if (unlikely(IS_ERR(p))) {
			r = PTR_ERR(p);
			goto err3;
		}
		spadfs_memcpy((u8 *)fs->cct + (i << 9), p, 512U << fs->sectors_per_buffer_bits);
		spadfs_brelse(fs, bh);
	}
	fs->txc = SPAD2CPU32_LV(&fs->cct[fs->cc]);

	/*
	 * In-RAM mirror of the cct side CRCs (Phase 2.3, option A5).
	 *
	 * Seeded from the CRC units ON DISK, not computed from fs->cct. Every
	 * commit rewrites whole CRC blocks from this table (spadfs_write_cct_crc),
	 * and with 4K blocks one block holds all five units. Computing the table
	 * from the cct we just read therefore re-blessed every cct sector on the
	 * first commit: a rotted sector got a fresh matching CRC and fsck could
	 * no longer see it. Seeded from disk, a commit changes only the CRCs of
	 * the cct sectors it actually wrote; every other sector keeps the CRC
	 * fsck will check it against.
	 *
	 * Still never fatal. Mount-time verification was implemented and then
	 * rejected: the cct is written on every commit, so failing the mount on
	 * a mismatch would turn an ordinary crash between the cct write and the
	 * CRC write into an unmountable volume. A mismatch is only reported;
	 * fsck owns verification and repair.
	 *
	 * A unit whose own trailer fails (or cannot be read) carries no usable
	 * CRCs, the same rule fsck applies. Its sectors fall back to CRCs
	 * computed from fs->cct, as before, and the next commit rewrites it
	 * sealed.
	 *
	 * Residual gap: each commit rewrites the cct sector holding fs->cc from
	 * RAM, so rot inside that one sector is still re-blessed. Only a check
	 * at mount can catch that; the warning below is that check.
	 */
	if (unlikely(!(fs->cct_crc = spadfs_alloc(1, CCT_SECTORS * sizeof(__u32), 0)))) {
		r = -ENOMEM;
		goto err3;
	}
	{
		unsigned s, u, i;
		unsigned bad_units = 0, mismatched = 0;

		for (s = 0; s < CCT_SECTORS; s++)
			fs->cct_crc[s] = spadfs_crc32c_std((u8 *)fs->cct + (s << 9), 512);

		for (u = 0; fs->cct_crc_sec && u < CCT_CRC_UNITS; u++) {
			const __u8 *unit = spadfs_read_sector(fs,
					fs->cct_crc_sec + u, &bh,
					CCT_CRC_UNITS - u, "spadfs_fill_super 3");

			if (unlikely(IS_ERR(unit))) {
				bad_units++;
				continue;
			}
			if (unlikely(spadfs_meta_verify(unit))) {
				bad_units++;
				spadfs_brelse(fs, bh);
				continue;
			}
			for (i = 0; i < CCT_CRC_PER_UNIT; i++) {
				__u32 stored;

				s = u * CCT_CRC_PER_UNIT + i;
				if (s >= CCT_SECTORS)
					break;
				stored = spadfs_meta_get_crc(unit + i * 4);
				if (unlikely(stored != fs->cct_crc[s]))
					mismatched++;
				fs->cct_crc[s] = stored;
			}
			spadfs_brelse(fs, bh);
		}
		if (unlikely(bad_units | mismatched))
			printk(KERN_WARNING "spadfs: crash count table: %u of %u cct sector(s) do not match their CRC, %u of %u CRC unit(s) unusable; running spadfsck is recommended\n",
			       mismatched, (unsigned)CCT_SECTORS,
			       bad_units, (unsigned)CCT_CRC_UNITS);
		/*
		 * Only a cct that matched every stored CRC may be copied over
		 * the backup. Copying a damaged primary would destroy the one
		 * good copy fsck could restore from.
		 */
		fs->cct_verified = fs->cct_crc_sec && !bad_units && !mismatched;
	}

	fs->n_apages = N_APAGES(fs->size, fs->pagesize_bits, fs->sectors_per_disk_block_bits + 9);
	n = APAGE_INDEX_SECTORS(fs->n_apages, 512U << fs->sectors_per_disk_block_bits);

	fs->apage_index = spadfs_alloc(n, 512, sizeof(struct apage_index_entry));
	if (unlikely(!fs->apage_index)) {
		r = -ENOMEM;
		goto err3;
	}
	memset(fs->apage_index, 0, sizeof(struct apage_index_entry));
	fs->apage_index++;

	sec = CC_VALID(fs, &fs->a_cc, &fs->a_txc) ?
				fs->apage_index0_sec : fs->apage_index1_sec;
	for (i = 0; i < n;
	     i += 1U << fs->sectors_per_buffer_bits,
	     sec += 1U << fs->sectors_per_buffer_bits) {
		void *p = spadfs_read_sector(fs, sec, &bh, n - i, "spadfs_fill_super 3");
		if (unlikely(IS_ERR(p))) {
			r = PTR_ERR(p);
			goto err3;
		}
		spadfs_memcpy((u8 *)fs->apage_index + ((unsigned long)i << 9), p, 512U << fs->sectors_per_buffer_bits);
		spadfs_brelse(fs, bh);
	}

	fs->apage_info = spadfs_alloc(fs->n_apages, sizeof(struct apage_info), 0);
	if (unlikely(!fs->apage_info)) {
		r = -ENOMEM;
		goto err3;
	}
	for (i = 0; i < fs->n_apages; i++) {
		fs->apage_info[i].mapping[0].map = NULL;
		spadfs_cond_resched();
	}
	for (i = 0; i < fs->n_apages; i++)  {
		APAGE_MAP *m;
		if (unlikely(!(m = kmalloc(fs->n_apage_mappings * 2 * sizeof(APAGE_MAP), GFP_KERNEL)))) {
			r = -ENOMEM;
			goto err3;
		}
		fs->apage_info[i].mapping[0].map = m;
		fs->apage_info[i].mapping[1].map = m + fs->n_apage_mappings;
		memset(m, 0, fs->n_apage_mappings * 2 * sizeof(APAGE_MAP));
		spadfs_cond_resched();
	}

	fs->n_groups = (fs->size + (u64)fs->group_mask) >> fs->sectors_per_group_bits;
	fs->zones[0].grp_start = 0;
	fs->zones[0].grp_n = SPAD2CPU16_LV(&sb->small_file_group);
	fs->zones[1].grp_start = SPAD2CPU16_LV(&sb->small_file_group);
	fs->zones[1].grp_n = SPAD2CPU16_LV(&sb->large_file_group) -
					SPAD2CPU16_LV(&sb->small_file_group);
	fs->zones[2].grp_start = SPAD2CPU16_LV(&sb->large_file_group);
	fs->zones[2].grp_n = fs->n_groups -
					SPAD2CPU16_LV(&sb->large_file_group);

	fs->group_info = spadfs_alloc(fs->n_groups, sizeof(struct spadfs_group_info), 0);
	if (unlikely(!fs->group_info)) {
		r = -ENOMEM;
		goto err3;
	}
	memset(fs->group_info, 0, fs->n_groups * sizeof(struct spadfs_group_info));
	for (i = 0; i < fs->n_groups; i++) {
		if (i < fs->zones[1].grp_start)
			fs->group_info[i].zone = &fs->zones[0];
		else if (i < fs->zones[2].grp_start)
			fs->group_info[i].zone = &fs->zones[1];
		else
			fs->group_info[i].zone = &fs->zones[2];
	}

	fs->tmp_map = kmalloc(fs->n_apage_mappings * sizeof(APAGE_MAP),
			      GFP_KERNEL);
	if (unlikely(!fs->tmp_map)) {
		r = -ENOMEM;
		goto err3;
	}
	memset(fs->tmp_map, 0, fs->n_apage_mappings * sizeof(APAGE_MAP));
	for (i = 0; i < fs->n_apage_mappings; i++) {
		fs->tmp_map[i].entry = kmalloc(512U << fs->sectors_per_buffer_bits, GFP_KERNEL);
		if (unlikely(!fs->tmp_map[i].entry)) {
			r = -ENOMEM;
			goto err3;
		}
	}

	for (i = 0; i < fs->n_apages; i++)
		if (!SPAD2CPU64_LV(&fs->apage_index[i].end_sector))
			break;
	fs->n_active_apages = i;

	r = spadfs_count_free_space(fs);
	if (r)
		goto err3;

	if (!sb_rdonly(s)) {
		/* before the tx block moves on; see spadfs_cct_backup_start */
		spadfs_cct_backup_start(fs);
		if ((r = spadfs_increase_cc(fs, 1))) {
			if (r < 0)
				goto err3;
			s->s_flags |= SB_RDONLY;
		}
		if ((r = spadfs_start_new_tx(fs)))
			goto err4;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6,17,0)
	if (unlikely(!(fs->flags_compat_none & FLAG_COMPAT_NONE_UNIX_NAMES)))
		s->s_d_op = &spadfs_dops;
#else
	if (unlikely(!(fs->flags_compat_none & FLAG_COMPAT_NONE_UNIX_NAMES)))
		set_default_d_op(s, &spadfs_dops);
#endif
	root = spadfs_iget(s, make_fixed_spadfs_ino_t(fs->root_sec), 0, 0);
	if (unlikely(IS_ERR(root))) {
		r = PTR_ERR(root);
		goto err4;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
	s->s_root = d_alloc_root(root);
#else
	s->s_root = d_make_root(root);
#endif
	if (unlikely(!s->s_root)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
		iput(root);
#endif
		printk(KERN_ERR "spadfs: unable to allocate root dentry\n");
		r = -ENOMEM;
		goto err4;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
	spadfs_set_dentry_operations(fs, s->s_root);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	fs->spadfs_syncer = create_singlethread_workqueue(wq_name);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
	fs->spadfs_syncer = create_freezeable_workqueue(wq_name);
#else
	fs->spadfs_syncer = create_freezable_workqueue(wq_name);
#endif
	if (unlikely(!fs->spadfs_syncer)) {
		r = -ENOMEM;
		goto err4;
	}
#ifndef NEW_WORKQUEUE
	INIT_WORK(&fs->spadfs_sync_work, spadfs_sync_work, fs);
#else
	INIT_DELAYED_WORK(&fs->spadfs_sync_work, spadfs_sync_work);
#endif
	queue_delayed_work(fs->spadfs_syncer, &fs->spadfs_sync_work, fs->spadfs_sync_time);

	kfree(sb);

	return 0;

err4:
	if (!sb_rdonly(s))
		spadfs_increase_cc(fs, -1);
err3:
	spadfs_update_error_flags(fs);
err2:
	spadfs_free_super(s);
err1:
	kfree(sb);
err0:
	return r;
}

__cold static void spadfs_free_super(struct super_block *s)
{
	SPADFS *fs = spadfs(s);
	unsigned i;

	if (fs->spadfs_syncer) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
		cancel_rearming_delayed_workqueue(fs->spadfs_syncer,
						  &fs->spadfs_sync_work);
#else
		cancel_delayed_work_sync(&fs->spadfs_sync_work);
#endif
		destroy_workqueue(fs->spadfs_syncer);
	}

	spadfs_allocmem_done(fs);

	if (fs->apage_info) {
		spadfs_prune_cached_apage_buffers(fs);
		for (i = 0; i < fs->n_apages; i++) {
			kfree(fs->apage_info[i].mapping[0].map);
			spadfs_cond_resched();
		}
		kvfree(fs->apage_info);
	}
	spadfs_cond_resched();

	if (fs->tmp_map) {
		for (i = 0; i < fs->n_apage_mappings; i++)
			kfree(fs->tmp_map[i].entry);
		kfree(fs->tmp_map);
	}
	spadfs_cond_resched();

	kvfree(fs->group_info);
	spadfs_cond_resched();

	if (fs->apage_index)
		kvfree(fs->apage_index - 1);
	spadfs_cond_resched();

	kvfree(fs->cct);
	kvfree(fs->cct_crc);

	if (fs->inode_list) {
		for (i = 0; i < SPADFS_INODE_HASH_SIZE; i++)
			if (unlikely(!hlist_empty(&fs->inode_list[i])))
				panic("spadfs: spadfs_free_super: inodes leaked");
		kvfree(fs->inode_list);
	}

	done_sync_lock(fs);

	spadfs_buffer_leaks_done(fs);

	mutex_destroy(&fs->alloc_lock);
	mutex_destroy(&fs->inode_list_lock);
#ifdef SPADFS_QUOTA
	mutex_destroy(&fs->quota_alloc_lock);
#endif
	mutex_destroy(&fs->trim_lock);

	kvfree(fs);

	s->s_fs_info = NULL;
}

__cold static void spadfs_put_super(struct super_block *s)
{
	SPADFS *fs = spadfs(s);

#ifdef SPADFS_QUOTA
	dquot_disable(s, -1, DQUOT_USAGE_ENABLED | DQUOT_LIMITS_ENABLED);
#endif

	/*
	 * If it didn't commit due to I/O error, do not decrease crash count
	 * --- act as if crash happened
	 */
	if (!spadfs_commit(fs)) {
		if (fs->mount_flags & MOUNT_FLAGS_CLEAR_DIRTY_ON_UNMOUNT) {
			fs->txflags &= ~TXFLAGS_DIRTY;
			spin_lock_irq(&fs->txflags_new_lock);
			fs->txflags_new &= ~TXFLAGS_DIRTY;
			spin_unlock_irq(&fs->txflags_new_lock);
		}
		if (!sb_rdonly(s))
			spadfs_increase_cc(fs, -1);
	} else {
		spadfs_error(fs, TXFLAGS_IO_WRITE_ERROR,
			"i/o error at unmount - not commiting");
		spadfs_update_error_flags(fs);
	}
	/*{
		int i;
		printk("groups: ");
		for (i = 0; i < fs->n_groups; i++)
			printk(" %Ld", (long long)fs->group_info[i].freespace);
		printk("\nzones: %Ld %Ld %Ld\n",
			(long long)fs->zones[0].freespace,
			(long long)fs->zones[1].freespace,
			(long long)fs->zones[2].freespace);
	}*/
	spadfs_free_super(s);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
static int spadfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, spadfs_fill_super);
}

static void spadfs_free_fc(struct fs_context *fc)
{
	kfree(fc->fs_private);
}

static const struct constant_table spadfs_param_ino64[] = {
	{"no",		0},
	{"yes",		1},
	{"force",	2},
};

static const struct fs_parameter_spec spadfs_param_spec[] = {
	fsparam_flag	("help",		Opt_help),
	fsparam_u32	("uid",			Opt_uid),
	fsparam_u32	("gid",			Opt_gid),
	fsparam_u32oct	("umask",		Opt_umask),
	fsparam_u32	("prealloc_part",	Opt_prealloc_part),
	fsparam_u32	("prealloc_min",	Opt_prealloc_min),
	fsparam_u32	("prealloc_max",	Opt_prealloc_max),
	fsparam_u32	("xfer_size",		Opt_xfer_size),
	fsparam_u32	("buffer_size",		Opt_buffer_size),
	fsparam_u32	("prefetch",		Opt_prefetch),
	fsparam_u32	("sync_time",		Opt_sync_time),
	fsparam_flag	("no_checksums",	Opt_no_checksums),
	fsparam_flag	("checksums",		Opt_checksums),
	fsparam_enum	("ino64",		Opt_ino64,	spadfs_param_ino64),
#ifdef SPADFS_QUOTA
	fsparam_flag	("usrquota",		Opt_usrquota),
	fsparam_flag	("grpquota",		Opt_grpquota),
#endif
	{}
};

__cold static int spadfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct spadfs_fc_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, spadfs_param_spec, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
		case Opt_help:
			spadfs_help();
			return -EINVAL;
		case Opt_uid:
			if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
				if (ctx->uid != result.uint_32)
					return -EINVAL;
			} else {
				ctx->uid = result.uint_32;
			}
			break;
		case Opt_gid:
			if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
				if (ctx->gid != result.uint_32)
					return -EINVAL;
			} else {
				ctx->gid = result.uint_32;
			}
			break;
		case Opt_umask:
			if (result.uint_32 > 0777)
				return -EINVAL;
			if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
				if (ctx->umask != result.uint_32)
					return -EINVAL;
			} else {
				ctx->umask = result.uint_32;
			}
			break;
		case Opt_prealloc_part:
			if (!result.uint_32)
				return -EINVAL;
			ctx->prealloc_part = result.uint_32;
			break;
		case Opt_prealloc_min:
			ctx->prealloc_min = result.uint_32;
			break;
		case Opt_prealloc_max:
			ctx->prealloc_max = result.uint_32;
			break;
		case Opt_xfer_size:
			if (!result.uint_32)
				return -EINVAL;
			ctx->xfer_size = result.uint_32;
			break;
		case Opt_buffer_size:
			if (result.uint_32 < 512 ||
			    result.uint_32 > PAGE_SIZE ||
			    result.uint_32 & (result.uint_32 - 1))
				return -EINVAL;
			if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
				if (ctx->buffer_size != result.uint_32)
					return -EINVAL;
			} else {
				ctx->buffer_size = result.uint_32;
			}
			break;
		case Opt_prefetch:
			ctx->prefetch = result.uint_32 >> 9;
			if (ctx->prefetch & (ctx->prefetch - 1))
				ctx->prefetch = 1U << (fls(ctx->prefetch) - 1);
			break;
		case Opt_sync_time:
			if (!result.uint_32 ||
			    result.uint_32 >= (1UL << (BITS_PER_LONG - 1)) / HZ)
				return -EINVAL;
			ctx->sync_time = (unsigned long)result.uint_32 * HZ;
			break;
		case Opt_no_checksums:
			ctx->no_checksums = true;
			ctx->checksums = false;
			break;
		case Opt_checksums:
			ctx->no_checksums = false;
			ctx->checksums = true;
			break;
		case Opt_ino64:
			ctx->ino64 = result.uint_32;
			break;
#ifdef SPADFS_QUOTA
		case Opt_usrquota:
			ctx->usrquota = true;
			break;
		case Opt_grpquota:
			ctx->grpquota = true;
			break;
#endif
		default:
			return -EINVAL;
	}

	return 0;
}

static const struct fs_context_operations spadfs_fc_context_ops = {
	.parse_param	= spadfs_parse_param,
	.get_tree	= spadfs_get_tree,
	.reconfigure	= spadfs_reconfigure,
	.free           = spadfs_free_fc,
};

__cold static int spadfs_init_fs_context(struct fs_context *fc)
{
	struct spadfs_fc_context *ctx;

	ctx = kzalloc(sizeof(struct spadfs_fc_context), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
		struct super_block *s = fc->root->d_sb;
		SPADFS *fs = spadfs(s);
		ctx->uid = fs->uid;
		ctx->gid = fs->gid;
		ctx->umask = ~fs->mode & 0777;
		ctx->prealloc_part = fs->prealloc_part;
		ctx->prealloc_min = fs->min_prealloc;
		ctx->prealloc_max = fs->max_prealloc;
		ctx->xfer_size = fs->xfer_size;
		ctx->buffer_size = 512U << fs->sectors_per_buffer_bits;
		ctx->prefetch = fs->metadata_prefetch;
		ctx->sync_time = fs->spadfs_sync_time;
		ctx->no_checksums = (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS_OVERRIDE) && !(fs->mount_flags & MOUNT_FLAGS_CHECKSUMS);
		ctx->checksums = (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS_OVERRIDE) && (fs->mount_flags & MOUNT_FLAGS_CHECKSUMS);
		if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO))
			ctx->ino64 = 0;
		else if (!(fs->mount_flags & MOUNT_FLAGS_64BIT_INO_FORCE))
			ctx->ino64 = 1;
		else
			ctx->ino64 = 2;
#ifdef SPADFS_QUOTA
		ctx->usrquota = !!(fs->mount_flags & MOUNT_FLAGS_USRQUOTA);
		ctx->grpquota = !!(fs->mount_flags & MOUNT_FLAGS_GRPQUOTA);
#endif
	} else {
		ctx->uid = get_current_uid();
		ctx->gid = get_current_gid();
		ctx->umask = current_umask();
		ctx->prealloc_part = SPADFS_DEFAULT_PREALLOC_PART;
		ctx->prealloc_min = SPADFS_DEFAULT_MIN_PREALLOC;
		ctx->prealloc_max = SPADFS_DEFAULT_MAX_PREALLOC;
		ctx->xfer_size = 0;
		ctx->buffer_size = 0;
		ctx->prefetch = SPADFS_DEFAULT_METADATA_PREFETCH >> 9;
		ctx->sync_time = SPADFS_DEFAULT_SYNC_TIME;
		ctx->no_checksums = false;
		ctx->checksums = false;
		ctx->ino64 = 0;
#ifdef SPADFS_QUOTA
		ctx->usrquota = false;
		ctx->grpquota = false;
#endif
	}

	fc->fs_private = ctx;
	fc->ops = &spadfs_fc_context_ops;

	return 0;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
__cold static struct dentry *spadfs_mount(struct file_system_type *fs_type, int flags,
				   const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, spadfs_fill_super);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
__cold static int spadfs_get_sb(struct file_system_type *fs_type, int flags,
			 const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, spadfs_fill_super,
			   mnt);
}
#else
__cold static struct super_block *spadfs_get_sb(struct file_system_type *fs_type,
					 int flags, const char *dev_name,
					 void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, spadfs_fill_super);
}
#endif

static
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
const
#endif
struct super_operations spadfs_sops = {
	.alloc_inode = spadfs_alloc_inode,
	.destroy_inode = spadfs_destroy_inode,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	.delete_inode = spadfs_delete_inode,
#else
	.evict_inode = spadfs_evict_inode,
#endif
	.put_super = spadfs_put_super,
	.sync_fs = spadfs_sync_fs,
	.statfs = spadfs_statfs,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0)
	.remount_fs = spadfs_remount_fs,
#endif
	.show_options = spadfs_show_options,
#ifdef SPADFS_QUOTA
	.quota_read = spadfs_quota_read,
	.quota_write = spadfs_quota_write,
#if SPADFS_QUOTA >= 2
	.get_dquots = spadfs_quota_get,
#endif
#endif
};

static struct file_system_type spadfs_fs_type = {
	.owner = THIS_MODULE,
	.name = __stringify(SPADFS_NAME),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
	.init_fs_context = spadfs_init_fs_context,
	.parameters = spadfs_param_spec,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
	.mount = spadfs_mount,
#else
	.get_sb = spadfs_get_sb,
#endif
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV
#if TEST_RHEL_VERSION(5,3)
		| FS_HAS_FALLOCATE
#endif
#if TEST_RHEL_VERSION(5,4)
		| FS_HAS_FIEMAP
#endif
};

#ifdef CONFIG_SMP
unsigned spadfs_nr_cpus;
int spadfs_unlocked_extent_cache;
unsigned spadfs_extent_cache_size;
#endif

__cold static void spadfs_free_caches(void)
{
	rcu_barrier();
	spadfs_free_cache(spadfs_inode_cachep);
	spadfs_free_cache(spadfs_ea_cachep);
	spadfs_free_cache(spadfs_extent_cachep);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
static int minimize_inode_size = 0;
#else
static bool minimize_inode_size = 0;
#endif
module_param(minimize_inode_size, bool, S_IRUGO);

__cold static int __init spadfs_init(void)
{
	int r;

	if (!spadfs_struct_check_correctness()) {
		printk(KERN_ERR "SPADFS: BAD STRUCTURE ALIGNMENT, THE PROGRAM WAS COMPILED WITH WRONG ABI\n");
		r = -EINVAL;
		goto ret_r;
	}

#ifdef CONFIG_SMP
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	{
		unsigned i;
		spadfs_nr_cpus = 1;
		for (i = 0; i < nr_cpumask_bits; i++)
			if (cpu_possible(i))
				spadfs_nr_cpus = i + 1;
	}
#else
	spadfs_nr_cpus = cpumask_last(cpu_possible_mask) + 1;
#endif

	spadfs_unlocked_extent_cache = sizeof(SPADFNODE) + spadfs_nr_cpus * sizeof(struct extent_cache) <= PAGE_SIZE;
	if (spadfs_nr_cpus > 1 && minimize_inode_size)
		spadfs_unlocked_extent_cache = 0;
	spadfs_extent_cache_size = spadfs_unlocked_extent_cache ? spadfs_nr_cpus : 1;
#endif

	spadfs_inode_cachep = kmem_cache_create(__stringify(SPADFS_NAME) "_inode_cache",
						sizeof(SPADFNODE) + spadfs_extent_cache_size * sizeof(struct extent_cache),
						FNODE_EA_ALIGN, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
						fnode_ctor
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
						, NULL
#endif
	);
	if (unlikely(!spadfs_inode_cachep)) {
		r = -ENOMEM;
		goto ret_r;
	}

	spadfs_ea_cachep = kmem_cache_create(__stringify(SPADFS_NAME) "_ea_cache",
					     FNODE_MAX_EA_SIZE,
					     FNODE_EA_ALIGN, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
					     NULL
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
					     , NULL
#endif
	);
	if (unlikely(!spadfs_ea_cachep)) {
		r = -ENOMEM;
		goto ret_r;
	}

	spadfs_extent_cachep = kmem_cache_create(__stringify(SPADFS_NAME) "_extent_cache",
						 sizeof(struct allocmem),
						 0, SLAB_MEM_SPREAD,
						 NULL
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
						 , NULL
#endif
	);
	if (unlikely(!spadfs_extent_cachep)) {
		r = -ENOMEM;
		goto ret_r;
	}

	r = register_filesystem(&spadfs_fs_type);
	if (unlikely(r))
		goto ret_r;

	return 0;

ret_r:
	spadfs_free_caches();
	return r;
}

__cold static void __exit spadfs_exit(void)
{
	unregister_filesystem(&spadfs_fs_type);
	spadfs_free_caches();
}

module_init(spadfs_init)
module_exit(spadfs_exit)
MODULE_LICENSE("GPL");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
MODULE_ALIAS_FS(__stringify(SPADFS_NAME));
#endif

#if __GNUC__ <= 3 && BITS_PER_LONG == 32
/*
 * GCC 2.95.* has a bug --- it generates ".globl __udivdi3" into assembler
 * although it doesn't ever call this function. This causes module linking
 * problems.
 *
 * GCC 3.3.6 does it too. I have no idea why.
 */
void __udivdi3(void)
{
	printk(KERN_EMERG "__udivdi3 called!\n");
	BUG();
}
#endif
