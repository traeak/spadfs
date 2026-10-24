#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

unsigned n_apages;
static unsigned apage_index_sectors;

static struct apage_index_entry *apage_index;
blk_t *swapspace;
int allow_swapping = 0;
static unsigned c_index;
static int apage_index_valid;
static int apages_valid;

void init_apages(void)
{
	apage_index = NULL;
	apage_index_valid = 0;
	apages_valid = 0;
}

static int blk_cmp(const void *p1, const void *p2)
{
	blk_t b1 = *(blk_t *)p1;
	blk_t b2 = *(blk_t *)p2;
	if (b1 < b2) return -1;
	if (__likely(b1 != b2)) return 1;
	return 0;
}

int check_apages(void)
{
	int j, pfj;
	int r;
	blk_t fb;
	blk_t freeb;
	int tail;
	int apage_checks, apage_err;
	unsigned used_apages;

	n_apages = N_APAGES(SPAD2CPU64_LV(&super.size), super.sectors_per_page_bits + SSIZE_BITS, super.sectors_per_block_bits + SSIZE_BITS);
	apage_index_sectors = APAGE_INDEX_SECTORS(n_apages, 1 << SSIZE_BITS << super.sectors_per_block_bits);
	/*__debug_printf("n_apages: %u, sectors: %u\n", n_apages, apage_index_sectors);*/

	if ((unsigned long)apage_index_sectors << SSIZE_BITS >> SSIZE_BITS != apage_index_sectors ||
	    (unsigned long)n_apages * sizeof(blk_t) / sizeof(blk_t) != n_apages) {
		log_printf(1, "TOO BIG APAGE INDEX, YOU NEED 64-BIT " TOOL_BANNER "");
		return 2;
	}

	apage_index = mem_alloc((unsigned long)apage_index_sectors << SSIZE_BITS);
	if (__unlikely(!apage_index)) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR APAGE INDEX (%lu BYTES)", (unsigned long)apage_index_sectors << SSIZE_BITS);
		return 2;
	}
	swapspace = mem_alloc((unsigned long)n_apages * sizeof(blk_t));
	if (__unlikely(!swapspace)) {
		mem_free(apage_index), apage_index = NULL;
		log_printf(1, "CAN'T ALLOC MEMORY FOR SWAPSPACE MAP (%lu BYTES)", (unsigned long)n_apages * sizeof(blk_t));
		return 2;
	}

	if (cc_check(&tx.a_cc, &tx.a_txc, "APAGE INDEX IN TXBLOCK", NULL, NULL, SPAD2CPU64_LV(&super.txblock))) write_txblock();
	c_index = !cc_valid(tx.a_cc, tx.a_txc);

	if (__unlikely(!tx_valid)) goto apage_index_invalid;

	if (extend) goto apage_index_invalid;

	status_printf(0, "checking apage index...");

	for (j = 0; j < apage_index_sectors; j++) {
		void *a;
		blk_t blk;
		print_progress(apage_index_sectors, j, 1);
		blk = SPAD2CPU64_LV(&super.apage_index[c_index]) + j;
		if ((!j || !(blk & (SECTORS_PER_BUFFER - 1))) && ((__u64)j + SECTORS_PER_BUFFER < apage_index_sectors))
			prefetch_buffer(blk + SECTORS_PER_BUFFER, BUFFER_FREE_SOON);
		if (__unlikely(!(a = read_buffer(blk, 1, BUFFER_FREE_SOON)))) {
			log_printf(0, "ERROR READING APAGE INDEX, SECTOR %u (ABS %"blk_format"X): %s", j, blk, buffer_error);
			goto apage_index_invalid;
		}
		memcpy((char *)apage_index + ((unsigned long)j << SSIZE_BITS), a, 1 << SSIZE_BITS);
	}

	for (j = 0; j < n_apages; j++) {
		if (__unlikely(!ptr_valid(SPAD2CPU64_LV(&apage_index[j].apage), 1 << super.sectors_per_page_bits))) {
			log_printf(0, "INVALID APAGE_INDEX [%u][%u] %"blk_format"X: %s", c_index, j, SPAD2CPU64_LV(&apage_index[j].apage), ptr_error);
			goto apage_index_invalid;
		}
	}

	fb = 0, tail = 0;
	used_apages = 0;	/* zap warning */
	for (j = 0; j < n_apages; j++) {
		blk_t e;
		e = SPAD2CPU64_LV(&apage_index[j].end_sector);
		if (!e) {
			if (__unlikely(!j)) {
				log_printf(0, "APAGE INDEX %u DOESN'T COVER ANY ALLOCATED SPACE", c_index);
				goto apage_index_invalid;
			}
			tail = 1;
		} else {
			used_apages = j + 1;
			if (__unlikely(tail)) {
				log_printf(0, "APAGE INDEX %u CONTAINS ALLOCATED ENTRY AT %u AFTER FREE ENTRY", c_index, j);
				goto apage_index_invalid;
			}
			if (__unlikely(e <= fb)) {
				log_printf(0, "APAGE INDEX %u CONTAINS ENTRY %u POINTING BACKWARDS (%"blk_format"X <= %"blk_format"X)", c_index, j, e, fb);
				goto apage_index_invalid;
			}
			if (__unlikely((unsigned)e & ((1 << super.sectors_per_block_bits) - 1))) {
				log_printf(0, "APAGE INDEX %u CONTAINS UNALIGNED ENTRY %u (%"blk_format"X)", c_index, j, e);
				goto apage_index_invalid;
			}
			if (__unlikely(e - fb < MIN_APAGE_SECTORS(super.sectors_per_page_bits + SSIZE_BITS, super.sectors_per_block_bits + SSIZE_BITS))) {
				if (__unlikely(j == n_apages - 1) || __likely(!SPAD2CPU64_LV(&apage_index[j + 1].end_sector))) goto last_can_be_incomplete;
				log_printf(0, "APAGE INDEX %u CONTAINS TOO SMALL ENTRY %u POINTING FROM %"blk_format"X TO %"blk_format"X (MINIMUM IS %"blk_format"X)", c_index, j, fb, e, (blk_t)MIN_APAGE_SECTORS(super.sectors_per_page_bits + SSIZE_BITS, super.sectors_per_block_bits + SSIZE_BITS));
				goto apage_index_invalid;
				last_can_be_incomplete:;
			}
			fb = e;
		}
	}
	if (__unlikely(fb != SPAD2CPU64_LV(&super.size))) {
		log_printf(0, "APAGE INDEX %u DOESN'T END AT FILESYSTEM END (%"blk_format"X != %"blk_format"X)", c_index, fb, SPAD2CPU64_LV(&super.size));
		goto apage_index_invalid;
	}

	if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.apage_index[0]), apage_index_sectors))) {
		if (r == 2) return 2;
		log_printf(0, "APAGE INDEX 0 ALLOCATION ERROR: %s", alloc_error);
		goto apage_index_invalid;
	}

	if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.apage_index[1]), apage_index_sectors))) {
		if (free_blocks(SPAD2CPU64_LV(&super.apage_index[0]), apage_index_sectors) == 2) return 2;
		if (r == 2) return 2;
		log_printf(0, "APAGE INDEX 1 ALLOCATION ERROR: %s", alloc_error);
		goto apage_index_invalid;
	}

	status_printf(0, "checking apages...");

	for (pfj = j = n_apages - 1; j >= 0; j--) {
		int buffer_flags = !SPAD2CPU64_LV(&apage_index[j].end_sector) ? BUFFER_FREE_SOON : 0;
		struct apage_head *a;
		blk_t sec;
		int do_pf;
		print_progress(n_apages + used_apages, n_apages - 1 - j, 1);
		sec = SPAD2CPU64_LV(&apage_index[j].apage);
		do_pf = 0;
		while (pfj >= 0 && (SPAD2CPU64_LV(&apage_index[pfj].apage) & -(blk_t)SECTORS_PER_BUFFER) == (sec & -(blk_t)SECTORS_PER_BUFFER))
			pfj--, do_pf = 1;
		if (pfj >= 0 && do_pf)
			prefetch_buffer(SPAD2CPU64_LV(&apage_index[pfj].apage), buffer_flags);
		if (__unlikely(!(a = read_buffer(sec, 1 << super.sectors_per_page_bits, buffer_flags)))) {
			read_err_1:
			log_printf(0, "ERROR READING APAGE %u (%"blk_format"X): %s", j, sec, buffer_error);
			goto free_apage_index;
		}
		if (__unlikely(a->magic != CPU2SPAD16(APAGE_MAGIC))) {
			log_printf(0, "BAD MAGIC ON APAGE %u (%"blk_format"X): %04X", j, sec, (unsigned)SPAD2CPU16_LV(&a->magic));
			goto free_apage_index;
		}
		if (__unlikely(cc_check(&a->cc, &a->txc, "APAGE", NULL, NULL, sec))) {
			write_buffer(sec, 1 << super.sectors_per_page_bits, a);
			if (__unlikely(!(a = read_buffer(sec, 1 << super.sectors_per_page_bits, buffer_flags)))) goto read_err_1;
		}
		if (!cc_valid(a->cc, a->txc)) {
			swapspace[j] = sec;
		} else {
			swapspace[j] = sec + (1 << (super.sectors_per_page_bits - 1));
		}
		if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&apage_index[j].apage), 1 << super.sectors_per_page_bits))) {
			int jj;
			if (r == 2) return 2;
			log_printf(0, "APAGE %u ALLOCATION ERROR: %s", j, alloc_error);
			free_apage_index:
			for (jj = j + 1; jj < n_apages; jj++) if (free_blocks(SPAD2CPU64_LV(&apage_index[jj].apage), 1 << super.sectors_per_page_bits) == 2) return 2;
			if (free_blocks(SPAD2CPU64_LV(&super.apage_index[0]), apage_index_sectors) == 2) return 2;
			if (free_blocks(SPAD2CPU64_LV(&super.apage_index[1]), apage_index_sectors) == 2) return 2;
			goto apage_index_invalid;
		}
	}

	apage_index_valid = 1;

	apage_checks = 0;
	apage_err = 0;

