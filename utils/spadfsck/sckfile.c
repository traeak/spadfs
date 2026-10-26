#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

/*
static __u64 blocks_2_size(blk_t b)
{
	__u64 size = b & ~(__u64)((1 << super.sectors_per_block_bits) - 1);
	size <<= SSIZE_BITS;
	if (size >= ((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) * SPAD2CPU16_LV(&super.cluster_threshold)) size &= ~(((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) - 1);
	return size;
}
*/

static struct anode *map_anode(blk_t ano, blk_t min, blk_t max, int *was_error, int ign_err, const char *(*get_filename)(void *p), void *p)
{
	struct anode *anode;
	int i;
	if (__unlikely(!ano)) {
		if (!ign_err) log_printf(0, "%s: ZERO ANODE POINTER", get_filename(p));
		return NULL;
	}
	if (__unlikely(!ptr_valid(ano, 1))) {
		if (!ign_err) log_printf(0, "%s: INVALID ANODE POINTER %"blk_format"X: %s", get_filename(p), ano, ptr_error);
		return NULL;
	}
	anode = read_buffer(ano, 1, 0);
	if (__unlikely(!anode)) {
		read_err:
		if (!ign_err) log_printf(0, "%s: ERROR READING ANODE %"blk_format"X: %s", get_filename(p), ano, buffer_error);
		return NULL;
	}
	if (__unlikely(SPAD2CPU32_LV(&anode->magic) != magic(ano, ANODE_MAGIC))) {
		magic_err:
		if (!ign_err) log_printf(0, "%s: BAD MAGIC ON ANODE %"blk_format"X: %08X", get_filename(p), ano, (unsigned)SPAD2CPU32_LV(&anode->magic));
		return NULL;
	}
#define update_anode							\
do {									\
	anode->flags |= ANODE_CHECKSUM_VALID;				\
	anode->checksum = 0; spadfs_meta_seal(anode);\
	write_buffer(ano, 1, anode);					\
	if (__unlikely(!(anode = read_buffer(ano, 1, 0)))) goto read_err;\
	if (__unlikely(SPAD2CPU32_LV(&anode->magic) != magic(ano, ANODE_MAGIC)))									\
		goto magic_err;						\
} while (0)
	if (anode->flags & ANODE_CHECKSUM_VALID && __unlikely(spadfs_meta_verify(anode))) {
		log_printf(0, "%s: BAD CHECKSUM ON ANODE %"blk_format"X: stored %08X computed %08X", get_filename(p), ano, spadfs_meta_stored_crc(anode), spadfs_meta_computed_crc(anode));
		if (query(qprefix 0, "FIX?")) {
			update_anode;
			*was_error = 2;
		}
	}
	if (__unlikely((unsigned)(anode->valid_extents - 1) >= ANODE_N_EXTENTS)) {
		int i;
		for (i = 0; i < ANODE_N_EXTENTS; i++) {
			if (SPAD2CPU64_LV(&anode->x[i - 1].end_off) >= SPAD2CPU64_LV(&anode->x[i].end_off)) break;
			if (SPAD2CPU64_LV(&anode->x[i].blk) >= SPAD2CPU64_LV(&super.size)) break;
			if (!SPAD2CPU64_LV(&anode->x[i].blk)) break;
			if (SPAD2CPU64_LV(&anode->x[i].end_off) == (__u64)-1) {
				i++;
				break;
			}
			if (SPAD2CPU64_LV(&anode->x[i].end_off) > SPAD2CPU64_LV(&super.size)) break;
		}
		if (i || !ign_err) log_printf(0, "%s: ANODE %"blk_format"X HAS %X VALID EXTENTS", get_filename(p), ano, anode->valid_extents);
		if (!i) return NULL;
		if (query(qprefix 0, "FIX?")) {
			anode->valid_extents = i;
			update_anode;
			*was_error = 2;
		} else return NULL;
	}
	if (__unlikely(SPAD2CPU64_LV(&anode->start_off) != min)) {
		if (!ign_err && !*was_error) log_printf(0, "%s: ANODE %"blk_format"X HAS INVALID START OFFSET: %"blk_format"X != %"blk_format"X", get_filename(p), ano, (blk_t)SPAD2CPU64_LV(&anode->start_off), min);
		if (!*was_error) *was_error = 1;
		return anode;
	}
	for (i = 0; i < anode->valid_extents; i++) {
		if (__unlikely(SPAD2CPU64_LV(&anode->x[i].end_off) <= SPAD2CPU64_LV(&anode->x[i - 1].end_off))) {
			if (!ign_err && !*was_error) log_printf(0, "%s: ANODE %"blk_format"X HAS NON-MONOTONIC EXTENTS", get_filename(p), ano);
			if (!*was_error) *was_error = 1;
			break;
		}
		if (__unlikely(SPAD2CPU64_LV(&anode->x[i - 1].end_off) < min) || __unlikely(SPAD2CPU64_LV(&anode->x[i - 1].end_off) >= max)) {
			if (!ign_err && !*was_error) log_printf(0, "%s: ANODE %"blk_format"X HAS OFFSET %"blk_format"X OUT OF RANGE %"blk_format"X,%"blk_format"X", get_filename(p), ano, (blk_t)SPAD2CPU64_LV(&anode->x[i - 1].end_off), min, max);
			if (!*was_error) *was_error = 1;
			break;
		}
	}
	return anode;
}

