#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

typedef struct {
	LIST_ENTRY list;
	char *ea;
	unsigned ea_size;
	char *path;
	char filename[1];	/* path is after filename's '\0', path is pointing to it */
} RECOVERY_FILENAME;

struct __recovery {
	LIST_ENTRY list;
	RECOVERY_FILEDESC rfd;
	LIST_HEAD filenames;	/* if more than one RECOVERY_FILENAMEs are in list, hardlinks should be created */
	unsigned ea_size;
	__u64 idx;	/* original index when sorting */
			/* this field is also used to force 64-bit alignment of ea */
	char ea[1];
};

static LIST_HEAD dir_recoveries;
static LIST_HEAD file_recoveries;

void init_recovery(void)
{
	INIT_LIST(&dir_recoveries);
	INIT_LIST(&file_recoveries);
}

int skip_recovery_blocks(blk_t b, blk_t l, blk_t *new_b)
{
	RECOVERY *rec;
	ALLOC *al;
	*new_b = b;
	LIST_FOR_EACH(rec, &file_recoveries, RECOVERY, list) LIST_FOR_EACH(al, &rec->rfd.runs, ALLOC, list) {
		if (b + l > al->start && __unlikely(b < al->start + al->len)) {
			b = al->start + al->len;
		}
	}
	if (__unlikely(b != *new_b)) {
		*new_b = b;
		return 1;
	}
	return 0;
}

void exclude_recovery_blocks(blk_t b, blk_t e, blk_t *new_b, blk_t *new_e)
{
	RECOVERY *rec;
	ALLOC *al;
	LIST_FOR_EACH(rec, &file_recoveries, RECOVERY, list) LIST_FOR_EACH(al, &rec->rfd.runs, ALLOC, list) {
		if (al->start > b && __unlikely(al->start < e))
			e = al->start;
		if (al->start <= b && __unlikely(al->start + al->len > b)) {
			b = al->start + al->len;
			if (__unlikely(b >= e))
				break;
		}
	}
	*new_b = b;
	*new_e = e;
}

int recover_file(RECOVERY_FILEDESC *rfd, const char *file_ea, unsigned file_ea_size, const char *path, const char *filename, const char *name_ea, unsigned name_ea_size, RECOVERY **output)
{
	RECOVERY *rec;
	if (__unlikely(!path)) return 2;	/* can happen if get_filename runs out of memory */
	if (__unlikely(!*path)) path = "/";
	if (__unlikely(!(rec = mem_alloc(sizeof(RECOVERY) - 1 + file_ea_size)))) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR RECOVERY RECORD");
		oom:
		return 2;
	}

	INIT_LIST(&rec->filenames);
	memcpy(&rec->rfd, rfd, sizeof(RECOVERY_FILEDESC));
	rec->ea_size = file_ea_size;
	memcpy(rec->ea, file_ea, file_ea_size);

	if (__unlikely(add_recovery_hardlink(rec, path, filename, name_ea, name_ea_size))) {
		mem_free(rec);
		goto oom;
	}

	if (__likely(!(rec->rfd.flags & FNODE_FLAGS_DIR))) {
		MOVE_LIST_HEAD(&rec->rfd.runs, &rfd->runs);
		ADD_TO_LIST_END(&file_recoveries, &rec->list);
	} else {
		INIT_LIST(&rec->rfd.runs);
		ADD_TO_LIST_END(&dir_recoveries, &rec->list);
	}
	if (output) *output = rec;
	return 0;
}

int add_recovery_hardlink(RECOVERY *rec, const char *path, const char *filename, const char *name_ea, unsigned name_ea_size)
{
	RECOVERY_FILENAME *nam;
	unsigned names_len = sizeof(RECOVERY_FILENAME) - 1 + strlen(path) + 1 + strlen(filename) + 1;
	unsigned names_len_aligned = (names_len + 7) & ~7;
	unsigned total_len = names_len_aligned + name_ea_size;
	if (__unlikely(!(nam = mem_alloc(total_len)))) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR RECOVERY RECORD FILENAME");
		return 2;
	}
	strcpy(nam->filename, filename);
	nam->path = nam->filename + strlen(nam->filename) + 1;
	strcpy(nam->path, path);
	ADD_TO_LIST_END(&rec->filenames, &nam->list);
	nam->ea = (char *)nam + names_len_aligned;
	nam->ea_size = name_ea_size;
	memcpy(nam->ea, name_ea, name_ea_size);
	return 0;
}