#define apage_error(msg)						\
do {									\
	if (apage_err < MAX_SAME_ERRORS) {				\
		log_printf msg;						\
		if (++apage_err == MAX_SAME_ERRORS) log_suppress();	\
	}								\
	if (map) mem_free(map), map = NULL;				\
	goto cont;							\
} while (0)

	freeb = 0;
	/*for (j = 0; j < used_apages; j++)*/
	for (j = used_apages - 1; j >= 0; j--) {
		blk_t sec = SPAD2CPU64_LV(&apage_index[j].apage);
		blk_t end = SPAD2CPU64_LV(&apage_index[j].end_sector);
		char *map = NULL;
		struct apage_head *a;
#define aa(i) (*(struct aentry *)((char *)a + (i)))
		blk_t start;
		unsigned asize;
		print_progress(n_apages + used_apages, n_apages + used_apages - j - 1, 1);
		if (__unlikely(!(a = read_buffer(sec, 1 << super.sectors_per_page_bits, 0)))) {
			log_printf(1, "ERROR READING APAGE %u (%"blk_format"X): %s", j, sec, buffer_error);
			log_printf(1, "THE DEVICE IS UNRELIABLE, APAGE HAS BEEN SUCCESSFULY READ BEFORE");
			return 1;
		}
		if (__unlikely(a->magic != CPU2SPAD16(APAGE_MAGIC))) {
			log_printf(1, "BAD MAGIC ON APAGE %u (%"blk_format"X): %04X", j, sec, (unsigned)SPAD2CPU16_LV(&a->magic));
			log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
			return 1;
		}
		if (!cc_valid(a->cc, a->txc)) {
			a = (struct apage_head *)((char *)a + (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)));
			sec += 1 << (super.sectors_per_page_bits - 1);
		}
		start = j ? SPAD2CPU64_LV(&apage_index[j - 1].end_sector) : 0;
		if (a->s.u.l.flags & APAGE_CHECKSUM_VALID) {
			__u8 chs = __byte_sum(&a->s, (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)) - (sizeof(struct apage_head) - sizeof(struct apage_subhead))) ^ CHECKSUM_BASE;
			if (__unlikely(chs)) {
				if (apage_checks < MAX_SAME_ERRORS) {
					log_printf(0, "BAD CHECKSUM ON APAGE %u (%"blk_format"X): %02X", j, sec, chs);
					if (++apage_checks == MAX_SAME_ERRORS) log_suppress();
				}
				goto cont;
			}
		}
		if (__unlikely((asize = APAGE_SIZE(a->s.u.l.flags)) != (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits))) || __unlikely(APAGE_SECTORS_PER_BLOCK_BITS(a->s.u.l.flags) != super.sectors_per_block_bits)) {
			apage_error((0, "APAGE %u (%"blk_format"X) MISREPORTS SIZE (%u != %u) OR BLOCKSIZE (%u != %u)", j, sec, APAGE_SIZE(a->s.u.l.flags), (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)), APAGE_SECTORS_PER_BLOCK_BITS(a->s.u.l.flags), super.sectors_per_block_bits));
		}
		if (__unlikely(a->s.u.l.flags & APAGE_BITMAP)) {
			unsigned bmpsize = BITMAP_SIZE(asize);
			unsigned es;
			unsigned k;
			blk_t run_start;
			if (__unlikely(end - start > bmpsize << super.sectors_per_block_bits)) {
				apage_error((0, "APAGE %u (%"blk_format"X) IS BITMAP AND MAPS TOO LARGE SPACE (%"blk_format"X > %X)", j, sec, end - start, bmpsize << super.sectors_per_block_bits));
			}
			if (__unlikely(MAKE_D_OFF(a->s.u.b.start0, a->s.u.b.start1) != start)) {
				apage_error((0, "APAGE %u (%"blk_format"X) IS BITMAP AND MISREPORTS ITS START (%"blk_format"X != %"blk_format"X)", j, sec, MAKE_D_OFF(a->s.u.b.start0, a->s.u.b.start1), start));
			}
			es = (end - start) >> super.sectors_per_block_bits;

			run_start = 0;
			for (k = 0; k < es; k++) {
				int bt;
				if (!(bt = BITMAP_TEST(a, k))) {
					freeb += 1 << super.sectors_per_block_bits;
					if (!run_start) run_start = start + (k << super.sectors_per_block_bits);
				} else {
					if (run_start) {
						suggest_free_space(run_start, start + (k << super.sectors_per_block_bits) - run_start);
						run_start = 0;
					}
				}
			}
			if (run_start) suggest_free_space(run_start, start + (k << super.sectors_per_block_bits) - run_start);

			for (; k < bmpsize; k++) if (__unlikely(!BITMAP_TEST(a, k))) {
				apage_error((0, "APAGE %u (%"blk_format"X) IS BITMAP AND HAS FREE BIT (%X) OUT OF ITS SPACE (%X)", j, sec, k, (unsigned)(end - start)));
			}
		} else {
			unsigned list, prev, next, k;
			blk_t last_blk;
			map = mem_alloc(asize / sizeof(struct aentry));
			if (__unlikely(!map)) {
				log_printf(1, "CAN'T ALLOC MEMORY FOR APAGE MAP (%lu BYTES)", (unsigned long)asize / sizeof(struct aentry));
				return 2;
			}
			memset(map, 0, asize / sizeof(struct aentry));
			map[0] = 1;
			list = SPAD2CPU16_LV(&a->s.u.l.freelist);
			while (list) {
				if (__unlikely(APTR_INVALID(list, asize))) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS INVALID ENTRY IN FREELIST: %04X", j, sec, list));
				}
				if (__unlikely(map[list / sizeof(struct aentry)])) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS CROSS LINKED FREELIST: %04X", j, sec, list));
				}
				map[list / sizeof(struct aentry)] = 1;
				if (__unlikely((SPAD2CPU64_LV(&aa(list).start) | (SPAD2CPU32_LV(&aa(list).len) | SPAD2CPU16_LV(&aa(list).prev))) != 0)) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS NON-EMPTY ENTRY IN FREELIST: %04X", j, sec, list));
				}
				list = SPAD2CPU16_LV(&aa(list).next);
			}
			last_blk = start;
			list = 0;
			do {
				prev = SPAD2CPU16_LV(&aa(list).prev);
				next = SPAD2CPU16_LV(&aa(list).next);
				if (__unlikely(APTR_INVALID(prev, asize))) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS BAD PREV POINTER IN LIST: %04X -> %04X -> %04X", j, sec, prev, list, next));
				}
				if (__unlikely(APTR_INVALID(next, asize))) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS BAD NEXT POINTER IN LIST: %04X -> %04X -> %04X", j, sec, prev, list, next));
				}
				if (__unlikely(SPAD2CPU16_LV(&aa(prev).next) != list)) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS INCONSISTENT PREV POINTER IN LIST: %04x -> %04x -> %04X", j, sec, prev, list, next));
				}
				if (__unlikely(SPAD2CPU16_LV(&aa(next).prev) != list)) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS INCONSISTENT NEXT POINTER IN LIST: %04x -> %04x -> %04X", j, sec, prev, list, next));
				}
				if (__likely(list)) {
					if (__unlikely(map[list / sizeof(struct aentry)])) {
						apage_error((0, "APAGE %u (%"blk_format"X) HAS CROSS LINKED LIST: %04X", j, sec, list));
					}
					map[list / sizeof(struct aentry)] = 1;
					if (__unlikely(aa(list).len == CPU2SPAD32(0))) {
						apage_error((0, "APAGE %u (%"blk_format"X) HAS ZERO-LENGHT ENTRY %04X ON LIST", j, sec, list));
					}
					if (__unlikely(SPAD2CPU64_LV(&aa(list).start) < start) || __unlikely(SPAD2CPU64_LV(&aa(list).start) >= end) || __unlikely(SPAD2CPU64_LV(&aa(list).start) + SPAD2CPU32_LV(&aa(list).len) > end)) {
						apage_error((0, "APAGE %u (%"blk_format"X) HAS ENTRY %04X (%"blk_format"X - %"blk_format"X) OUT OF BORDERS (%"blk_format"X - %"blk_format"X)", j, sec, list, SPAD2CPU64_LV(&aa(list).start), SPAD2CPU64_LV(&aa(list).start) + SPAD2CPU32_LV(&aa(list).len), start, end));
					}
					if (__unlikely(((unsigned)SPAD2CPU64_LV(&aa(list).start) | SPAD2CPU32_LV(&aa(list).len)) & ((1 << super.sectors_per_block_bits) - 1))) {
						apage_error((0, "APAGE %u (%"blk_format"X) HAS UNALIGNED ENTRY %04X (%"blk_format"X - %"blk_format"X)", j, sec, list, SPAD2CPU64_LV(&aa(list).start), SPAD2CPU64_LV(&aa(list).start) + SPAD2CPU32_LV(&aa(list).len)));
					}
					if (__unlikely(SPAD2CPU64_LV(&aa(list).start) < last_blk)) {
						apage_error((0, "APAGE %u (%"blk_format"X) HAS NON-MONOTONIC ENTRY %04X (%"blk_format"X < %"blk_format"X)", j, sec, list, SPAD2CPU64_LV(&aa(list).start), last_blk));
					}
					suggest_free_space(SPAD2CPU64_LV(&aa(list).start), SPAD2CPU32_LV(&aa(list).len));
					last_blk = SPAD2CPU64_LV(&aa(list).start) + SPAD2CPU32_LV(&aa(list).len);
					freeb += SPAD2CPU32_LV(&aa(list).len);
				}
			} while ((list = next));
			for (k = 0; k < asize; k += sizeof(struct aentry)) {
				if (__unlikely(!map[k / sizeof(struct aentry)])) {
					apage_error((0, "APAGE %u (%"blk_format"X) HAS LOST ENTRY %04X", j, sec, k));
				}
			}
			mem_free(map), map = NULL;
		}
		cont:;
	}

	if (__likely(freeb < total_sectors_allocated)) total_sectors_allocated -= freeb;

