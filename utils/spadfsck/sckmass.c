#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#define BLOCK_HASH_SIZE		262144
#define BLOCK_HASH(b)		((b) & (BLOCK_HASH_SIZE - 1))

static blk_t mass_scan_total_blocks;
static blk_t mass_scan_scanned_blocks;

typedef struct {
	LIST_ENTRY e;
	blk_t start;
	blk_t end;
} AREA;

static LIST_HEAD areas;

typedef struct {
	blk_t start;
	blk_t len;
} RANGE;

typedef struct {
	LIST_ENTRY hash;
	LIST_ENTRY all;
	LIST_ENTRY tmp;
	blk_t b;
	blk_t refcount;
	blk_t reachable_nodes;
	char reachable_state;
	int n_pointers;
	RANGE pointers[1];
} BLOCK;

static LIST_HEAD all_blocks;
static XLIST_HEAD block_hash[BLOCK_HASH_SIZE];
static blk_t all_blocks_n;
static blk_t recovered_blocks_n;

static int scan_block(blk_t b);
static int recover(BLOCK *bl, int *done);

static char lost_plus_found[256];
static blk_t file_idx;

void mass_scan_init(void)
{
	unsigned i;
	INIT_LIST(&areas);
	INIT_LIST(&all_blocks);
	for (i = 0; i < BLOCK_HASH_SIZE; i++) INIT_XLIST(&block_hash[i]);
}

void mass_scan_done(void)
{
	while (!LIST_EMPTY(&areas)) {
		AREA *a = LIST_STRUCT(areas.next, AREA, e);
		DEL_FROM_LIST(&a->e);
		mem_free(a);
	}
	while (!LIST_EMPTY(&all_blocks)) {
		BLOCK *bl = LIST_STRUCT(all_blocks.next, BLOCK, all);
		DEL_FROM_LIST(&bl->hash);
		DEL_FROM_LIST(&bl->all);
		mem_free(bl);
	}
}

int mass_scan_prepare(blk_t total_blocks)
{
	unsigned lf_idx;

	mass_scan_total_blocks = total_blocks;
	mass_scan_scanned_blocks = 0;

	file_idx = 0;
	lf_idx = 0;
	again:
	_snprintf(lost_plus_found, sizeof lost_plus_found, "/LOST+FOUND.%u", lf_idx);
	if (file_exists_in_root(lost_plus_found)) {
		lf_idx++;
		if (lf_idx) goto again;
		_snprintf(lost_plus_found, sizeof lost_plus_found, "/LOST+FOUND");
	}
	return 0;
}

static void mass_scan_set_range_internal(blk_t start, blk_t end)
{
	AREA *a;
	if (__likely(!LIST_EMPTY(&areas))) {
		a = LIST_STRUCT(areas.prev, AREA, e);
		if (__unlikely(a->end == start)) {
			a->end = end;
			return;
		}
	}
	a = mem_alloc(sizeof(AREA));
	if (!a) return;
	a->start = start;
	a->end = end;
	ADD_TO_LIST_END(&areas, &a->e);
}

void mass_scan_set_range(blk_t start, blk_t end)
{
	do {
		blk_t new_start, new_end;
		exclude_recovery_blocks(start, end, &new_start, &new_end);
		if (unlikely(new_start >= new_end))
			break;
		mass_scan_set_range_internal(new_start, new_end);
		start = new_end;
	} while (start < end);
}

void mass_scan_do(void)
{
	while (!LIST_EMPTY(&areas)) {
		blk_t b;
		AREA *a = LIST_STRUCT(areas.next, AREA, e);
		int n = 0;
		for (b = a->start; b < a->end; b++, mass_scan_scanned_blocks++) {
			print_progress(mass_scan_total_blocks, mass_scan_scanned_blocks, 1);
			if (!n) n = scan_block(b);
			if (n) n--;
		}
		DEL_FROM_LIST(&a->e);
		mem_free(a);
	}
}