static int create_blocks(blk_t lbn, blk_t len, LIST_HEAD *list)
{
	INIT_LIST(list);
	while (len) {
		ALLOC *al;
		blk_t l = len;
		blk_t result;
		int r;
		again:
		if (__unlikely(r = alloc_space(0, NULL, l, 0, 0, &result))) {
			if (__unlikely(r > 0)) return dealloc_and_free_blocklist(r, list);
			l >>= 1;
			l &= ~(blk_t)((1 << super.sectors_per_block_bits) - 1);
			if (__likely(l != 0)) goto again;
			log_printf(0, "NO SPACE FOR RECOVERY OF CROSS LINKED BLOCKS");
			return dealloc_and_free_blocklist(1, list);
		}
		if (__unlikely(!(al = mem_alloc(sizeof(ALLOC))))) {
			log_printf(1, "CAN'T ALLOC MEMORY WHEN REMAPPING CROSS LINKED BLOCKS");
			free_file_runs(list);
			return 2;
		}
		al->start = result;
		al->len = l;
		al->lbn = lbn;
		al->ano = 0;
		ADD_TO_LIST_END(list, &al->list);
		lbn += l;
		len -= l;
	}
	return 0;
}

typedef struct {
	LIST_ENTRY list;
	blk_t lbn;
	blk_t len;
} ZEROED;

static void copy_to_list(__u64 size, blk_t start, LIST_HEAD *dest, LIST_HEAD *zeroed)
{
	int wrote_err = 0;
	ALLOC *al;
	blk_t partial_lbn = size >> SSIZE_BITS;
	blk_t first_unused_lbn = (size + (1U << SSIZE_BITS) - 1) >> SSIZE_BITS;
	LIST_FOR_EACH(al, dest, ALLOC, list) {
		void *b;
		unsigned cnt1 = 0;
		blk_t dpos = al->start;
		blk_t dlen = al->len;
		do {
			blk_t lbn = al->lbn + (dpos - al->start);
			unsigned ll = SECTORS_PER_BUFFER;
			if (start != (blk_t)-1) ll -= (unsigned)start & (SECTORS_PER_BUFFER - 1);
			if (ll > dlen) ll = dlen;
			if (start == (blk_t)-1) goto zeroonly;
			if (lbn >= first_unused_lbn) goto zeroonly;
			if (lbn == partial_lbn) ll = 1;
			else if (lbn + ll > partial_lbn) ll = partial_lbn - lbn;
			try_1:
			if (__unlikely(cnt1)) ll = 1, cnt1--;
			b = read_buffer(start, ll, BUFFER_NOCACHE);
			if (__unlikely(!b)) {
				ZEROED *za;
				void *z;
				if (ll != 1) {
					cnt1 = ll;
					goto try_1;
				}
				if (!wrote_err) log_printf(0, "ERROR READING DATA AT %"blk_format"X, LOGICAL BLOCK %"blk_format"X: %s; ZEROING", start, lbn, buffer_error), wrote_err = 1;
				zeroonly:
				z = dummy_buffer();
				memset(z, 0, ll << SSIZE_BITS);
				write_buffer(dpos, ll, z);
				if (!LIST_EMPTY(zeroed)) {
					za = LIST_STRUCT(zeroed->prev, ZEROED, list);
					if (za->lbn + za->len == al->lbn + al->len - dlen) {
						za->len += ll;
						goto za_ok;
					}
				}
				if (__likely((za = mem_alloc(sizeof(ZEROED))) != NULL)) {
					za->lbn = lbn;
					za->len = ll;
					ADD_TO_LIST(zeroed, &za->list);
				}
				za_ok:;
			} else {
				if (lbn != partial_lbn) {
					write_buffer(dpos, ll, b);
				} else {
					unsigned char data[1U << SSIZE_BITS];
					unsigned offset = size & ((1U << SSIZE_BITS) - 1);
					memcpy(data, b, offset);
					memset(data + offset, 0, (1U << SSIZE_BITS) - offset);
					write_buffer(dpos, ll, data);
				}
			}
			dpos += ll;
			dlen -= ll;
			if (start != (blk_t)-1) start += ll;
		} while (dlen);
	}
}

static int alloc_anode(struct anode **a, blk_t *ano)
{
	int r;
	static struct anode *an;
	if (__unlikely(r = alloc_space(0, "ANODE", 1 << super.sectors_per_block_bits, 0, 0, ano))) return r;
	an = dummy_buffer();
	memset(an, 0, sizeof(struct anode));
	CPU2SPAD32_LV(&an->magic, magic(*ano, ANODE_MAGIC));
	*a = an;
	return 0;
}