#ifdef TESTCODE
	if (!always_swap)
#endif
	qsort(swapspace, n_apages, sizeof(blk_t), blk_cmp);
	allow_swapping = 1;

	apages_valid = !(apage_checks | apage_err);

	apage_index_invalid:

	if (!apages_valid && preen && !extend && !(SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_DIRTY)) {
		log_printf(1, "CAN'T REPAIR APAGES IN AUTOMATIC MODE, RUN FSCK MANUALLY");
		return 1;
	}

	return 0;
}

#undef apage_error
#undef aa

int recover_apage_allocs(void)
{
	if (apage_index_valid) {
		int r;
		int j;
		if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.apage_index[0]), apage_index_sectors))) {
			if (r == 1) log_printf(1, "FAILED TO RECOVER APAGE INDEX 0 ALLOCATION: %s", alloc_error);
			return 2;
		}
		if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&super.apage_index[1]), apage_index_sectors))) {
			if (r == 1) log_printf(1, "FAILED TO RECOVER APAGE INDEX 1 ALLOCATION: %s", alloc_error);
			return 2;
		}
		for (j = 0; j < n_apages; j++) {
			if (__unlikely(r = alloc_blocks(SPAD2CPU64_LV(&apage_index[j].apage), 1 << super.sectors_per_page_bits))) {
				if (r == 1) log_printf(1, "FAILED TO RECOVER APAGE %u ALLOCATION: %s", j, alloc_error);
				return 2;
			}
		}
	}
	return 0;
}

