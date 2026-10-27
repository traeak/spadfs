#include "spadfs.h"

/*
 * Read various filesystem structures, and check for magic and checksum
 * TODO:
 * check checksum only when reading from disk.
 *	      use some flag that is reset when reading buffer from disk (which?)
 */

struct txblock *spadfs_read_tx_block(SPADFS *fs, struct buffer_head **bhp,
				     const char *msg)
{
	struct txblock *tb = spadfs_read_sector(fs, fs->txb_sec, bhp, 0, msg);
	if (unlikely(IS_ERR(tb))) return tb;
	if (unlikely(SPAD2CPU32_LV(&tb->magic) !=
		     spadfs_magic(fs, fs->txb_sec, TXBLOCK_MAGIC))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"bad magic on tx block %Lx at %s",
			(unsigned long long)fs->txb_sec,
			msg);
rel_err:
		spadfs_brelse(fs, *bhp);
		return ERR_PTR(-EFSERROR);
	}
	if (unlikely(check_checksums(fs))) {
		if (unlikely(spadfs_meta_verify(tb))) {
			spadfs_error(fs, TXFLAGS_CHECKSUM_ERROR,
				"bad checksum on tx block %Lx at %s: stored %08x computed %08x",
				(unsigned long long)fs->txb_sec,
				msg,
				spadfs_meta_stored_crc(tb),
				spadfs_meta_computed_crc(tb));
			goto rel_err;
		}
	}
	return tb;
}

/*
 * struct_type means what can we accept:
 *	SRFB_FNODE : fnode_block
 *	SRFB_DNODE : dnode
 *	SRFB_FIXED_FNODE : fixed fnode block
 * if more are set, the type of returned structure must be determined by
 * the caller from magic.
 */

struct fnode_block *spadfs_read_fnode_block(SPADFS *fs, sector_t secno,
					    struct buffer_head **bhp,
					    int struct_type, const char *msg)
{
	struct fnode_block *fnode_block = spadfs_read_sector(fs, secno, bhp,
						fs->metadata_prefetch , msg);
	if (unlikely(IS_ERR(fnode_block)))
		return fnode_block;

	if (likely(SPAD2CPU32_LV(&fnode_block->magic) ==
		   spadfs_magic(fs, secno, FNODE_BLOCK_MAGIC))) {
		if (unlikely(!(struct_type & SRFB_FNODE)))
			goto bad_magic;
		/* Regular fnode chain: legacy XOR fold (see the fixed-fnode
		 * branch below for why the trailer cannot be used here). */
		if (unlikely(check_checksums(fs)) &&
		    likely(fnode_block->flags & FNODE_BLOCK_CHECKSUM_VALID)) {
			start_concurrent_atomic_buffer_read(fs, *bhp);
			if (likely(fnode_block->flags &
				   FNODE_BLOCK_CHECKSUM_VALID)) {
				if (unlikely(__byte_sum(fnode_block, 512) !=
					     CHECKSUM_BASE)) {
					end_concurrent_atomic_buffer_read(fs,
									  *bhp);
					spadfs_error(fs, TXFLAGS_CHECKSUM_ERROR,
						"bad checksum on fnode block %Lx at %s",
						(unsigned long long)secno,
						msg);
					goto rel_err;
				}
			}
			end_concurrent_atomic_buffer_read(fs, *bhp);
		}
		return fnode_block;
	}
	if (likely(SPAD2CPU32_LV(&fnode_block->magic) ==
		   spadfs_magic(fs, secno, DNODE_PAGE_MAGIC))) {
		if (unlikely(!(struct_type & SRFB_DNODE)))
			goto bad_magic;
		if (unlikely((((struct dnode_page *)fnode_block)->gflags &
			     DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1) >>
			     DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT !=
			     fs->sectors_per_page_bits - 1)) {
			spadfs_error(fs, TXFLAGS_FS_ERROR,
				"dnode %Lx has invalid page size at %s",
				(unsigned long long)secno,
				msg);
			goto rel_err;
		}
		return fnode_block;
	}
	if (likely(SPAD2CPU32_LV(&fnode_block->magic) ==
		   spadfs_magic(fs, secno, FIXED_FNODE_BLOCK_MAGIC))) {
		if (unlikely(!(struct_type & SRFB_FIXED_FNODE)))
			goto bad_magic;
		/*
		 * Phase 2.2 Cat 2 gave fixed_fnode_block the CRC trailer and
		 * zeroed the legacy __byte_sum field, but this path still fell
		 * through to check_fn, which tests the XOR. Measured on a
		 * freshly formatted volume:
		 *
		 *   __byte_sum(block,512) = 0x96   CHECKSUM_BASE = 0x43  -> kernel rejects
		 *   trailer stored        = 0x88b78f11
		 *   trailer computed      = 0x88b78f11                    -> fsck accepts
		 *
		 * so every mount failed with EUCLEAN. Verify the trailer here,
		 * matching the anode path above.
		 */
		if (unlikely(check_checksums(fs)) &&
		    likely(fnode_block->flags & FNODE_BLOCK_CHECKSUM_VALID)) {
			start_concurrent_atomic_buffer_read(fs, *bhp);
			if (likely(fnode_block->flags &
				   FNODE_BLOCK_CHECKSUM_VALID)) {
				if (unlikely(spadfs_meta_verify(fnode_block))) {
					end_concurrent_atomic_buffer_read(fs,
									  *bhp);
					spadfs_error(fs, TXFLAGS_CHECKSUM_ERROR,
						"bad checksum on fixed fnode block %Lx at %s: stored %08x computed %08x",
						(unsigned long long)secno, msg,
						spadfs_meta_stored_crc(fnode_block),
						spadfs_meta_computed_crc(fnode_block));
					goto rel_err;
				}
			}
			end_concurrent_atomic_buffer_read(fs, *bhp);
		}
		return fnode_block;
	}
bad_magic:
	spadfs_error(fs, TXFLAGS_FS_ERROR,
		"bad magic %08x on fnode block %Lx at %s",
		SPAD2CPU32(fnode_block->magic),
		(unsigned long long)secno,
		msg);
rel_err:
	spadfs_brelse(fs, *bhp);
	return ERR_PTR(-EFSERROR);
}