static int map_file(RECOVERY *rec, struct fnode *fnode, LIST_HEAD *zeroed)
{
	int r;
	long c, cc;
	int free_all;
	ALLOC *al, *all;
	__u64 size = SPAD2CPU64(fnode->size[!cc_valid(fnode->cc, fnode->txc)]);
	blk_t total_lblocks = (size + (1 << SSIZE_BITS) - 1) >> SSIZE_BITS;
	blk_t new_lblocks;
	blk_t zeroed_lblocks;
	ZEROED *za;
	blk_t last_lbn;
	blk_t blocks;
	INIT_LIST(zeroed);
	LIST_FOR_EACH(al, &rec->rfd.runs, ALLOC, list) {
		al->ano = 0;
	}
	c = 0;
	free_all = 0;
	LIST_FOR_EACH(al, &rec->rfd.runs, ALLOC, list) {
		blk_t splitlen;
		blk_t b_e;
		again:
		if (free_all) goto skip_this;
		if (al->list.prev == (LIST_ENTRY *)(void *)&rec->rfd.runs) last_lbn = 0;
		else {
			all = LIST_STRUCT(al->list.prev, ALLOC, list);
			last_lbn = all->lbn + all->len;
		}
		if (__unlikely(al->lbn < last_lbn)) {
			blk_t over = last_lbn - al->lbn;
			if (over >= al->len) goto skip_this;
			al->start += over;
			al->len -= over;
			al->lbn += over;
		}
		if (__unlikely(al->start >= SPAD2CPU64_LV(&super.size))) {
			skip_this:
			all = LIST_STRUCT(al->list.prev, ALLOC, list);
			DEL_FROM_LIST(&al->list);
			mem_free(al);
			al = all;
			continue;
		}
		if (__unlikely(al->start + al->len > SPAD2CPU64_LV(&super.size))) {
			al->len = SPAD2CPU64_LV(&super.size) - al->start;
		}
		if (__unlikely(al->lbn & ((1 << super.sectors_per_block_bits) - 1))) {
			unsigned add = (1 << super.sectors_per_block_bits) - ((unsigned)al->lbn & ((1 << super.sectors_per_block_bits) - 1));
			if (add >= al->len) goto skip_this;
			al->lbn += add;
			al->start += add;
			al->len -= add;
		}
		if (__unlikely(al->len & ((1 << super.sectors_per_block_bits) - 1))) {
			al->len &= ~(blk_t)((1 << super.sectors_per_block_bits) - 1);
			if (!al->len) goto skip_this;
		}
		if (__unlikely(al->lbn != last_lbn)) {
			LIST_HEAD newlist;
			if (__unlikely(r = create_blocks(last_lbn, al->lbn - last_lbn, &newlist))) {
				if (r > 1) {
					free_file_runs(&newlist);
					return r;
				}
				free_all = 1;
				goto again;
			}
			copy_to_list(size, (blk_t)-1, &newlist, zeroed);
			LIST_FOR_EACH(all, &newlist, ALLOC, list) {
				free_blocks(all->start, all->len);
			}
			all = LIST_STRUCT(al->list.prev, ALLOC, list);
			LIST_JOIN_END((LIST_HEAD *)(void *)&al->list, &newlist);
			al = all;
			continue;
		}
		if (__unlikely(check_allocation(al->start, al->len)))
			goto copy_it;
		if (check_last_block(size, al, NULL, NULL))
			goto copy_it;
#ifdef TESTCODE
		if (__unlikely(move_recovered_files)) goto copy_it;
#endif
		if (__unlikely(r = get_blockrun(al->start, 0, &b_e))) return r;
		if (__unlikely(b_e < al->start + al->len)) {
			LIST_ENTRY *le;
			LIST_HEAD newlist;
			if (b_e != al->start) {
				splitlen = b_e - al->start;
				goto do_split;
			}
			if (__unlikely(r = get_blockrun(al->start, 1, &b_e))) return r;
			if (__unlikely(b_e < al->start + al->len)) {
				if (__likely(b_e != al->start)) {
					splitlen = b_e - al->start;
					goto do_split;
				}
				panic("map_file: get_blockrun returns no data");
			}
			copy_it:
			if (__unlikely(r = create_blocks(al->lbn, al->len, &newlist))) {
				if (r > 1) {
					free_file_runs(&newlist);
					return r;
				}
				free_all = 1;
				goto again;
			}
			le = al->list.next;
			DEL_FROM_LIST(&al->list);
			copy_to_list(size, al->start, &newlist, zeroed);
			mem_free(al);
			LIST_FOR_EACH(al, &newlist, ALLOC, list) c++;
			LIST_JOIN_END((LIST_HEAD *)le, &newlist);
			le = le->prev;
			al = LIST_STRUCT(le, ALLOC, list);
			continue;
		}
		if (__unlikely(r = alloc_try(al->start, al->len))) {
			if (r == -1) panic("map_file: alloc_try failed");
			return r;
		}
		c++;
		continue;
		do_split:
		if (__unlikely(!(all = mem_alloc(sizeof(ALLOC))))) {
			log_printf(1, "CAN'T ALLOC MEMORY WHEN SPLITTING SECTOR RUN");
			return 2;
		}
		all->lbn = al->lbn + splitlen;
		all->start = al->start + splitlen;
		all->len = al->len - splitlen;
		all->ano = 0;
		al->len = splitlen;
		ADD_TO_LIST_AFTER(&al->list, &all->list);
		goto again;
	}
	if (!LIST_EMPTY(&rec->rfd.runs)) {
		al = LIST_STRUCT(rec->rfd.runs.prev, ALLOC, list);
		last_lbn = al->lbn + al->len;
	} else {
		last_lbn = 0;
	}
	/*__debug_printf("%s/%s: %Lx ... %Lx\n", rec->path, rec->filename, last_lbn, (__u64)SPAD2CPU16_LV(&super.cluster_threshold) << super.sectors_per_cluster_bits);*/
	if (last_lbn >= (__u64)SPAD2CPU16_LV(&super.cluster_threshold) << super.sectors_per_cluster_bits && __unlikely((unsigned)last_lbn & ((1 << super.sectors_per_cluster_bits) - 1))) {
		LIST_HEAD newlist;
		if (__unlikely(r = create_blocks(last_lbn, (1 << super.sectors_per_cluster_bits) - ((unsigned)last_lbn & ((1 << super.sectors_per_cluster_bits) - 1)), &newlist))) {
			if (r > 1) {
				free_file_runs(&newlist);
				return r;
			}
			/* we may completely skip this step --- in that case
			   the code at the end of this function will truncate
			   file */
			goto skip_spacealloc;
		}
		copy_to_list(size, (blk_t)-1, &newlist, zeroed);
		LIST_FOR_EACH(al, &newlist, ALLOC, list) c++;
		LIST_JOIN_END((LIST_HEAD *)&rec->rfd.runs, &newlist);
		skip_spacealloc:;
	}
	cc = 0;
	LIST_FOR_EACH(al, &rec->rfd.runs, ALLOC, list) {
		blk_t splitlen;
#ifdef TESTCODE
		if (__unlikely(fragment_recovered_files) && al->len > 1 << super.sectors_per_block_bits) {
			splitlen = 1 << super.sectors_per_block_bits;
			goto do_split_2;
		}
#endif
		recheck_2:
		if (cc <= 1 && __unlikely(al->len > MAX_DIRECT_BLKS(1 << super.sectors_per_block_bits))) {
			splitlen = MAX_DIRECT_BLKS(1 << super.sectors_per_block_bits);
			goto do_split_2;
		}
#ifdef TESTCODE
		if (__likely(!fragment_recovered_files))
#endif
		if (al->list.next != (LIST_ENTRY *)(void *)&rec->rfd.runs) {
			all = LIST_STRUCT(al->list.next, ALLOC, list);
			if (al->start + al->len == all->start) {
				al->len += all->len;
				DEL_FROM_LIST(&all->list);
				mem_free(all);
				c--;
				goto recheck_2;
			}
		}
		cp_2:
		cc++;
		continue;

		do_split_2:
		if (__unlikely(!(all = mem_alloc(sizeof(ALLOC))))) {
			log_printf(1, "CAN'T ALLOC MEMORY WHEN SPLITTING SECTOR RUN");
			return 2;
		}
		all->lbn = al->lbn + splitlen;
		all->start = al->start + splitlen;
		all->len = al->len - splitlen;
		all->ano = 0;
		al->len = splitlen;
		ADD_TO_LIST_AFTER(&al->list, &all->list);
		c++;
		goto cp_2;
	}
#if __DEBUG >= 2
	if (__unlikely(c != cc)) panic("map_file: number of entries miscounted: %ld != %ld", c, cc);
#endif
#define write_anode							\
	do {								\
		a->flags |= ANODE_CHECKSUM_VALID;			\
		a->checksum ^= __byte_sum(a, ANODE_SIZE) ^ CHECKSUM_BASE;\
		write_buffer(ano, 1, a);				\
	} while (0)
	c = 0;
	free_all = 0;
	LIST_FOR_EACH(al, &rec->rfd.runs, ALLOC, list) {
		struct anode *a;
		blk_t ano;
		if (free_all) goto free_this;
		if (c == 0) {
			if (__unlikely(al->len > MAX_DIRECT_BLKS(1 << super.sectors_per_block_bits))) panic("map_file: run 1 has length %"blk_format"x", al->len);
			fnode->run10 = MAKE_PART_0(al->start);
			fnode->run11 = MAKE_PART_1(al->start);
			CPU2SPAD16_LV(&fnode->run1n, al->len);
		} else if (c == 1) {
			if (__unlikely(al->len > MAX_DIRECT_BLKS(1 << super.sectors_per_block_bits))) panic("map_file: run 2 has length %"blk_format"x", al->len);
			fnode->run20 = MAKE_PART_0(al->start);
			fnode->run21 = MAKE_PART_1(al->start);
			CPU2SPAD16_LV(&fnode->run2n, al->len);
		} else if (c == 2) {
			RECOVERY_FILENAME *nam;
			if (__unlikely(r = alloc_anode(&a, &ano))) {
				alloc_anode_failed:
				if (r > 1) return r;
				free_all_next:
				free_all = 1;
				free_this:
				if (__unlikely(r = free_blocks(al->start, al->len))) return r;
				all = LIST_STRUCT(al->list.prev, ALLOC, list);
				DEL_FROM_LIST(&al->list);
				mem_free(al);
				al = all;
				continue;
			}
			al->ano = ano;
			fnode->anode0 = MAKE_PART_0(ano);
			fnode->anode1 = MAKE_PART_1(ano);
			a->flags |= ANODE_ROOT;
			a->valid_extents = 3;
			CPU2SPAD64_LV(&a->x[0].blk, MAKE_D_OFF(fnode->run10, fnode->run11));
			CPU2SPAD64_LV(&a->x[0].end_off, SPAD2CPU16_LV(&fnode->run1n));
			CPU2SPAD64_LV(&a->x[1].blk, MAKE_D_OFF(fnode->run20, fnode->run21));
			CPU2SPAD64_LV(&a->x[1].end_off, SPAD2CPU16_LV(&fnode->run1n) + SPAD2CPU16_LV(&fnode->run2n));
			CPU2SPAD64_LV(&a->x[2].blk, al->start);
			CPU2SPAD64_LV(&a->x[2].end_off, al->lbn + al->len);
			nam = LIST_STRUCT(rec->filenames.next, RECOVERY_FILENAME, list);
			strncpy(ANODE_ROOT_NAME(a), nam->filename, ANODE_ROOT_NAME_LEN);
			write_anode;
		} else {
			int depth_now = 0, depth_total = 0;
			int direct;
			blk_t ano = MAKE_D_OFF(fnode->anode0, fnode->anode1);
			blk_t ano_l = (blk_t)-1;
			subnode:
			if (__unlikely(!(a = read_buffer(ano, 1, 0)))) {
				log_printf(1, "CAN'T READ ANODE %"blk_format"X: %s", ano, buffer_error);
				log_printf(1, "THE DEVICE IS UNRELIABLE, ANODE HAS BEEN WRITTEN BEFORE");
				goto free_all_next;
			}
			if (__unlikely(SPAD2CPU32_LV(&a->magic) != magic(ano, ANODE_MAGIC))) {
				log_printf(1, "BAD MAGIC ON ANODE %"blk_format"X: %08X", ano, (unsigned)SPAD2CPU32_LV(&a->magic));
				log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
				goto free_all_next;
			}
			if ((unsigned)a->valid_extents > ANODE_N_EXTENTS) {
				log_printf(1, "ANODE %"blk_format"X HAS %d VALID EXTENTS", ano, a->valid_extents);
				log_printf(1, "THE DEVICE IS UNRELIABLE, ANODE HAS BEEN CORRECT BEFORE");
				goto free_all_next;
			}
			direct = find_direct(depth_now, depth_total);
			if (__likely(a->valid_extents < direct)) {
				CPU2SPAD64_LV(&a->x[a->valid_extents].blk, al->start);
				CPU2SPAD64_LV(&a->x[a->valid_extents].end_off, al->lbn + al->len);
				a->valid_extents++;
				write_anode;
				goto cont;
			}
			if (a->valid_extents < ANODE_N_EXTENTS) ano_l = ano;
			if (a->valid_extents > direct) {
				ano = SPAD2CPU64_LV(&a->x[a->valid_extents - 1].blk);
				update_depth(&depth_now, &depth_total, a->valid_extents - 1);
				goto subnode;
			}
			if (__unlikely(ano_l == -1)) {
				log_printf(1, "ROOT ANODE %"blk_format"X IS FULL", (blk_t)MAKE_D_OFF(fnode->anode0, fnode->anode1));
				log_printf(1, "THE DEVICE IS UNRELIABLE, ANODES HAVE BEEN CORRECT BEFORE");
				goto free_all_next;
			}
			if (__unlikely(r = alloc_anode(&a, &ano))) {
				goto alloc_anode_failed;
			}
			al->ano = ano;
			a->valid_extents = 1;
			CPU2SPAD64_LV(&a->start_off, al->lbn);
			CPU2SPAD64_LV(&a->x[0].end_off, al->lbn + al->len);
			CPU2SPAD64_LV(&a->x[0].blk, al->start);
			write_anode;
			if (__unlikely(!(a = read_buffer(ano_l, 1, 0)))) {
				log_printf(1, "CAN'T READ ANODE %"blk_format"X: %s", ano_l, buffer_error);
				log_printf(1, "THE DEVICE IS UNRELIABLE, ANODE HAS BEEN WRITTEN BEFORE");
				if ((r = free_blocks(ano, 1 << super.sectors_per_block_bits))) return r;
				goto free_all_next;
			}
			if (__unlikely(SPAD2CPU32_LV(&a->magic) != magic(ano_l, ANODE_MAGIC))) {
				log_printf(1, "BAD MAGIC ON ANODE %"blk_format"X: %08X", ano_l, (unsigned)SPAD2CPU32_LV(&a->magic));
				log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
				if ((r = free_blocks(ano, 1 << super.sectors_per_block_bits))) return r;
				goto free_all_next;
			}
			if ((unsigned)a->valid_extents >= ANODE_N_EXTENTS) {
				log_printf(1, "ANODE %"blk_format"X HAS %d VALID EXTENTS", ano_l, a->valid_extents);
				log_printf(1, "THE DEVICE IS UNRELIABLE, ANODE HAS BEEN CORRECT BEFORE");
				if ((r = free_blocks(ano, 1 << super.sectors_per_block_bits))) return r;
				goto free_all_next;
			}
			CPU2SPAD64_LV(&a->x[a->valid_extents].blk, ano);
			CPU2SPAD64_LV(&a->x[a->valid_extents].end_off, -1);
			CPU2SPAD64_LV(&a->x[a->valid_extents - 1].end_off, al->lbn);
			a->valid_extents++;
			ano = ano_l;
			write_anode;
		}
		cont:
		c++;
	}
	if (__unlikely(LIST_EMPTY(&rec->rfd.runs))) {
		if (size) {
			log_printf(0, "FILE HAS NO VALID DATA, DELETING");
			return 1;
		}
		return 0;
	}
	al = LIST_STRUCT(rec->rfd.runs.prev, ALLOC, list);
	FILE_SECTORS(1 << SSIZE_BITS << super.sectors_per_block_bits, 1 << SSIZE_BITS << super.sectors_per_cluster_bits, ((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) * SPAD2CPU16_LV(&super.cluster_threshold), size, blocks);
	if (__unlikely(blocks != al->lbn + al->len)) {
		size = (al->lbn + al->len) << SSIZE_BITS;
		try_next_size:
		CPU2SPAD64_LV(&fnode->size[0], size);
		CPU2SPAD64_LV(&fnode->size[1], size);
		FILE_SECTORS(1 << SSIZE_BITS << super.sectors_per_block_bits, 1 << SSIZE_BITS << super.sectors_per_cluster_bits, ((__u64)1 << SSIZE_BITS << super.sectors_per_cluster_bits) * SPAD2CPU16_LV(&super.cluster_threshold), size, blocks);
		if (blocks > al->lbn + al->len) {
			size -= 1 << SSIZE_BITS;
			goto try_next_size;
		}
		while (blocks < al->lbn + al->len) {
			if (blocks <= al->lbn) {
				if (al->ano) if ((r = free_blocks(al->ano, 1 << super.sectors_per_block_bits))) return r;
				if (__unlikely(r = free_blocks(al->start, al->len))) return r;
				DEL_FROM_LIST(&al->list);
				mem_free(al);
			} else {
				if (__unlikely(r = free_blocks(al->start + (blocks - al->lbn), al->len - (blocks - al->lbn)))) return r;
				al->len = blocks - al->lbn;
			}
			al = LIST_STRUCT(rec->rfd.runs.prev, ALLOC, list);
		}
	}
	new_lblocks = (size + (1 << SSIZE_BITS) - 1) >> SSIZE_BITS;
	if (__unlikely(new_lblocks < total_lblocks)) {
		log_printf(0, "FILE TRUNCATED FROM %"blk_format"d TO %"blk_format"d BLOCKS", total_lblocks, new_lblocks);
	}
	zeroed_lblocks = 0;
	LIST_FOR_EACH(za, zeroed, ZEROED, list) {
		if (za->lbn + za->len <= new_lblocks) zeroed_lblocks += za->len;
		else if (za->lbn < new_lblocks) zeroed_lblocks += new_lblocks - za->lbn;
	}
	if (__unlikely(zeroed_lblocks != 0)) {
		log_printf(0, "%"blk_format"d BLOCKS COULD NOT BE RECOVERED AND WERE FILLED WITH ZEROS", zeroed_lblocks);
	}
	return 0;
#undef write_anode
}

static int do_recover_file(RECOVERY *rec)
{
	RECOVERY_FILENAME *first_nam = LIST_STRUCT(rec->filenames.next, RECOVERY_FILENAME, list);
	void *result;
	int r;
	struct fnode fnode;
	memset(&fnode, 0, sizeof fnode);
	CPU2SPAD64_LV(&fnode.size[0], rec->rfd.size);
	CPU2SPAD32_LV(&fnode.ctime, rec->rfd.ctime);
	CPU2SPAD32_LV(&fnode.mtime, rec->rfd.mtime);
	fnode.flags = rec->rfd.flags;
	if (!(rec->rfd.flags & FNODE_FLAGS_DIR)) {
		LIST_HEAD z;
		r = map_file(rec, &fnode, &z);
		while (!LIST_EMPTY(&z)) {
			ZEROED *za = LIST_STRUCT(z.next, ZEROED, list);
			DEL_FROM_LIST(&za->list);
			mem_free(za);
		}
		if (__unlikely(r)) return r;
		if (!SPAD2CPU64_LV(&fnode.size[0])) {
			fnode.run10 = CPU2SPAD32(rec->rfd.run10);
			fnode.run11 = CPU2SPAD16(rec->rfd.run11);
			fnode.run1n = CPU2SPAD16(rec->rfd.run1n);
			fnode.run20 = CPU2SPAD32(rec->rfd.run20);
			fnode.run21 = CPU2SPAD16(rec->rfd.run21);
			fnode.run2n = CPU2SPAD16(rec->rfd.run2n);
			fnode.anode0 = CPU2SPAD32(rec->rfd.anode0);
			fnode.anode1 = CPU2SPAD16(rec->rfd.anode1);
		}
	} else {
		fnode.run10 = MAKE_PART_0(rec->rfd.dnode);
		fnode.run11 = MAKE_PART_1(rec->rfd.dnode);
		fnode.run1n = CPU2SPAD16(rec->rfd.run1n);
		fnode.run2n = CPU2SPAD16(rec->rfd.run2n);
		fnode.anode0 = CPU2SPAD32(rec->rfd.anode0);
		fnode.anode1 = CPU2SPAD16(rec->rfd.anode1);
		/* directories do not have hardlinks, take the first and only entry */
		if (__unlikely(r = claim_recovered_directory(first_nam->path, first_nam->filename, rec->rfd.dnode, &result))) return r;
	}
	if (rec->filenames.next == rec->filenames.prev && !first_nam->ea_size) {	/* not a hardlink */
		blk_t result_block;
		unsigned result_offset;
		fix_ea(&fnode, (struct fnode_ea *)rec->ea, &rec->ea_size, EA_TYPE_FILE_NAME | EA_ZERO_PREALLOC);
		if (__unlikely(r = add_fnode_to_dir(first_nam->path, first_nam->filename, &fnode, (struct fnode_ea *)rec->ea, rec->ea_size, &result_block, &result_offset))) {
			failure:
			if (rec->rfd.flags & FNODE_FLAGS_DIR) {
				int rr = undo_recovered_directory(result);
				if (rr > r) return rr;
			} else {
				r = dealloc_and_free_blocklist(r, &rec->rfd.runs);
			}
			return r;
		}
		if (rec->rfd.flags & FNODE_FLAGS_DIR)
			set_recovered_directory_position(result, result_block, result_offset);
	} else {
		struct fixed_fnode_block *fx;
		blk_t nlink;
		blk_t fx_blk;
		RECOVERY_FILENAME *nam;
		fix_ea(&fnode, (struct fnode_ea *)rec->ea, &rec->ea_size, EA_TYPE_FILE | EA_ZERO_PREALLOC);
		if (__unlikely(r = create_fixed_fnode(&fnode, (struct fnode_ea *)rec->ea, rec->ea_size, &fx_blk))) goto failure;
		nlink = 0;
		LIST_FOR_EACH(nam, &rec->filenames, RECOVERY_FILENAME, list) {
			struct fnode hardlink_fnode;
			log_printf(0, "RECOVERING HARDLINK \"%s%s%s\"", escape_name(nam->path), !*nam->path || nam->path[strlen(nam->path) - 1] != '/' ? "/" : "" , escape_name(nam->filename));
			memset(&hardlink_fnode, 0, sizeof hardlink_fnode);
			hardlink_fnode.anode0 = MAKE_PART_0(fx_blk);
			hardlink_fnode.anode1 = MAKE_PART_1(fx_blk);
			hardlink_fnode.flags = FNODE_FLAGS_HARDLINK;
			fix_ea(&hardlink_fnode, (struct fnode_ea *)nam->ea, &nam->ea_size, EA_TYPE_NAME);
			r = add_fnode_to_dir(nam->path, nam->filename, &hardlink_fnode, (struct fnode_ea *)nam->ea, nam->ea_size, NULL, NULL);
			if (__unlikely(r == 2)) return r;
			if (!r) nlink++;
		}
		if (__unlikely(!nlink)) {
			if (__unlikely(free_blocks(fx_blk, 1 << super.sectors_per_block_bits) == 2)) return 2;
			goto failure;
		}
		fx = reread_fixed_fnode_block(fx_blk);
		if (__unlikely(!fx)) {
			log_printf(1, "ERROR READING FIXED FNODE BLOCK %"blk_format"X: %s", fx_blk, buffer_error);
			log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN WRITTEN BEFORE");
			return 1;
		}
		CPU2SPAD64_LV(&fx->nlink0, nlink);
		if (__unlikely(!update_fixed_fnode_block(fx, fx_blk))) {
			log_printf(1, "ERROR UPDATING FIXED FNODE BLOCK %"blk_format"X: %s", fx_blk, buffer_error);
			log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN WRITTEN BEFORE");
			return 1;
		}
	}
	return 0;
}

static void delete_recovery(RECOVERY *rec)
{
	if (!(rec->rfd.flags & FNODE_FLAGS_DIR)) free_file_runs(&rec->rfd.runs);
	while (!LIST_EMPTY(&rec->filenames)) {
		RECOVERY_FILENAME *nam = LIST_STRUCT(rec->filenames.next, RECOVERY_FILENAME, list);
		DEL_FROM_LIST(&nam->list);
		mem_free(nam);
	}
	DEL_FROM_LIST(&rec->list);
	mem_free(rec);
}

static int cmp_files(const void *p1, const void *p2)
{
	RECOVERY_FILENAME *nam1, *nam2;
	RECOVERY *r1 = *(RECOVERY **)p1;
	RECOVERY *r2 = *(RECOVERY **)p2;
	int c;
	if (LIST_EMPTY(&r1->rfd.runs) && !LIST_EMPTY(&r2->rfd.runs)) return -1;
	if (!LIST_EMPTY(&r1->rfd.runs) && LIST_EMPTY(&r2->rfd.runs)) return 1;
	if (!LIST_EMPTY(&r1->rfd.runs) && !LIST_EMPTY(&r2->rfd.runs)) {
		ALLOC *a1 = LIST_STRUCT(r1->rfd.runs.prev, ALLOC, list);
		ALLOC *a2 = LIST_STRUCT(r2->rfd.runs.prev, ALLOC, list);
		if (a1->lbn + a1->len < a2->lbn + a2->len) return -1;
		if (a1->lbn + a1->len > a2->lbn + a2->len) return 1;
	}
	nam1 = LIST_STRUCT(r1->filenames.next, RECOVERY_FILENAME, list);
	nam2 = LIST_STRUCT(r2->filenames.next, RECOVERY_FILENAME, list);
	if ((c = strcmp(nam1->path, nam2->path))) return c;
	if ((c = strcmp(nam1->filename, nam2->filename))) return c;
	return 0;
}

/*
 * Sorting directories is required.
 * It is possible that there are two nested recoveries, like
 * "a/b" and "a/b/c". In this case we must recover the deeper directory first.
 */

static unsigned long dir_depth(const char *p)
{
	unsigned long depth = 0;
	while ((p = strchr(p, '/'))) {
		depth++;
		p++;
	}
	return depth;
}

static int cmp_directories(const void *p1, const void *p2)
{
	RECOVERY_FILENAME *nam1, *nam2;
	unsigned long d1, d2;
	RECOVERY *r1 = *(RECOVERY **)p1;
	RECOVERY *r2 = *(RECOVERY **)p2;
	nam1 = LIST_STRUCT(r1->filenames.next, RECOVERY_FILENAME, list);
	nam2 = LIST_STRUCT(r2->filenames.next, RECOVERY_FILENAME, list);

	d1 = dir_depth(nam1->path);
	d2 = dir_depth(nam2->path);
	if (d1 > d2) return -1;
	if (d1 < d2) return 1;
	if (r1->idx < r2->idx) return -1;
	if (r1->idx > r2->idx) return 1;
	return 0;
}

static int do_recovery(LIST_HEAD *recoveries)
{
	RECOVERY *rec, *recc;
	RECOVERY **a;
	long c;

#ifdef TESTCODE
	if (__unlikely(dont_sort_recovered_files) && recoveries == &file_recoveries) goto dont_sort;
#endif
	c = 0;
	LIST_FOR_EACH(rec, recoveries, RECOVERY, list) c++;
	if (__unlikely(!(a = mem_alloc(sizeof(RECOVERY *) * c)))) {
		log_printf(0, "CAN'T ALLOC MEMORY WHEN SORTING");
		return 2;
	}
	c = 0;
	LIST_FOR_EACH(rec, recoveries, RECOVERY, list) {
		(a[c] = rec)->idx = c;
		c++;
	}
	qsort(a, c, sizeof(RECOVERY *), recoveries == &file_recoveries ? cmp_files : cmp_directories);

	INIT_LIST(recoveries);
	while (--c >= 0) ADD_TO_LIST(recoveries, &a[c]->list);

	mem_free(a);

#ifdef TESTCODE
	dont_sort:
#endif
	LIST_FOR_EACH(rec, recoveries, RECOVERY, list) {
		int r;
		RECOVERY_FILENAME *first_nam = LIST_STRUCT(rec->filenames.next, RECOVERY_FILENAME, list);
		log_printf(0, "RECOVERING %s \"%s%s%s\"%s", rec->rfd.flags & FNODE_FLAGS_DIR ? "DIRECTORY" : "FILE", escape_name(first_nam->path), !*first_nam->path || first_nam->path[strlen(first_nam->path) - 1] != '/' ? "/" : "" , escape_name(first_nam->filename), rec->filenames.next != rec->filenames.prev ? " AND HARDLINKS" : "");
		if (query(qprefix Q_NOFIX, "RECOVER?")) {
			r = do_recover_file(rec);
			if (__unlikely(r > 1)) return r;
		}
		recc = LIST_STRUCT(rec->list.prev, RECOVERY, list);
		delete_recovery(rec);
		rec = recc;
	}
	return 0;
}

int do_directories_recovery(void)
{
	if (__unlikely(!LIST_EMPTY(&dir_recoveries))) status_printf(0, "recovering directories...");
	return do_recovery(&dir_recoveries);
}

int do_files_recovery(void)
{
	if (__unlikely(!LIST_EMPTY(&file_recoveries))) status_printf(0, "recovering files...");
	return do_recovery(&file_recoveries);
}

void done_recovery(void)
{
	while (!LIST_EMPTY(&dir_recoveries)) {
		RECOVERY *rec = LIST_STRUCT(dir_recoveries.next, RECOVERY, list);
		delete_recovery(rec);
	}
	while (!LIST_EMPTY(&file_recoveries)) {
		RECOVERY *rec = LIST_STRUCT(file_recoveries.next, RECOVERY, list);
		delete_recovery(rec);
	}
}