static int aidx_cmp(const void *p1, const void *p2)
{
	const struct apage_index_entry *e1 = p1;
	const struct apage_index_entry *e2 = p2;
	if (__unlikely(SPAD2CPU64_LV(&e1->apage) > SPAD2CPU64_LV(&super.cct))) {
		if (SPAD2CPU64_LV(&e2->apage) < SPAD2CPU64_LV(&super.cct)) return 1;
		if (SPAD2CPU64_LV(&e1->apage) < SPAD2CPU64_LV(&e2->apage)) return -1;
		if (__likely(SPAD2CPU64_LV(&e1->apage) > SPAD2CPU64_LV(&e2->apage))) return 1;
		return 0;
	}
	if (__unlikely(SPAD2CPU64_LV(&e2->apage) > SPAD2CPU64_LV(&super.cct))) return -1;
	if (SPAD2CPU64_LV(&e1->apage) > SPAD2CPU64_LV(&e2->apage)) return -1;
	if (__likely(SPAD2CPU64_LV(&e1->apage) < SPAD2CPU64_LV(&e2->apage))) return 1;
	return 0;
}

static int create_apage(blk_t sec, struct apage_head **aa1, struct apage_head **aa2)
{
	int allocated;
	int i;
	struct aentry *ae;
	struct apage_head *a, *a2;
	unsigned a2_off;
	allocated = 0;
	if (__unlikely(!ptr_valid(sec, 1 << super.sectors_per_page_bits)) ||
	    __unlikely(!(a = read_buffer(sec, 1 << super.sectors_per_page_bits, 0)))) {
		do_alloc:
		if (swap_allocmap) {
			log_printf(1, "UNABLE TO READ APAGE WHILE SWAPPING TO THEM");
			return 2;
		}
		a = dummy_buffer();
		memset(a, 0, 1 << super.sectors_per_page_bits);
		allocated = 1;
	}
	if (__likely(a->magic == CPU2SPAD16(APAGE_MAGIC)) && !cc_valid(a->cc, a->txc)) {
		CPU2SPAD16_LV(&a->magic, APAGE_MAGIC);
		CPU2SPAD16_LV(&a->cc, 0);
		CPU2SPAD32_LV(&a->txc, 0x80000000U);
		a2_off = 1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits);
	} else {
		CPU2SPAD16_LV(&a->magic, APAGE_MAGIC);
		CPU2SPAD16_LV(&a->cc, 0);
		CPU2SPAD32_LV(&a->txc, 0);
		a2_off = 0;
	}
	write_buffer(sec, !allocated ? 1 : 1 << super.sectors_per_page_bits, a);
	if (!allocated) {
		if (__unlikely(!(a = read_buffer(sec, 1 << super.sectors_per_page_bits, 0)))) {
			goto do_alloc;
		}
	}
	a2 = (struct apage_head *)((char *)a + a2_off);
	*aa1 = a;
	*aa2 = a2;
	a2->s.u.l.flags = ((super.sectors_per_page_bits - 1) << APAGE_SIZE_BITS_SHIFT) | (super.sectors_per_block_bits << APAGE_BLOCKSIZE_BITS_SHIFT);
	a2->s.u.l.checksum = 0;
	CPU2SPAD16_LV(&a2->s.u.l.first, 0);
	CPU2SPAD16_LV(&a2->s.u.l.last, 0);
	CPU2SPAD16_LV(&a2->s.u.l.freelist, sizeof(struct aentry));
	ae = NULL;	/* warning, go away */
	for (i = sizeof(struct aentry); i < 1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits); i += sizeof(struct aentry)) {
		ae = (struct aentry *)((char *)a2 + i);
		memset(ae, 0, sizeof(struct aentry));
		CPU2SPAD16_LV(&ae->next, i + sizeof(struct aentry));
	}
	CPU2SPAD16_LV(&ae->next, 0);
	return 0;
}

