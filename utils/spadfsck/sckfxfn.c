#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

struct fixed_fnode_block *update_fixed_fnode_block(struct fixed_fnode_block *fx, blk_t blk)
{
	fx->flags |= FIXED_FNODE_BLOCK_CHECKSUM_VALID;
	fx->checksum ^= CHECKSUM_BASE ^ __byte_sum(fx, FIXED_FNODE_BLOCK_SIZE);
	if (__unlikely(write_buffer(blk, 1, fx))) return NULL;
	return reread_fixed_fnode_block(blk);
}

struct fixed_fnode_block *reread_fixed_fnode_block(blk_t blk)
{
	struct fixed_fnode_block *fx = read_buffer(blk, 1, 0);
	if (__unlikely(!fx)) return NULL;
	if (__unlikely(SPAD2CPU32_LV(&fx->magic) != magic(blk, FIXED_FNODE_BLOCK_MAGIC))) {
		_snprintf(buffer_error, sizeof buffer_error, "BAD MAGIC WHEN RE-READING FIXED FNODE BLOCK %"blk_format"X: %08X", blk, (unsigned)SPAD2CPU32_LV(&fx->magic));
		return NULL;
	}
	return fx;
}

struct fnode *fixed_fnode_block_fnode(struct fixed_fnode_block *fx, int complementary)
{
	if (cc_valid(fx->cc, fx->txc) ^ complementary) return (struct fnode *)&fx->fnode0;
	else return (struct fnode *)&fx->fnode1;
}

static int quick_check_fnode(struct fnode *fnode, int directory)
{
	unsigned ea_size;
	if (__unlikely(fnode->flags & FNODE_FLAGS_HARDLINK)) return 1;
	if (__unlikely(!(fnode->flags & FNODE_FLAGS_DIR) != !directory)) return 1;
	if (__unlikely(fnode->namelen)) return 1;
	ea_size = (SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) - FNODE_EA_POS(0);
	if (__unlikely(ea_size > FNODE_MAX_EA_SIZE)) return 1;
	return 0;
}

/* 0 --- ok, 1 --- error, 2 --- allocation error, -1 cross linked, need another pass */

