#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

static char *fblk_error;

static struct fnode_block *read_fblk(blk_t blk)
{
	void *f;
	if (__unlikely(!ptr_valid(blk, 1 << super.sectors_per_block_bits))) {
		fblk_error = ptr_error;
		return NULL;
	}
	f = read_buffer(blk, 1 << super.sectors_per_block_bits, 0);
	if (!f) fblk_error = buffer_error;
	return f;
}

static int scan_fnode_blocks_run(blk_t blk, blk_t prev, const char *(*get_filename)(void *p), void *p, unsigned *len, int *dno, int *next_valid, blk_t *next)
{
	blk_t alloc_start = blk;
	unsigned alloc_len = 0;
	int err_recurse = 0;
	int first, last;
	unsigned c;
	struct fnode_block *f;
	int r;
	*len = 1 << super.sectors_per_block_bits;
	*next_valid = 0;
	*dno = 0;
#define ff(x)	((struct fnode_block *)((char *)f + ((x) * FNODE_BLOCK_SIZE)))
	if (__unlikely(!(f = read_fblk(blk)))) {
		read_err:
		log_printf(0, "%s: ERROR %s FNODE BLOCK: %s", get_filename(p), alloc_len ? "FIXING" : "READING", fblk_error);
		if (alloc_len > 0 && !err_recurse) {
			err_recurse = 1;
			alloc_len -= 1 << super.sectors_per_block_bits;
			if (__unlikely(r = sys_free(alloc_start + alloc_len, 1 << super.sectors_per_block_bits, 1))) return r;
			goto truncq;
		}
		sys_free_ret_1:
		if (__unlikely(r = sys_free(alloc_start, alloc_len, 1))) return r;
		return 1;
	}
	if (__unlikely(SPAD2CPU32_LV(&f->magic) != magic(blk, FNODE_BLOCK_MAGIC))) {
		if (__likely(SPAD2CPU32_LV(&f->magic) == magic(blk, DNODE_PAGE_MAGIC))) {
			if (__unlikely(prev != 0)) {
				log_printf(0, "%s: DNODE IN THE MIDDLE OF FNODE BLOCK CHAIN AT %"blk_format"X FROM %"blk_format"X", get_filename(p), blk, prev);
				return 1;
			}
			if (__unlikely(!ptr_valid(blk, dnode_page_sectors))) {
				log_printf(0, "%s: INVALID DNODE PLACEMENT AT %"blk_format"X: %s", get_filename(p), blk, ptr_error);
				return 1;
			}
			prefetch_dnode(blk);
			r = sys_alloc(blk, dnode_page_sectors, 1, "DNODE PAGE", NULL, get_filename, p);
			if (__unlikely(r)) return r;
			*dno = 1;
			*len = dnode_page_sectors;
			return 0;
		}
		log_printf(0, "%s: BAD MAGIC ON FNODE BLOCK %"blk_format"X: %08X", get_filename(p), blk, (unsigned)SPAD2CPU32_LV(&f->magic));
		return 1;
	}
	if (__unlikely(!(f->flags & FNODE_BLOCK_FIRST))) {
		log_printf(0, "%s: FNODE BLOCK %"blk_format"X DOES NOT HAVE FIRST BLOCK FLAG (FLAGS %02X)", get_filename(p), blk, f->flags);
		return 1;
	}
	if (__unlikely(MAKE_D_OFF(f->prev0, f->prev1) != prev)) {
		if (!prev) log_printf(0, "%s: FNODE BLOCK %"blk_format"X IS PART OF CHAIN, BACK POINTER %"blk_format"X", get_filename(p), blk, MAKE_D_OFF(f->prev0, f->prev1));
		else log_printf(0, "%s: FNODE BLOCK %"blk_format"X HAS MISMATCHING PREV POINTER (%"blk_format"X != %"blk_format"X)", get_filename(p), blk, MAKE_D_OFF(f->prev0, f->prev1), prev);
		return 1;
	}
	r = sys_alloc(blk, 1 << super.sectors_per_block_bits, 1, "FNODE BLOCK", NULL, get_filename, p);
	if (__unlikely(r)) return r;
	alloc_len += 1 << super.sectors_per_block_bits;
	first = 1;
	next_block:
	last = 0;
	c = 0;

#define update_block(c)							\
do {									\
	ff(c)->flags |= FNODE_BLOCK_CHECKSUM_VALID;			\
	ff(c)->checksum ^= CHECKSUM_BASE ^ __byte_sum(ff(c), FNODE_BLOCK_SIZE);\
	if (__unlikely(write_buffer(blk + c, 1, ff(c))) ||		\
	    __unlikely(!(f = read_fblk(blk)))) {			\
		goto read_err;						\
	}								\
} while (0)

	while (c < 1 << super.sectors_per_block_bits) {
		if (__unlikely(SPAD2CPU32_LV(&ff(c)->magic) != magic(blk + c, FNODE_BLOCK_MAGIC))) {
			log_printf(0, "%s: BAD MAGIC ON FNODE BLOCK %"blk_format"X: %08X", get_filename(p), blk + c, (unsigned)SPAD2CPU32_LV(&ff(c)->magic));
			if (query(qprefix 0, "FIX?")) {
				memset(ff(c), 0, FNODE_BLOCK_SIZE);
				CPU2SPAD32_LV(&ff(c)->magic, magic(blk + c, FNODE_BLOCK_MAGIC));
				CPU2SPAD32_LV(&ff(c)->txc, 0x80000000U);
				CPU2SPAD16_LV(&ff(c)->fnodes[1].next, (FNODE_MAX_SIZE & FNODE_NEXT_SIZE) | FNODE_NEXT_FREE);
				update_block(c);
			} else {
				goto sys_free_ret_1;
			}
		}
		/*
		  checked in check_fnode_block ... this allows delayed recovery
		  of files from damaged fnodes
		if (ff(c)->flags & FNODE_BLOCK_CHECKSUM_VALID && __unlikely(__byte_sum(ff(c), FNODE_BLOCK_SIZE) != CHECKSUM_BASE)) {
			log_printf(0, "%s: BAD CHECKSUM ON FNODE BLOCK %"blk_format"X: %02X", get_filename(p), blk + c, __byte_sum(ff(c), FNODE_BLOCK_SIZE) ^ CHECKSUM_BASE);
			if (query(qprefix 0, "FIX?")) {
				update_block(c);
			}
		}
		*/
		prefetch_fnode_block(ff(c));
		if (!first) {
			if (__unlikely(ff(c)->flags & FNODE_BLOCK_FIRST)) {
				log_printf(0, "%s: INNER FNODE BLOCK %"blk_format"X HAS FIRST BLOCK FLAG (FLAGS %02X)", get_filename(p), blk + c, ff(c)->flags);
				if (query(qprefix 0, "FIX?")) {
					ff(c)->flags &= ~FNODE_BLOCK_FIRST;
					update_block(c);
				}
			}
			if (__unlikely(SPAD2CPU32_LV(&ff(c)->prev1) | SPAD2CPU16_LV(&ff(c)->prev0))) {
				log_printf(0, "%s: INNER FNODE BLOCK %"blk_format"X HAS PREV POINTER TO %"blk_format"X", get_filename(p), blk + c, MAKE_D_OFF(ff(c)->prev0, ff(c)->prev1));
				if (query(qprefix 0, "FIX?")) {
					CPU2SPAD32_LV(&ff(c)->prev0, 0);
					CPU2SPAD16_LV(&ff(c)->prev1, 0);
					update_block(c);
				}
			}
		}
		if (ff(c)->flags & FNODE_BLOCK_LAST) {
			if (__unlikely(c != (1 << super.sectors_per_block_bits) - 1)) {
				log_printf(0, "%s: INNER FNODE BLOCK %"blk_format"X HAS LAST BLOCK FLAG (FLAGS %02X)", get_filename(p), blk + c, ff(c)->flags);
				if (query(qprefix 0, "FIX?")) {
					ff(c)->flags &= ~FNODE_BLOCK_LAST;
					update_block(c);
				}
			} else {
				last = 1;
			}
		}
		if (__unlikely(cc_check(&ff(c)->cc, &ff(c)->txc, "FNODE NEXT POINTER", get_filename, p, blk))) update_block(c);
		if (__unlikely(cc_valid(ff(c)->cc, ff(c)->txc))) {
			if (!last) {
				log_printf(0, "%s: INNER FNODE BLOCK %"blk_format"X HAS NEXT POINTER TO %"blk_format"X", get_filename(p), blk + c, MAKE_D_OFF(ff(c)->next0, ff(c)->next1));
				if (query(qprefix 0, "FIX?")) {
					CPU2SPAD16_LV(&ff(c)->cc, 0);
					CPU2SPAD32_LV(&ff(c)->txc, 0x80000000U);
					CPU2SPAD32_LV(&ff(c)->next0, 0);
					CPU2SPAD16_LV(&ff(c)->next1, 0);
					update_block(c);
				}
			} else {
				*next_valid = 1;
				*next = MAKE_D_OFF(ff(c)->next0, ff(c)->next1);
			}
		}
		first = 0;
		c++;
	}
	if (last) return 0;
	if (__unlikely(prev != 0)) {
		log_printf(0, "%s: CHAIN FNODE PAGE %"blk_format"X IS LONGER THAN ONE BLOCK", get_filename(p), blk);
		truncq:
		if (query(qprefix 0, "TRUNCATE?")) {
			blk -= 1 << super.sectors_per_block_bits;
			if (__unlikely(!(f = read_fblk(blk)))) goto read_err;
			ff((1 << super.sectors_per_block_bits) - 1)->flags |= FNODE_BLOCK_LAST;
			update_block((1 << super.sectors_per_block_bits) - 1);
		} else {
			goto sys_free_ret_1;
		}
		return 0;
	}
	blk += 1 << super.sectors_per_block_bits;
	if (__unlikely(!(f = read_fblk(blk)))) {
		log_printf(0, "%s: ERROR READING FNODE RUN: %s", get_filename(p), fblk_error);
		goto truncq;
	}
	if (__unlikely(SPAD2CPU32_LV(&f->magic) != magic(blk, FNODE_BLOCK_MAGIC))) {
		log_printf(0, "%s: BAD MAGIC IN FNODE RUN ON BLOCK %"blk_format"X: %08X", get_filename(p), blk, (unsigned)SPAD2CPU32_LV(&f->magic));
		goto truncq;
	}
	if (__unlikely(f->flags & FNODE_BLOCK_FIRST)) {
		log_printf(0, "%s: INNER FNODE BLOCK %"blk_format"X HAS FIRST BLOCK FLAG (FLAGS %02X)", get_filename(p), blk, f->flags);
		goto truncq;
	}
	if (__unlikely(!((unsigned)blk & ((1 << super.sectors_per_page_bits) - 1)))) {
		log_printf(0, "%s: FNODE RUN CROSSES PAGE BOUNDARY AT %"blk_format"X", get_filename(p), blk);
		goto truncq;
	}
	r = sys_alloc(blk, 1 << super.sectors_per_block_bits, 1, "INNER FNODE BLOCK", NULL, get_filename, p);
	if (__unlikely(r)) {
		if (r == 1) goto truncq;
		return r;
	}
	alloc_len += 1 << super.sectors_per_block_bits;
	*len += 1 << super.sectors_per_block_bits;
	goto next_block;
}