static int write_apage(blk_t sec, struct apage_head *a1, struct apage_head *a2)
{
	a2->s.u.l.flags |= APAGE_CHECKSUM_VALID;
	a2->s.u.l.checksum ^= CHECKSUM_BASE ^ __byte_sum(&a2->s, (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)) - (sizeof(struct apage_head) - sizeof(struct apage_subhead)));
	write_buffer(sec + (((unsigned long)a2 - (unsigned long)a1) >> SSIZE_BITS), 1 << (super.sectors_per_page_bits - 1), a2);
	return 0;
}

static void print_lost_alloc(blk_t start, blk_t end)
{
	log_printf(0, "LOST RUN AT %"blk_format"X, LENGTH %"blk_format"X", start, end - start);
}

static void print_usedfree_alloc(blk_t start, blk_t end)
{
	log_printf(0, "USED RUN AT %"blk_format"X, LENGTH %"blk_format"X", start, end - start);
}

static int claim_allocation(blk_t start, blk_t end, int is_allocated, blk_t *lost, blk_t *usedfree, blk_t *lost_runs, blk_t *usedfree_runs, int *flag, void (*claim_callback)(blk_t start, blk_t end), void (*usedfree_callback)(blk_t start, blk_t end))
{
	while (start < end) {
		blk_t e;
		int r;
		if (__unlikely(r = get_blockrun(start, is_allocated, &e))) return r;
		if (__likely(e != start)) *flag = 0;
		if (__likely(e >= end)) return 0;
		start = e;
		if (__unlikely(r = get_blockrun(start, !is_allocated, &e))) return r;
		if (e > end) e = end;
		if (is_allocated) {
			*lost += e - start;
			if (*flag != 1) (*lost_runs)++;
			*flag = 1;
			if (claim_callback) claim_callback(start, e);
		} else {
			*usedfree += e - start;
			if (*flag != -1) (*usedfree_runs)++;
			*flag = -1;
			if (usedfree_callback) usedfree_callback(start, e);
		}
		start = e;
	}
	return 0;
}