int read_fixed_fnode(blk_t blk, int directory, struct fixed_fnode_block **result_fx, struct fnode **result_fnode, const char *(*get_filename)(void *p), void *p)
{
	int r;
	int complementary_used = 0;
	unsigned ea_size;
	struct fixed_fnode_block *fx;
	struct fnode *fnode;
	if (__unlikely(!ptr_valid(blk, 1))) {
		log_printf(0, "%s: BAD FIXED FNODE BLOCK %"blk_format"X: %s", get_filename(p), blk, ptr_error);
		return 1;
	}
	fx = read_buffer(blk, 1, 0);
	if (__unlikely(!fx)) {
		log_printf(0, "%s: ERROR READING FIXED FNODE BLOCK: %s", get_filename(p), buffer_error);
		return 1;
	}
	if (__unlikely(SPAD2CPU32_LV(&fx->magic) != magic(blk, FIXED_FNODE_BLOCK_MAGIC))) {
		log_printf(0, "%s: BAD MAGIC ON FIXED FNODE BLOCK %"blk_format"X: %08X", get_filename(p), blk, (unsigned)SPAD2CPU32_LV(&fx->magic));
		return 1;
	}

	r = sys_alloc(blk, 1 << super.sectors_per_block_bits, 1, "FIXED FNODE BLOCK", NULL, get_filename, p);
	if (__unlikely(r)) return r;

	{
		blk_t end;
		if (__unlikely(r = get_blockrun(blk, 1, &end))) return r;
		if (end < blk + (1 << super.sectors_per_block_bits)) return 1;
	}

	if (fx->flags & FIXED_FNODE_BLOCK_CHECKSUM_VALID && __unlikely(__byte_sum(fx, FIXED_FNODE_BLOCK_SIZE) != CHECKSUM_BASE)) {
		log_printf(0, "%s: BAD CHECKSUM ON FIXED FNODE BLOCK %"blk_format"X: %02X", get_filename(p), blk, __byte_sum(fx, FIXED_FNODE_BLOCK_SIZE) ^ CHECKSUM_BASE);
		if (query(qprefix 0, "FIX?")) {
			if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) {
				fix_err:
				log_printf(1, "%s: ERROR UPDATING FIXED FNODE BLOCK %"blk_format"X: %s", get_filename(p), blk, buffer_error);
				log_printf(1, "THE DEVICE IS UNRELIABLE");
				free_ret_1:
				if (__unlikely(r = sys_free(blk, 1 << super.sectors_per_block_bits, 1))) return r;
				return 1;
			}
		}
	}
	if (__unlikely(cc_check(&fx->cc, &fx->txc, "FIXED FNODE BLOCK", get_filename, p, blk))) {
		if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) goto fix_err;
	}
	test_again:
	fnode = fixed_fnode_block_fnode(fx, 0);
	if (__unlikely(fnode->flags & FNODE_FLAGS_HARDLINK)) {
		log_printf(0, "%s: FIXED FNODE BLOCK %"blk_format"X IS HARDLINK, FLAGS %02X", get_filename(p), blk, fnode->flags);
		test_complementary:
		if (!complementary_used && !quick_check_fnode(fixed_fnode_block_fnode(fx, 1), directory)) {
			log_printf(0, "%s: HOWEVER, COMPLEMENTARY FNODE SEEMS OK", get_filename(p));
			if (query(qprefix Q_NOFIX, "USE IT?")) {
				use_complementary:
				CPU2SPAD16_LV(&fx->cc, 0);
				CPU2SPAD32_LV(&fx->txc, (__u32)cc_valid(fx->cc, fx->txc) << 31);
				if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) goto fix_err;
				complementary_used = 1;
				goto test_again;
			}
		}
		goto free_ret_1;
	}
	if (__unlikely(!(fnode->flags & FNODE_FLAGS_DIR) != !directory)) {
		log_printf(0, "%s: FIXED FNODE BLOCK %"blk_format"X IS %sDIRECTORY, FLAGS %02X", get_filename(p), blk, directory ? "NOT " : "", fnode->flags);
		goto test_complementary;
	}
	if (__unlikely(fnode->namelen)) {
		log_printf(0, "%s: FIXED FNODE BLOCK %"blk_format"X HAS NON-EMPTY NAME, LENGTH %02X", get_filename(p), blk, fnode->namelen);
		if (!complementary_used && !quick_check_fnode(fixed_fnode_block_fnode(fx, 1), directory)) {
			log_printf(0, "%s: HOWEVER, COMPLEMENTARY FNODE SEEMS OK", get_filename(p));
			if (query(qprefix Q_NOFIX, "USE IT?")) goto use_complementary;
		}
		if (query(qprefix 0, "FIX?")) {
			fnode->namelen = 0;
			CPU2SPAD16_LV(&fnode->next, FNODE_SIZE(0, 0));
			if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) goto fix_err;
			fnode = fixed_fnode_block_fnode(fx, 0);
		} else {
			goto free_ret_1;
		}
	}
	if (directory) {
		if (__unlikely(SPAD2CPU64_LV(&*FIXED_FNODE_NLINK_PTR(fnode)) != 1)) {
			log_printf(0, "%s: NLINK ISN'T 1", get_filename(p));
			if (query(qprefix 0, "FIX?")) {
				CPU2SPAD64_LV(&*FIXED_FNODE_NLINK_PTR(fnode), 1);
				if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) goto fix_err;
				fnode = fixed_fnode_block_fnode(fx, 0);
			}
		}
	}
	ea_size = (SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) - FNODE_EA_POS(0);
	if (__unlikely(ea_size > FNODE_MAX_EA_SIZE)) {
		log_printf(0, "%s: INVALID EXTENDED ATTRIBUTES ON FIXED FNODE BLOCK %"blk_format"X", get_filename(p), blk);
		if (!complementary_used && !quick_check_fnode(fixed_fnode_block_fnode(fx, 1), directory)) {
			log_printf(0, "%s: HOWEVER, COMPLEMENTARY FNODE SEEMS OK", get_filename(p));
			if (query(qprefix Q_NOFIX, "USE IT?")) goto use_complementary;
		}
		if (query(qprefix 0, "TRUNCATE?")) {
			CPU2SPAD16_LV(&fnode->next, FNODE_SIZE(0, 0));
			if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) goto fix_err;
			fnode = fixed_fnode_block_fnode(fx, 0);
		} else {
			goto free_ret_1;
		}
	}
	if (__unlikely(cc_check(&fnode->cc, &fnode->txc, "FNODE IN FIXED FNODE BLOCK", get_filename, p, blk))) {
		if (__unlikely(!(fx = update_fixed_fnode_block(fx, blk)))) goto fix_err;
		fnode = fixed_fnode_block_fnode(fx, 0);
	}
	*result_fx = fx;
	*result_fnode = fnode;
	return 0;
}