int get_fnode_description(blk_t blk, LIST_HEAD *l, const char *(*get_filename)(void *p), void *p)
{
	int r;
	unsigned len;
	FNODE_DESC *fd;
	int next_valid;
	int dno;
	blk_t next, prev;
	INIT_LIST(l);
	if (__unlikely(r = scan_fnode_blocks_run(blk, 0, get_filename, p, &len, &dno, &next_valid, &next))) return r;
	another_run:
	if (__unlikely(!(fd = mem_alloc(sizeof(FNODE_DESC))))) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR FNODE DESCRIPTION");
		free_fnode_description(l);
		return 2;
	}
	fd->start = blk;
	fd->len = len;
	fd->dno = dno;
	ADD_TO_LIST_END(l, &fd->list);
	if (__unlikely(next_valid)) {
		prev = blk + len - 1;
		blk = next;
		if (__likely(!(r = scan_fnode_blocks_run(blk, prev, get_filename, p, &len, &dno, &next_valid, &next)))) goto another_run;
		if (r == 2) {
			free_fnode_description(l);
			return 2;
		}
		if (query(qprefix 0, "TRUNCATE PREVIOUS?")) {
			struct fnode_block *f = read_buffer(prev, 1, 0);
			if (__unlikely(!f)) {
				log_printf(1, "%s: ERROR RE-READING FNODE BLOCK: %s", get_filename(p), fblk_error);
				log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN READ BEFORE");
			} else if (__unlikely(SPAD2CPU32_LV(&f->magic) != magic(prev, FNODE_BLOCK_MAGIC))) {
				log_printf(1, "%s: BAD MAGIC WHEN RE-READING FNODE BLOCK %"blk_format"X: %08X", get_filename(p), prev, (unsigned)SPAD2CPU32_LV(&f->magic));
				log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
			} else {
				CPU2SPAD16_LV(&f->cc, 0);
				CPU2SPAD32_LV(&f->txc, 0x80000000U);
				f->flags |= FNODE_BLOCK_CHECKSUM_VALID;
				f->checksum ^= CHECKSUM_BASE ^ __byte_sum(f, FNODE_BLOCK_SIZE);
				write_buffer(prev, 1, f);
			}
		}
	}
	return 0;
}

void free_fnode_description(LIST_HEAD *l)
{
	while (!LIST_EMPTY(l)) {
		FNODE_DESC *fd = LIST_STRUCT(l->next, FNODE_DESC, list);
		DEL_FROM_LIST(&fd->list);
		mem_free(fd);
	}
}

int dealloc_and_free_fnode_description(LIST_HEAD *l)
{
	int r = 0;
	FNODE_DESC *fd;
	LIST_FOR_EACH(fd, l, FNODE_DESC, list) {
		if (__unlikely(r = sys_free(fd->start, fd->len, 1))) break;
	}
	free_fnode_description(l);
	return r;
}