#define apage_error(msg)						\
do {									\
	log_printf msg;							\
	log_printf(1, "THE DEVICE IS UNRELIABLE, APAGES HAVE BEEN CORRECT BEFORE");\
	return 1;							\
} while (0)

static int compare_apages(blk_t *lost, blk_t *usedfree, blk_t *lost_runs, blk_t *usedfree_runs, void (*claim_callback)(blk_t start, blk_t end), void (*usedfree_callback)(blk_t start, blk_t end), void (*flush_callback)(void))
{
	int r;
	int j, k;
	blk_t start = 0;
	int flag = 0;
	*lost = 0, *usedfree = 0;
	*lost_runs = 0, *usedfree_runs = 0;
	for (k = 0; k < n_apages && SPAD2CPU64_LV(&apage_index[k].end_sector); k++) ;
	for (j = 0; j < k; j++) {
		blk_t sec, end;
		struct apage_head *a;
		if (!flush_callback) print_progress(k, j, 1);
		sec = SPAD2CPU64_LV(&apage_index[j].apage);
		end = SPAD2CPU64_LV(&apage_index[j].end_sector);
		if (__unlikely(!(a = read_buffer(sec, 1 << super.sectors_per_page_bits, 0)))) {
			apage_error((0, "ERROR READING APAGE %u (%"blk_format"X): %s", j, sec, buffer_error));
		}
		if (__unlikely(a->magic != CPU2SPAD16(APAGE_MAGIC))) {
			apage_error((0, "BAD MAGIC ON APAGE %u (%"blk_format"X): %04X", j, sec, (unsigned)SPAD2CPU16_LV(&a->magic)));
		}
		if (!cc_valid(a->cc, a->txc)) {
			a = (struct apage_head *)((char *)a + (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)));
			sec += 1 << (super.sectors_per_page_bits - 1);
		}
		if (__unlikely(a->s.u.l.flags & APAGE_BITMAP)) {
			unsigned bmpsize = BITMAP_SIZE(1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits));
			unsigned bmpl, k;
			if (__unlikely(end - start > bmpsize << super.sectors_per_block_bits))
				apage_error((0, "APAGE %u (%"blk_format"X) IS BITMAP AND MAPS TOO LARGE SPACE (%"blk_format"X > %X)", j, sec, end - start, bmpsize << super.sectors_per_block_bits));
			bmpl = (end - start) >> super.sectors_per_block_bits;
			k = 0;
			while (k < bmpl) {
				unsigned l;
				for (l = k; l < bmpl && BITMAP_TEST(a, l); l++) ;
				if (__unlikely(r = claim_allocation(start + (k << super.sectors_per_block_bits), start + (l << super.sectors_per_block_bits), 1, lost, usedfree, lost_runs, usedfree_runs, &flag, claim_callback, usedfree_callback))) return r;
				k = l;
				for (l = k; l < bmpl && !BITMAP_TEST(a, l); l++) ;
				if (__unlikely(r = claim_allocation(start + (k << super.sectors_per_block_bits), start + (l << super.sectors_per_block_bits), 0, lost, usedfree, lost_runs, usedfree_runs, &flag, claim_callback, usedfree_callback))) return r;
				k = l;
			}
			start = end;
		} else {
#define aa(i) (*(struct aentry *)((char *)a + (i)))
			unsigned asize = 1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits);
			unsigned list = SPAD2CPU16_LV(&aa(0).next);
			while (list) {
				blk_t b;
				__u32 l;
				if (__unlikely(APTR_INVALID(list, asize)))
					apage_error((0, "APAGE %u (%"blk_format"X) HAS BAD POINTER IN LIST: %04X", j, sec, list));
				b = SPAD2CPU64_LV(&aa(list).start);
				l = SPAD2CPU32_LV(&aa(list).len);
				if (__unlikely(aa(list).len == CPU2SPAD32(0)))
					apage_error((0, "APAGE %u (%"blk_format"X) HAS ZERO-LENGHT ENTRY %04X ON LIST", j, sec, list));
				if (__unlikely(b < start))
					apage_error((0, "APAGE %u (%"blk_format"X) HAS NON-MONOTONIC ENTRY %04X (%"blk_format"X < %"blk_format"X)", j, sec, list, SPAD2CPU64_LV(&aa(list).start), start));
				if (__unlikely(b + l > end) || __unlikely(b + l < start))
					apage_error((0, "APAGE %u (%"blk_format"X) HAS ENTRY %04X OUT OF BORDERS", j, sec, list));
				if (__unlikely(r = claim_allocation(start, b, 1, lost, usedfree, lost_runs, usedfree_runs, &flag, claim_callback, usedfree_callback))) return r;
				if (__unlikely(r = claim_allocation(b, b + l, 0, lost, usedfree, lost_runs, usedfree_runs, &flag, claim_callback, usedfree_callback))) return r;
				start = b + l;
				list = SPAD2CPU16_LV(&aa(list).next);
			}
			if (__unlikely(r = claim_allocation(start, end, 1, lost, usedfree, lost_runs, usedfree_runs, &flag, claim_callback, usedfree_callback))) return r;
			start = end;
#undef aa
		}
		if (flush_callback) flush_callback();
	}
	return 0;
}