static int bmap(struct fnode *f, blk_t lbn, blk_t *blk, blk_t *nblks, blk_t *nback, blk_t *aptr, int *was_error, int ign_err, const char *(*get_filename)(void *p), void *p)
{
	blk_t ano;
	struct anode *anode;
	int depth_now, depth_total;
	int direct, off;
	blk_t nb;
	blk_t min = 0, max = -1;
	*blk = 0;
	*aptr = 0;
	if (__likely(lbn < SPAD2CPU16_LV(&f->run1n))) {
		*blk = MAKE_D_OFF(f->run10, f->run11) + lbn;
		*nblks = SPAD2CPU16_LV(&f->run1n) - lbn;
		*nback = lbn;
		return 0;
	}
	if (__likely(lbn < SPAD2CPU16_LV(&f->run1n) + SPAD2CPU16_LV(&f->run2n))) {
		*blk = MAKE_D_OFF(f->run20, f->run21) + lbn - SPAD2CPU16_LV(&f->run1n);
		*nblks = SPAD2CPU16_LV(&f->run1n) + SPAD2CPU16_LV(&f->run2n) - lbn;
		*nback = lbn - SPAD2CPU16_LV(&f->run1n);
		return 0;
	}
	ano = MAKE_D_OFF(f->anode0, f->anode1);
	depth_now = 0;
	depth_total = 0;
	subnode:
	anode = map_anode(ano, min, max, was_error, ign_err, get_filename, p);
	if (__unlikely(!anode)) return -1;
	if (__likely(!depth_now)) {
		if (__unlikely(anode->valid_extents < 3) ||
		    __unlikely(SPAD2CPU64_LV(&anode->x[0].end_off) != SPAD2CPU16_LV(&f->run1n)) ||
		    __unlikely(SPAD2CPU64_LV(&anode->x[1].end_off) != SPAD2CPU16_LV(&f->run1n) + SPAD2CPU16_LV(&f->run2n)) ||
		    __unlikely(SPAD2CPU64_LV(&anode->x[0].blk) != MAKE_D_OFF(f->run10, f->run11)) ||
		    __unlikely(SPAD2CPU64_LV(&anode->x[1].blk) != MAKE_D_OFF(f->run20, f->run21))) {
			if (!*was_error) log_printf(0, "%s: FIRST TWO EXTENTS IN ANODE DON'T MATCH EXTENTS IN FNODE", get_filename(p));
			if (!*was_error) *was_error = 1;
		}
	}
	if (__unlikely(!!(anode->flags & ANODE_ROOT) != !depth_now)) {
		if (!*was_error) {
			if (!depth_now) log_printf(0, "%s: ROOT ANODE %"blk_format"X DOESN'T HAVE ROOT ANODE FLAG", get_filename(p), ano);
			else log_printf(0, "%s: INNER ANODE %"blk_format"X HAS ROOT ANODE FLAG", get_filename(p), ano);
		}
		if (!*was_error) *was_error = 1;
	}
	direct = find_direct(depth_now, depth_total);
	off = find_in_anode(anode, lbn, anode->valid_extents);
	if (__unlikely(lbn >= SPAD2CPU64_LV(&anode->x[off].end_off)) || __unlikely(lbn < SPAD2CPU64_LV(&anode->x[off - 1].end_off))) {
		return -1;
	}
	if (off >= direct) {
		blk_t old_ano = ano;
		min = SPAD2CPU64_LV(&anode->x[off - 1].end_off);
		if (max > SPAD2CPU64_LV(&anode->x[off].end_off)) max = SPAD2CPU64_LV(&anode->x[off].end_off);
		if (SPAD2CPU64_LV(&anode->x[off].end_off) != (__u64)-1) *blk = SPAD2CPU64_LV(&anode->x[off].end_off);
		ano = SPAD2CPU64_LV(&anode->x[off].blk);
		update_depth(&depth_now, &depth_total, off);
		if (__likely(off > direct)) {
			anode = map_anode(SPAD2CPU64_LV(&anode->x[off - 1].blk), SPAD2CPU64_LV(&anode->x[off - 2].end_off), SPAD2CPU64_LV(&anode->x[off - 1].end_off), was_error, 1, get_filename, p);
			if (__likely(anode != NULL) && __unlikely(anode->valid_extents != ANODE_N_EXTENTS)) {
				if (!ign_err && !*was_error) log_printf(0, "%s: ANODE TREE NOT FILLED UP (ANODE %"blk_format"X, OFFSET %d)", get_filename(p), old_ano, off);
				if (!*was_error) *was_error = 1;
			}
		}
		goto subnode;
	}
	if (!off || (off == 2 && !depth_now && !depth_total)) *aptr = ano;
	nb = lbn - SPAD2CPU64_LV(&anode->x[off - 1].end_off);
	*nback = nb;
	*blk = SPAD2CPU64_LV(&anode->x[off].blk) + nb;
	*nblks = SPAD2CPU64_LV(&anode->x[off].end_off) - lbn;
	return 0;
}