static int compare_ranges(const void *r1_, const void *r2_)
{
	const RANGE *r1 = r1_, *r2 = r2_;
	if (r1->start < r2->start) return -1;
	if (r1->start > r2->start) return 1;
	if (r1->len < r2->len) return -1;
	if (r1->len > r2->len) return 1;
	return 0;
}

static int scan_block(blk_t b)
{
	void *a;
	__u32 mag;
	RANGE *pointers;
	int n_pointers;
	int i, j;
	BLOCK *bl;
	int len;

	pointers = alloca(sizeof(RANGE) << dnode_hash_bits);
	n_pointers = 0;

	if (!(a = read_buffer(b, 1, BUFFER_FREE_SOON))) goto ret_0;
	mag = SPAD2CPU32_LV((__u32 *)a);

	if (mag == magic(b, FNODE_BLOCK_MAGIC)) {
		unsigned fp;
		struct fnode_block *f = a;
		len = 1;

		if (!(f->flags & FNODE_BLOCK_LAST)) {
			pointers[n_pointers].start = b + 1;
			pointers[n_pointers++].len = 1;
		} else if (cc_valid(f->cc, f->txc)) {
			pointers[n_pointers].start = MAKE_D_OFF(f->next0, f->next1);
			pointers[n_pointers++].len = 1;
		}

		fp = SIZEOF_FNODE_BLOCK;
#define fn ((struct fnode *)((char *)f + fp))
		do {
			unsigned size;

			size = SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_SIZE;

			if (cc_valid(fn->cc, fn->txc) && SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_FREE) {
				if (__unlikely(size < FNODE_HEAD_SIZE) || __unlikely(fp + size > FNODE_BLOCK_SIZE)) {
					corrupted_fnode_block:
					log_printf(0, "CORRUPTED FNODE BLOCK AT %"blk_format"X", b);
					goto ret_0;
				}
				goto cont;
			}
			if (__unlikely(size <= FNODE_NAME_POS) || __unlikely(fp + size > FNODE_BLOCK_SIZE)) {
				goto corrupted_fnode_block;
			}
			if (__unlikely(fn->flags & FNODE_FLAGS_DIR)) {
				blk_t subblk;
				if (cc_valid(fn->cc, fn->txc)) subblk = MAKE_D_OFF(fn->run10, fn->run11);
				else subblk = MAKE_D_OFF(fn->run20, fn->run21);
				pointers[n_pointers].start = subblk;
				pointers[n_pointers++].len = 1;
			} else if (__unlikely(fn->flags & FNODE_FLAGS_HARDLINK)) {
				blk_t subblk;
				subblk = MAKE_D_OFF(fn->anode0, fn->anode1);
				pointers[n_pointers].start = subblk;
				pointers[n_pointers++].len = 1;
			} else {
				blk_t subblk;
#if 0
				__u64 size;
				blk_t size_blocks;
				unsigned b1, b2;

				size = SPAD2CPU64_LV(&fn->size[!cc_valid(fn->cc, fn->txc)]);
				FILE_SECTORS(1 << SSIZE_BITS << super.sectors_per_block_bits, 1 << SSIZE_BITS << super.sectors_per_cluster_bits, ((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) * SPAD2CPU16_LV(&super.cluster_threshold), size, size_blocks);

				b1 = SPAD2CPU16_LV(&fn->run1n);
				if (b1 > size) b1 = size;
				if (b1) {
					pointers[n_pointers].start = MAKE_D_OFF(fn->run10, fn->run11);
					pointers[n_pointers++].len = b1;
				}
				if (size > b1) {
					b2 = SPAD2CPU16_LV(&fn->run2n);
					if (b2 > size - b1) b2 = size - b1;
					if (b2 && 0) {
						pointers[n_pointers].start = MAKE_D_OFF(fn->run20, fn->run21);
						pointers[n_pointers++].len = b2;
					}
				}
#endif
				subblk = MAKE_D_OFF(fn->anode0, fn->anode1);
				if (subblk) {
					pointers[n_pointers].start = subblk;
					pointers[n_pointers++].len = 1;
				}
			}
			cont:
			fp += size;
		} while (fp < FNODE_BLOCK_SIZE);
#undef fn
		goto store_it;
	} else if (mag == magic(b, DNODE_PAGE_MAGIC)) {
		struct dnode_page *dn = a;
		unsigned data;

		if (__unlikely((dn->gflags & DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1) >> DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT != super.sectors_per_page_bits - 1)) {
			corrupted_dnode_page:
			log_printf(0, "CORRUPTED DNODE PAGE AT %"blk_format"X", b);
			goto ret_0;
		}

		len = dnode_page_sectors;

		if (!(dn = read_buffer(b, dnode_page_sectors, BUFFER_FREE_SOON))) goto ret_0;
		if (cc_valid(dn->cc, dn->txc)) data = DNODE_ENTRY_OFFSET;
		else data = DNODE_ENTRY_OFFSET + dnode_data_size;
#define dnode_entry(x)	((struct dnode_page_entry *)((char *)dn + data) + (x))
#define dnode_val(x)	dnode_decode(dnode_entry(x))
		for (i = 0; i < 1 << dnode_hash_bits; i++) {
			blk_t subblk = dnode_val(i);
			if (__unlikely(subblk >= SPAD2CPU64_LV(&super.size)))
				goto corrupted_dnode_page;
			pointers[n_pointers].start = subblk;
			pointers[n_pointers++].len = 1;
		}
		goto store_it;
#undef dnode_entry
#undef dnode_val
	}

	ret_0:
	return 0;

	store_it:

	qsort(pointers, n_pointers, sizeof(RANGE), compare_ranges);
	for (i = 0, j = 0; i < n_pointers; i++) {
		if (j && !memcmp(&pointers[i], &pointers[j - 1], sizeof(RANGE)))
			continue;
		pointers[j] = pointers[i];
		j++;
	}
	n_pointers = j;

	bl = mem_alloc(sizeof(BLOCK) + (n_pointers - 1) * sizeof(RANGE));
	if (!bl) return 0;
	bl->b = b;
	bl->refcount = 0;
	bl->n_pointers = n_pointers;
	memcpy(bl->pointers, pointers, n_pointers * sizeof(RANGE));
	ADD_TO_LIST_END(&all_blocks, &bl->all);
	ADD_TO_XLIST(&block_hash[BLOCK_HASH(b)], &bl->hash);

	return len;
}

static BLOCK *mass_scan_find_block(blk_t b)
{
	BLOCK *bl;
	XLIST_FOR_EACH(bl, &block_hash[BLOCK_HASH(b)], BLOCK, hash)
		if (bl->b == b)
			return bl;
	return NULL;
}

static void adjust_references(BLOCK *bl, int delta)
{
	int i;
	for (i = 0; i < bl->n_pointers; i++) {
		blk_t j;
		for (j = 0; j < bl->pointers[i].len; j++) {
			BLOCK *ref = mass_scan_find_block(bl->pointers[i].start + j);
			if (ref) {
				if (delta < 0 && ref->refcount < -delta)
					ref->refcount = 0;
				else
					ref->refcount += delta;
			}
		}
	}
}

static void get_reachable(BLOCK *bb)
{
	DECL_LIST(path);
	bb->reachable_state = 1;
	ADD_TO_LIST(&path, &bb->tmp);
	while (!LIST_EMPTY(&path)) {
		BLOCK *bl;
		unsigned i;
		blk_t total_reachable;
		bl = LIST_STRUCT(path.next, BLOCK, tmp);
		if (bl->reachable_state != 1) panic("get_reachable: invalid state %d", bl->reachable_state);
		total_reachable = 0;
		for (i = 0; i < bl->n_pointers; i++) {
			blk_t j;
			for (j = 0; j < bl->pointers[i].len; j++) {
				BLOCK *sub = mass_scan_find_block(bl->pointers[i].start + j);
				if (!sub) continue;
				if (!sub->reachable_state) {
					sub->reachable_state = 1;
					ADD_TO_LIST(&path, &sub->tmp);
					goto cont;
				} else if (__likely(sub->reachable_state == 2)) {
					if (__likely(total_reachable + sub->reachable_nodes > total_reachable))
						total_reachable += sub->reachable_nodes;
				}
				if (__likely(total_reachable + 1 > total_reachable))
					total_reachable += 1;
			}
		}
		bl->reachable_state = 2;
		bl->reachable_nodes = total_reachable;
		DEL_FROM_LIST(&bl->tmp);
		cont:;
	}
}

void mass_scan_sort(void)
{
	BLOCK *bl;

	all_blocks_n = 0;
	LIST_FOR_EACH(bl, &all_blocks, BLOCK, all) {
		all_blocks_n++;
	}
	recovered_blocks_n = 0;

	LIST_FOR_EACH(bl, &all_blocks, BLOCK, all) {
		bl->refcount = 0;
		bl->reachable_state = 0;
	}

	LIST_FOR_EACH(bl, &all_blocks, BLOCK, all) {
		adjust_references(bl, 1);
	}

	LIST_FOR_EACH(bl, &all_blocks, BLOCK, all) {
		if (!bl->reachable_state) {
			get_reachable(bl);
		}
	}
}

static int compare_reachable(const void *b1_, const void *b2_)
{
	const BLOCK *b1 = *(const BLOCK **)b1_;
	const BLOCK *b2 = *(const BLOCK **)b2_;
	if (b1->reachable_nodes > b2->reachable_nodes) return -1;
	if (b1->reachable_nodes < b2->reachable_nodes) return 1;
	if (b1->b < b2->b) return -1;
	if (b1->b > b2->b) return 1;
	return 0;
}

int lost_recover(void)
{
	int r;
	BLOCK *bl;
	blk_t min_refcount;
	blk_t max_reachable;
	int done;
	DECL_LIST(recover_now);
	int first_try = 1;

	BLOCK **recover_now_array;
	unsigned long recover_now_size, x;

	try_again:
	min_refcount = (blk_t)-1;
	max_reachable = 0;
	done = 0;

	LIST_FOR_EACH(bl, &all_blocks, BLOCK, all) {
		if (__unlikely(min_refcount == (blk_t)-1) || __unlikely(bl->refcount < min_refcount)) {
			min_refcount = bl->refcount;
			max_reachable = bl->reachable_nodes;
		} else if (bl->refcount == min_refcount && __unlikely(bl->reachable_nodes > max_reachable)) {
			max_reachable = bl->reachable_nodes;
		}
	}

	LIST_FOR_EACH(bl, &all_blocks, BLOCK, all) {
		if (bl->refcount == min_refcount && bl->reachable_nodes >= (max_reachable + 1) / 2) {
			BLOCK *old_bl = LIST_STRUCT(bl->all.prev, BLOCK, all);
			DEL_FROM_LIST(&bl->all);
			DEL_FROM_LIST(&bl->hash);
			ADD_TO_LIST_END(&recover_now, &bl->all);
			bl = old_bl;
		}
	}

	if (!LIST_EMPTY(&all_blocks) && LIST_EMPTY(&recover_now))
		panic("lost_recover: nothing picked");

	r = 0;

#if 1
	/*
	 * Sort directories according to the number of nodes in their
	 * subtree.
	 *
	 * Recover biggest directories first. If there are cross links,
	 * the biggest directories will be the ones least likely affected.
	 */
	recover_now_size = 0;

	LIST_FOR_EACH(bl, &recover_now, BLOCK, all) recover_now_size++;

	recover_now_array = mem_alloc(recover_now_size * sizeof(BLOCK *));
	if (!recover_now_array) return 2;

	x = 0;
	LIST_FOR_EACH(bl, &recover_now, BLOCK, all) recover_now_array[x++] = bl;
	qsort(recover_now_array, recover_now_size, sizeof(BLOCK *), compare_reachable);

	if (recover_now_size && first_try)
		status_printf(0, "preparing recovery of lost data...");
	for (x = 0; x < recover_now_size; x++) {
		print_progress(all_blocks_n, recovered_blocks_n, 1);
		recovered_blocks_n++;
		bl = recover_now_array[x];
		if (__unlikely(r = recover(bl, &done))) break;
		adjust_references(bl, -1);
	}
	mem_free(recover_now_array);
#else
	LIST_FOR_EACH(bl, &recover_now, BLOCK, all) {
		if (__unlikely(r = recover(bl, &done))) break;
		adjust_references(bl, -1);
	}
#endif
	while (!LIST_EMPTY(&recover_now)) {
		bl = LIST_STRUCT(recover_now.next, BLOCK, all);
		DEL_FROM_LIST(&bl->all);
		mem_free(bl);
	}

	if (!LIST_EMPTY(&all_blocks) && !done) {
		first_try = 0;
		goto try_again;
	}

	return r;
}

static int recover(BLOCK *bl, int *done)
{
	blk_t b_e;
	RECOVERY_FILEDESC rfd;
	char file[256];
	struct fnode_block *f;
	__u32 mag;
	unsigned req;
	int r;

	if (!ptr_valid(bl->b, 1 << super.sectors_per_block_bits)) return 0;

	f = read_buffer(bl->b, 1 << super.sectors_per_block_bits, 0);
	if (!f) return 0;
	mag = SPAD2CPU32_LV(&f->magic);
	if (mag == magic(bl->b, DNODE_PAGE_MAGIC)) {
		req = dnode_page_sectors;
	} else if (mag == magic(bl->b, FNODE_BLOCK_MAGIC)) {
		req = 1 << super.sectors_per_block_bits;
	} else return 0;

	if (__unlikely(r = get_blockrun(bl->b, 0, &b_e))) return r;
	if (b_e < bl->b + req) return 0;

	f = read_buffer(bl->b, req, 0);
	if (!f) return 0;
	if (mag == magic(bl->b, FNODE_BLOCK_MAGIC)) {
		int mod = 0;
		if (__unlikely(!(f->flags & FNODE_BLOCK_FIRST))) {
			f->flags |= FNODE_BLOCK_FIRST;
			mod = 1;
		}
		if (__unlikely(MAKE_D_OFF(f->prev0, f->prev1) != 0)) {
			f->prev0 = MAKE_PART_0(0);
			f->prev1 = MAKE_PART_0(0);
			mod = 1;
		}
		if (mod) {
			log_printf(0, "BLOCK AT %"blk_format"X IS LOST INNER DIRECTORY BLOCK", bl->b);
			if (query(qprefix 0, "FIX?")) {
				f->flags |= FNODE_BLOCK_CHECKSUM_VALID;
				f->checksum ^= CHECKSUM_BASE ^ __byte_sum(f, FNODE_BLOCK_SIZE);
				write_buffer(bl->b, 1, f);
			} else {
				return 0;
			}
		}
	}

	memset(&rfd, 0, sizeof rfd);
	rfd.size = SPAD2CPU32_LV(&super.flags_compat_ro) & FLAG_COMPAT_RO_DIRECTORY_SIZES ? (__u64)-1 : (__u64)0;
	rfd.ctime = rfd.mtime = time(NULL);
	rfd.flags = FNODE_FLAGS_DIR;
	rfd.dnode = bl->b;
	_snprintf(file, sizeof file, "DIRECTORY.%"blk_format"u", file_idx++);

	if (__unlikely(r = recover_file(&rfd, NULL, 0, lost_plus_found, file, NULL, 0, NULL))) return r;
	*done = 1;
	return 0;
}