struct anode *spadfs_read_anode(SPADFS *fs, sector_t secno,
				struct buffer_head **bhp, unsigned *vx,
				int read_lock, const char *msg)
{
	struct anode *anode = spadfs_read_sector(fs, secno, bhp,
					1 << fs->sectors_per_page_bits, msg);
	if (unlikely(IS_ERR(anode)))
		return anode;

	if (read_lock)
		start_concurrent_atomic_buffer_read(fs, *bhp);

	if (unlikely(SPAD2CPU32_LV(&anode->magic) !=
		     spadfs_magic(fs, secno, ANODE_MAGIC))) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"bad magic on anode %Lx at %s",
			(unsigned long long)secno,
			msg);
		goto rel_err;
	}
	if (unlikely(check_checksums(fs)) &&
	    likely(anode->flags & ANODE_CHECKSUM_VALID)) {
		if (unlikely(spadfs_meta_verify(anode))) {
			spadfs_error(fs, TXFLAGS_CHECKSUM_ERROR,
				"bad checksum on anode %Lx at %s: stored %08x computed %08x",
				(unsigned long long)secno,
				msg,
				spadfs_meta_stored_crc(anode),
				spadfs_meta_computed_crc(anode));
			goto rel_err;
		}
	}
	*vx = anode->valid_extents;
	if (unlikely((unsigned)(*vx - 1) >= ANODE_N_EXTENTS)) {
		spadfs_error(fs, TXFLAGS_FS_ERROR,
			"anode %Lx has %u valid extents at %s",
			(unsigned long long)secno,
			(unsigned)anode->valid_extents,
			msg);
		goto rel_err;
	}
	return anode;

rel_err:
	if (read_lock)
		end_concurrent_atomic_buffer_read(fs, *bhp);

	spadfs_brelse(fs, *bhp);
	return ERR_PTR(-EFSERROR);
}