static /*__attribute__((noinline,noclone))*/ int check_nonzero(char *ptr, unsigned len)
{
	while (len & 15) {
		if (__unlikely(*ptr))
			return 1;
		ptr++;
		len--;
	}
	while (len) {
		if (__unlikely((*(__u64 *)ptr | *(__u64 *)(ptr + 8)) != 0))
			return 1;
		ptr += 16;
		len -= 16;
	}
	return 0;
}

int check_last_block(__u64 size, ALLOC *al, const char *(*get_filename)(void *p), void *p)
{
	blk_t last_lbn = (blk_t)(size >> (super.sectors_per_block_bits + SSIZE_BITS)) << super.sectors_per_block_bits;
	if (size & ((1U << SSIZE_BITS << super.sectors_per_block_bits) - 1) && al->lbn <= last_lbn && al->lbn + al->len > last_lbn) {
		blk_t last_block = last_lbn - al->lbn + al->start;
		unsigned offset = size & ((1U << SSIZE_BITS << super.sectors_per_block_bits) - 1);
		unsigned length = -size & ((1U << SSIZE_BITS << super.sectors_per_block_bits) - 1);
		char *tail;
		tail = read_buffer(last_block, 1U << super.sectors_per_block_bits,
			last_block < (blk_t)SPAD2CPU16_LV(&super.large_file_group) << super.sectors_per_group_bits && size <= BUFFER_SIZE / 16 ? 0 :
			BUFFER_NOCACHE);
		if (unlikely(!tail)) {
			if (!p)
				return 1;
			log_printf(0, "%s: ERROR READING LAST BLOCK", get_filename(p));
			return 1;
		}
		if (check_nonzero(tail + offset, length)) {
			if (!p) {
				log_printf(0, "FILE TAIL IS NOT ZEROED (SECTOR %"blk_format"X, OFFSET %X)", last_block, offset);
				return 1;
			}
			log_printf(0, "%s: FILE TAIL IS NOT ZEROED (SECTOR %"blk_format"X, OFFSET %X)", get_filename(p), last_block, offset);
			if (query(qprefix 0, "ZERO?")) {
				memset(tail + offset, 0, length);
				write_buffer(last_block, 1U << super.sectors_per_block_bits, tail);
			}
		}
	}
	return 0;
}