static int scan_unallocated(blk_t limit, blk_t *n_sectors, void (*claim_callback)(blk_t start, blk_t end), void (*flush_callback)(void))
{
	int r;
	blk_t s, s_e;

	*n_sectors = 0;
	s = 0;
	while (s < limit) {
		if (__unlikely(r = get_blockrun(s, 0, &s_e))) return r;
		if (s_e > limit) s_e = limit;
		if (s_e > s) {
			*n_sectors += s_e - s;
			if (claim_callback) claim_callback(s, s_e);
			if (flush_callback) flush_callback();
		}
		s = s_e;
		if (__unlikely(r = get_blockrun(s, 1, &s_e))) return r;
		s = s_e;
	}
	return 0;
}

int lost_scan(void)
{
	int r;
	if (apages_valid && !undelete) {
		blk_t lost, usedfree;
		blk_t lost_runs, usedfree_runs;
		status_printf(0, "checking for lost data...");
		if (__unlikely(r = compare_apages(&lost, &usedfree, &lost_runs, &usedfree_runs, NULL, NULL, NULL))) return r;

		if (!lost && !usedfree) allocation_map_ok = 1;

		if (!lost) return 0;

		log_printf(0, "FILESYSTEM HAS %"blk_format"d LOST SECTORS IN %"blk_format"d RUNS", lost, lost_runs);
		if (!query(qprefix Q_NOFIX, "SCAN?")) return 0;

		status_printf(0, "scanning lost data...");
		if (__unlikely(r = mass_scan_prepare(lost))) return r;
		if (__unlikely(r = compare_apages(&lost, &usedfree, &lost_runs, &usedfree_runs, mass_scan_set_range, NULL, mass_scan_do))) return r;
		mass_scan_sort();
	} else {
		blk_t limit;
		blk_t n_sectors;
		if (!undelete && (extend || (preen && SPAD2CPU32_LV(&tx.txflags) & TXFLAGS_DIRTY))) return 0;
		if (!undelete && !query(qprefix Q_NOFIX, "SCAN FOR LOST METADATA?")) return 0;
		limit = (blk_t)SPAD2CPU16_LV(&super.small_file_group) << super.sectors_per_group_bits;
		if (!limit || limit >= SPAD2CPU64_LV(&super.size))
			goto scan_all;
		if (undelete) {
			if (undelete_scan_all) goto scan_all;
			else goto scan_meta_only;
		}
		if (query(qprefix Q_NOFIX, "SCAN DATA ZONE TOO?"))
			scan_all: limit = SPAD2CPU64_LV(&super.size);
		scan_meta_only:
		status_printf(0, "scanning the device...");
		if (__unlikely(r = scan_unallocated(limit, &n_sectors, NULL, NULL))) return r;
		if (__unlikely(r = mass_scan_prepare(n_sectors))) return r;
		if (__unlikely(r = scan_unallocated(limit, &n_sectors, mass_scan_set_range, mass_scan_do))) return r;
		mass_scan_sort();
	}
	return 0;
}