int check_file(struct fnode *fn, LIST_HEAD *allocs, const char *(*get_filename)(void *p), void *p, __u32 prealloc)
{
	int was_error = 0;
	__u64 size = SPAD2CPU64(fn->size[!cc_valid(fn->cc, fn->txc)]);
	blk_t b, blocks;
	ALLOC *al;
	FILE_SECTORS(1 << SSIZE_BITS << super.sectors_per_block_bits, 1 << SSIZE_BITS << super.sectors_per_cluster_bits, ((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) * SPAD2CPU16_LV(&super.cluster_threshold), size, blocks);
	INIT_LIST(allocs);
	b = 0;
	while (b < blocks) {
		blk_t blk, nblks, nback, aptr;
		blk_t blk2, nblks2, nback2, aptr2;
		if (__unlikely(bmap(fn, b, &blk, &nblks, &nback, &aptr, &was_error, 0, get_filename, p))) {
			if (!blk) {
				if (b < (size + (1 << SSIZE_BITS) - 1) >> SSIZE_BITS)
					log_printf(0, "%s IS TRUNCATED AT LOGICAL SECTOR %"blk_format"X", get_filename(p), b);
				else
					log_printf(0, "%s DOES NOT HAVE CORRECT PADDING, %"blk_format"d SECTORS MISSING", get_filename(p), blocks - b);
				was_error = 1;
	/* this would unnecesarily truncate valid portions of file ... better
	   fix for this condition is in SCKRCV.C
				size = blocks_2_size(b);
				CPU2SPAD64_LV(&fn->size[0], size);
				CPU2SPAD64_LV(&fn->size[1], size);
				FILE_SECTORS(1 << SSIZE_BITS << super.sectors_per_block_bits, 1 << SSIZE_BITS << super.sectors_per_cluster_bits, ((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) * SPAD2CPU16_LV(&super.cluster_threshold), size, blocks);
				*/
				break;
			}
			log_printf(0, "%s CONTAINS ALLOCATION HOLE AT LOGICAL SECTORS %"blk_format"X - %"blk_format"X", get_filename(p), b, blk - 1);
			b = blk;
			was_error = 1;
			continue;
		}
		if (__unlikely(nback != 0)) {
			log_printf(0, "%s HAS OVERLAPPING ALLOCATION AT LOGICAL SECTORS %"blk_format"X - %"blk_format"X", get_filename(p), b - nback, b - 1);
			was_error = 1;
		}
		if (__unlikely(blk >= SPAD2CPU64_LV(&super.size))) {
			log_printf(0, "%s HAS POINTER BEYOND END OF DEVICE (%"blk_format"X,%"blk_format"X) AT LOGICAL SECTOR %"blk_format"X", get_filename(p), blk, nblks, b);
			was_error = 1;
			if (__unlikely(b + nblks > blocks) || __unlikely(b + nblks < b)) b = blocks;
			else b += nblks;
			continue;
		}
		al = mem_alloc(sizeof(ALLOC));
		if (__unlikely(!al)) {
			log_printf(1, "CAN'T ALLOC MEMORY FOR DESCRIPTION OF A FILE RUN");
			return 2;
		}
		al->lbn = b;
		al->start = blk;
		if (__unlikely(b + nblks > blocks) || __unlikely(b + nblks < b)) b = blocks;
		else b += nblks;
		al->len = b - al->lbn;
		al->ano = aptr;
		ADD_TO_LIST_END(allocs, &al->list);
		if (__unlikely(bmap(fn, b - 1, &blk2, &nblks2, &nback2, &aptr2, &was_error, 1, get_filename, p))) {
			log_printf(0, "%s HAS PARTIALLY UNREACHABLE ALLOCATION AT LOGICAL SECTORS %"blk_format"X - %"blk_format"X", get_filename(p), al->lbn, b - 1);
			was_error = 1;
			continue;
		}
		if (__unlikely(nback2 + 1 != al->len)) {
			log_printf(0, "%s HAS DIFFERENT LENGTH ALLOCATIONS AT LOGICAL SECTORS %"blk_format"X - %"blk_format"X", get_filename(p), al->lbn, b - 1);
			was_error = 1;
			continue;
		}
		if (__unlikely(blk2 - nback2 != blk)) {
			log_printf(0, "%s HAS INCONSISTENT ALLOCATION AT LOGICAL SECTORS %"blk_format"X - %"blk_format"X", get_filename(p), al->lbn, b - 1);
			was_error = 1;
			continue;
		}
	}
	rescan:
	LIST_FOR_EACH_BACK(al, allocs, ALLOC, list) {
		if (al->lbn >= blocks) {
			DEL_FROM_LIST(&al->list);
			mem_free(al);
			goto rescan;
		}
		if (al->lbn + al->len > blocks) {
			al->len = blocks - al->lbn;
		}
		break;
	}
#ifdef TESTCODE
	if (__unlikely(recover_all_files)) {
		log_printf(0, "%s: TESTING RECOVERY OF FILE", get_filename(p));
		was_error = 1;
	}
#endif
	if (__likely(!was_error)) LIST_FOR_EACH(al, allocs, ALLOC, list) {
		int r = sys_alloc(al->start, al->len, 2, "FILE DATA", &al->lbn, get_filename, p);
		if (__unlikely(r)) {
			if (r != 1) return r;
			goto free_prev;
		}
		if (__unlikely(al->ano != 0)) {
			r = sys_alloc(al->ano, 1 << super.sectors_per_block_bits, 2, "ANODE", &al->lbn, get_filename, p);
			if (__unlikely(r)) {
				ALLOC *all;
				if (r != 1) return r;
				if (sys_free(al->start, al->len, 2)) return 2;
				free_prev:
				LIST_FOR_EACH(all, allocs, ALLOC, list) {
					if (all == al) break;
					if (all->ano != 0) if (sys_free(all->ano, 1 << super.sectors_per_block_bits, 2)) return 2;
					if (sys_free(all->start, all->len, 2)) return 2;
				}
				was_error = 1;
				break;
			}
		}
		if (__unlikely(zero_file_tails))
			if (__unlikely(check_last_block(size - prealloc, al, get_filename, p)))
				return 1;
	}
	/* check that anode is correct --- i.e.
		check anode root flag ... DONE
		monotonic logical offsets ... DONE
		first 2 extents are equal to inline ... DONE
		all anodes except the last one are full ... DONE
		all values except the last one within range ... DONE
		correct logical start offset ... DONE
	*/
	if (__unlikely(was_error) && LIST_EMPTY(allocs) && size) {
		log_printf(0, "%s HAS NO RECOVERABLE DATA", get_filename(p));
		return -2;
	}
	if (__unlikely(was_error == 2)) log_printf(0, "%s: FILE WILL BE RECOVERED", get_filename(p));
	if (__unlikely(was_error)) return 1;

	return 0;
}

int force_block_alloc_file_runs(LIST_HEAD *l)
{
	int r;
	ALLOC *al;
	LIST_FOR_EACH(al, l, ALLOC, list) {
		if (__unlikely(r = force_alloc_blocks(al->start, al->len))) return r;
		if (al->ano != 0) if (__unlikely(r = force_alloc_blocks(al->ano, 1 << super.sectors_per_block_bits))) return r;
	}
	return 0;
}

int dealloc_blocklist(int r, LIST_HEAD *list, int pass)
{
	ALLOC *al;
	if (r >= 2) return r;
	LIST_FOR_EACH(al, list, ALLOC, list) {
		int rr;
		if (__unlikely(rr = sys_free(al->start, al->len, pass))) {
			return rr;
		}
		if (al->ano != 0) if ((rr = sys_free(al->ano, 1 << super.sectors_per_block_bits, pass))) {
			return rr;
		}
	}
	return r;
}

int dealloc_and_free_blocklist(int r, LIST_HEAD *list)
{
	r = dealloc_blocklist(r, list, -1);
	free_file_runs(list);
	return r;
}

void free_file_runs(LIST_HEAD *l)
{
	while (!LIST_EMPTY(l)) {
		ALLOC *al = LIST_STRUCT(l->next, ALLOC, list);
		DEL_FROM_LIST(&al->list);
		mem_free(al);
	}
}