int fix_apages(void)
{
	int rebuild_msg = 0;
	int i;
	int r;
	blk_t blk, b;
	struct apage_head *a1, *a2;
	int remaining_entries;
	if (apages_valid) {
		blk_t lost, usedfree;
		blk_t lost_runs, usedfree_runs;

		if (!rebuild_apages && allocation_map_ok) return 0;

		status_printf(0, "checking apages again...");
		r = compare_apages(&lost, &usedfree, &lost_runs, &usedfree_runs, verbose ? print_lost_alloc : NULL, verbose ? print_usedfree_alloc : NULL, NULL);
		if (__unlikely(r)) return r;
		if (__unlikely((lost | usedfree) != 0)) {
			if (lost != 0) log_printf(0, "FILESYSTEM HAS %"blk_format"d LOST SECTORS IN %"blk_format"d RUNS", lost, lost_runs);
			if (usedfree != 0) log_printf(0, "FILESYSTEM HAS %"blk_format"d USED SECTORS MARKED AS FREE IN %"blk_format"d RUNS", usedfree, usedfree_runs);
			if (newalloc != 0) log_printf(0, "%"blk_format"d SECTORS WERE NEWLY ALLOCATED", newalloc);
			goto rebuild;
		}
		if (__unlikely(rebuild_apages)) {
			goto do_rebuild;
		}
		return 0;
	} else {
		if (!ro) status_printf(0, "rebuilding apages..."), rebuild_msg = 1;
	}
	rebuild:
	if (!extend) {
		if (!query(qprefix 0, !apage_index_valid ? "ALLOC NEW APAGES?" : "REBUILD APAGES?")) return 0;
	}
	do_rebuild:
	if (!rebuild_msg) status_printf(0, "rebuilding apages..."), rebuild_msg = 1;
	if (!apage_index_valid) {
		if (__unlikely((r = alloc_try(SPAD2CPU64_LV(&super.apage_index[0]), apage_index_sectors)))) {
			blk_t idx_sec_1;
			if (r > 0) return r;
			if (__unlikely((r = alloc_space(1, "APAGE INDEX 0", apage_index_sectors, 0, 1, &idx_sec_1)))) return r;
			CPU2SPAD64_LV(&super.apage_index[0], idx_sec_1);
		}
		if (__unlikely((r = alloc_try(SPAD2CPU64_LV(&super.apage_index[1]), apage_index_sectors)))) {
			blk_t idx_sec_2;
			if (r > 0) return r;
			if (__unlikely((r = alloc_space(1, "APAGE INDEX 1", apage_index_sectors, 0, 1, &idx_sec_2)))) return r;
			CPU2SPAD64_LV(&super.apage_index[1], idx_sec_2);
		}
		memset(apage_index, 0, (unsigned long)apage_index_sectors << SSIZE_BITS);
		for (i = 0; i < n_apages; i++) {
			blk_t new_ap;
			if (__unlikely((r = alloc_space(1, "APAGE", 1 << super.sectors_per_page_bits, (1 << super.sectors_per_page_bits) - 1, 1, &new_ap)))) return r;
			CPU2SPAD64_LV(&apage_index[i].apage, new_ap);
		}
		write_super();
	}
	qsort(apage_index, n_apages, sizeof(struct apage_index_entry), aidx_cmp);
	i = 0;
	blk = b = 0;
	a1 = NULL; /* against gcc 4.1 warning */
	a2 = NULL;
	remaining_entries = 0;
	while (1) {
		blk_t b_e;
		int f;
		struct aentry *ae, *ael;
		print_progress(n_apages, i, 1);
		if (__unlikely(!a2)) {
			if (__unlikely(i >= n_apages)) panic("fix_apages: all %u apages exhauseted", n_apages);
			if (__unlikely(r = create_apage(SPAD2CPU64_LV(&apage_index[i].apage), &a1, &a2))) return r;
			remaining_entries = (1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)) / sizeof(struct aentry) - 1;
			remaining_entries -= remaining_entries / FSCK_APAGE_FREE_PERCENTAGE;
		}
		if (__unlikely(r = get_blockrun(b, 0, &b_e))) return r;
		if (__unlikely(b_e > (__u64)b + ((__u64)1 << 32) - (1 << super.sectors_per_block_bits)))
			b_e = b + ((__u64)1 << 32) - (1 << super.sectors_per_block_bits);
		if (__likely(b_e != b)) {
			f = SPAD2CPU16_LV(&a2->s.u.l.freelist);
			if (__unlikely(!f)) panic("fix apages: out of freelist");
			ae = (struct aentry *)((char *)a2 + f);
			a2->s.u.l.freelist = ae->next;
			ael = (struct aentry *)((char *)a2 + SPAD2CPU16_LV(&a2->s.u.l.last));
			CPU2SPAD64_LV(&ae->start, b);
			CPU2SPAD32_LV(&ae->len, b_e - b);
			ae->prev = a2->s.u.l.last;
			CPU2SPAD16_LV(&ae->next, 0);
			CPU2SPAD16_LV(&ael->next, f);
			CPU2SPAD16_LV(&a2->s.u.l.last, f);
			remaining_entries--;
		}
		if (__unlikely(r = get_blockrun(b_e, 1, &b))) return r;
		if (__unlikely(!remaining_entries) || __unlikely(b == SPAD2CPU64_LV(&super.size))) {
			int r;
			if (__unlikely(b - blk < MIN_APAGE_SECTORS(super.sectors_per_page_bits + SSIZE_BITS, super.sectors_per_block_bits + SSIZE_BITS))
#ifdef TESTCODE
			    || make_apage_bitmaps
#endif
			    ) {
				unsigned len = ((1 << (SSIZE_BITS - 1 + super.sectors_per_page_bits)) - sizeof(struct apage_head)) * 8;
				a2->s.u.b.flags |= APAGE_BITMAP;
				a2->s.u.b.start0 = MAKE_PART_0(blk);
				a2->s.u.b.start1 = MAKE_PART_1(blk);
				memset(a2 + 1, 0xff, len / 8);
				b = blk;
				len <<= super.sectors_per_block_bits;
				while (b != SPAD2CPU64_LV(&super.size) && b < blk + len) {
					unsigned idx, idxend;
					if (__unlikely(r = get_blockrun(b, 0, &b_e))) return r;
					if (__unlikely(b_e > blk + len)) b_e = blk + len;
					idx = (b - blk) >> super.sectors_per_block_bits;
					idxend = (b_e - blk) >> super.sectors_per_block_bits;
					while (idx < idxend) {
						BITMAP_CLEAR(a2, idx);
						idx++;
					}
					if (__unlikely(r = get_blockrun(b_e, 1, &b))) return r;
				}
				if (b > blk + len) b = blk + len;
			}
			if (__unlikely((r = write_apage(SPAD2CPU64_LV(&apage_index[i].apage), a1, a2)))) return r;
			blk = b;
			CPU2SPAD64_LV(&apage_index[i].end_sector, blk);
			i++;
			a2 = NULL;
			if (__unlikely(blk == SPAD2CPU64_LV(&super.size))) break;
		}
	}
	while (i < n_apages) {
		print_progress(n_apages, i, 1);
		a1 = dummy_buffer();
		memset(a1, 0, 1 << SSIZE_BITS << super.sectors_per_page_bits);
		CPU2SPAD16_LV(&a1->magic, APAGE_MAGIC);
		write_buffer(SPAD2CPU64_LV(&apage_index[i].apage), swap_allocmap ? 1 : 1 << super.sectors_per_page_bits, a1);
		CPU2SPAD64_LV(&apage_index[i].end_sector, 0);
		i++;
	}
	write_buffer(SPAD2CPU64_LV(&super.apage_index[c_index]), apage_index_sectors, apage_index);
	return 0;
}

#undef apage_error

void done_apages(void)
{
	if (apage_index != NULL) mem_free(apage_index), apage_index = NULL;
	if (swapspace != NULL) mem_free(swapspace), swapspace = NULL;
	allow_swapping = 0;
}
