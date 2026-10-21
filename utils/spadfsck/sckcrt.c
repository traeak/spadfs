#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

/* algorithm from hpfs_stop_cycles in linux/fs/hpfs/super.c */
/* set (*c)[1] = 0 at first time */

static int spad_stop_cycles(blk_t key, blk_t (*c)[2])
{
	if (__unlikely((*c)[0] == key) && __unlikely((*c)[1] != 0)) return -1;
	(*c)[1]++;
	if (__likely(!(((*c)[1] - 1) & (*c)[1]))) (*c)[0] = key;
	return 0;
}

static void increase_filename(char filename[256])
{
	char *t = strrchr(filename, '~');
	char *tt;
	if (!t || __unlikely(!t[1])) {
		addsuffix:
		filename[253] = 0;
		strcat(filename, "~1");
		return;
	}
	for (tt = t + 1; *tt; tt++) if (__unlikely(*tt < '0') || __unlikely(*tt > '9')) goto addsuffix;
	while (--tt != t) {
		if (__likely(*tt != '9')) {
			(*tt)++;
			return;
		}
		*tt = '0';
	}
	if (__likely(strlen(filename) < 255)) {
		memmove(tt + 2, tt + 1, strlen(tt/* + 1*/)/* + 1*/);
		tt[1] = '1';
		return;
	}
	if (__likely(tt != filename)) {
		tt[-1] = '~';
		tt[0] = '1';
		return;
	}
	return;
}

static void make_fnode_block(struct fnode_block *f, blk_t block, unsigned dirblks)
{
	unsigned i;
	memset(f, 0, dirblks << SSIZE_BITS);
	for (i = 0; i < dirblks; i++, f = (struct fnode_block *)((char *)f + FNODE_BLOCK_SIZE)) {
		struct fnode *fn;
		CPU2SPAD32_LV(&f->magic, magic(block + i, FNODE_BLOCK_MAGIC));
		f->flags = (FNODE_BLOCK_FIRST * !i) | (FNODE_BLOCK_LAST * (i == dirblks - 1)) | FNODE_BLOCK_CHECKSUM_VALID;
		CPU2SPAD32_LV(&f->txc, 0x80000000U);
		CPU2SPAD16_LV(&f->cc, 0);
		fn = f->fnodes;
		CPU2SPAD16_LV(&fn->next, (FNODE_MAX_SIZE & FNODE_NEXT_SIZE) | FNODE_NEXT_FREE);
		CPU2SPAD16_LV(&fn->cc, 0);
		CPU2SPAD32_LV(&fn->txc, 0);
		f->checksum ^= CHECKSUM_BASE ^ __byte_sum(f, FNODE_BLOCK_SIZE);
	}
}

static inline void make_dnode_page(struct dnode_page *d, blk_t block)
{
	memset(d, 0, dnode_page_sectors << SSIZE_BITS);
	CPU2SPAD32_LV(&d->magic, magic(block, DNODE_PAGE_MAGIC));
	d->gflags = (super.sectors_per_page_bits - 1) << DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT;
}

static int lookup(blk_t blk, blk_t dir_blk, unsigned dir_offset, const char *filename, struct fnode **result, blk_t *res_page, blk_t *result_fnode, unsigned *result_offset)
{
	int r;
	__u32 hash = name_hash(filename);
	unsigned hash_bits_used = 0;
	struct fnode_block *f;
	unsigned fp;
	blk_t c[2] = { 0, 0 };
	*result = NULL;

	again:
	if (__unlikely(spad_stop_cycles(blk, &c))) {
		log_printf(0, "CYCLE DETECTED WHEN LOOKING UP AT BLOCK %"blk_format"X", blk);
		return 1;
	}
	f = read_buffer(blk, 1, 0);
	if (__unlikely(!f)) {
		log_printf(0, "ERROR READING FNODE BLOCK %"blk_format"X: %s", blk, buffer_error);
		return 1;
	}
	if (__unlikely(SPAD2CPU32_LV(&f->magic) == magic(blk, DNODE_PAGE_MAGIC))) {
		unsigned idx, number_of_bits, i;
		blk_t subblk;
		struct dnode_page *dn = read_buffer(blk, dnode_page_sectors, 0);
		struct dnode_page_entry *data;
		int o;
		if (__unlikely(!dn)) {
			log_printf(0, "ERROR READING DNODE %"blk_format"X: %s", blk, buffer_error);
			return 1;
		}
		idx = hash & ((1 << dnode_hash_bits) - 1);
		hash >>= dnode_hash_bits;
		if (cc_valid(dn->cc, dn->txc)) data = (struct dnode_page_entry *)((char *)dn + DNODE_ENTRY_OFFSET), o = 0;
		else data = (struct dnode_page_entry *)((char *)dn + DNODE_ENTRY_OFFSET + dnode_data_size), o = 1;
		subblk = MAKE_D_OFF3(data[idx].b0, data[idx].b1, data[idx].b2);
		if (__likely(subblk != 0)) {
			blk = subblk;
			hash_bits_used += dnode_hash_bits;
			goto again;
		}

		if (!res_page) return 0;

		if (__unlikely(r = alloc_space(0, "FNODE BLOCK", 1 << super.sectors_per_fnodepage_bits, (1 << super.sectors_per_fnodepage_bits) - 1, 0, &subblk))) return r;

		number_of_bits = 0;
		if (hash_bits_used + dnode_hash_bits > 32) number_of_bits = hash_bits_used + dnode_hash_bits - 32;

		idx = rev_bits(idx);
		idx &= ~((1 << number_of_bits) - 1);
		for (i = 0; i < 1 << number_of_bits; i++) {
			data[rev_bits(idx + i)].b0 = MAKE_PART_30(subblk);
			data[rev_bits(idx + i)].b1 = MAKE_PART_31(subblk);
			data[rev_bits(idx + i)].b2 = MAKE_PART_32(subblk);
		}
		dn->checksum[o] = __byte_sum(data, dnode_data_size) ^ CHECKSUM_BASE;
		write_buffer(blk, dnode_page_sectors, dn);
		f = mem_alloc(1 << SSIZE_BITS << super.sectors_per_fnodepage_bits);
		if (__unlikely(!f)) {
			log_printf(1, "CAN'T ALLOC MEMORY FOR FNODE BLOCK");
			return 2;
		}
		make_fnode_block(f, subblk, 1 << super.sectors_per_fnodepage_bits);
		write_buffer(subblk, 1 << super.sectors_per_fnodepage_bits, f);
		mem_free(f);
		if (__unlikely(r = adjust_dir_size(dir_blk, dir_offset, 1 << SSIZE_BITS << super.sectors_per_fnodepage_bits))) return r;
		blk = subblk;
		goto again;
	}
	if (__unlikely(SPAD2CPU32_LV(&f->magic) != magic(blk, FNODE_BLOCK_MAGIC))) {
		log_printf(0, "BAD MAGIC ON FNODE BLOCK %"blk_format"X: %08X", blk, (unsigned)SPAD2CPU16(f->magic));
		return 1;
	}
	if (res_page) *res_page = blk, res_page = NULL;
#define fn ((struct fnode *)((char *)f + fp))
	fp = SIZEOF_FNODE_BLOCK;
	do {
		unsigned size = SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_SIZE;
		if (cc_valid(fn->cc, fn->txc) && SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_FREE) {
			fp += size;
			continue;
		}
		if (__unlikely(size <= FNODE_NAME_POS) || __unlikely(fp + size > FNODE_BLOCK_SIZE) || __unlikely(FNODE_NAME_POS + fn->namelen > size) || __unlikely(!fn->namelen)) {
			log_printf(0, "FNODE BLOCK %"blk_format"X: INVALID FNODE SIZE %X AT %X, NAMELEN %X", blk, SPAD2CPU16_LV(&fn->next), fp, fn->namelen);
			return 1;
		}
		if (__unlikely(fn->namelen == strlen(filename))) {
			if (!(SPAD2CPU32_LV(&super.flags_compat_none) & FLAG_COMPAT_NONE_UNIX_NAMES) ?
			   !_memcasecmp(FNODE_NAME(fn), filename, fn->namelen) :
			   !memcmp(FNODE_NAME(fn), filename, fn->namelen)) {
				*result = fn;
				if (result_fnode) *result_fnode = blk;
				if (result_offset) *result_offset = fp;
				return 0;
			}
		}
		fp += size;
	} while (fp < FNODE_BLOCK_SIZE);
#undef fn
	if (!(f->flags & FNODE_BLOCK_LAST)) {
		blk++;
		goto again;
	}
	if (cc_valid(f->cc, f->txc)) {
		blk = MAKE_D_OFF(f->next0, f->next1);
		goto again;
	}
	return 0;
}

static int add_entry(blk_t blk, blk_t dir_blk, unsigned dir_offset, const struct fnode *new_fn, const char *filename, const struct fnode_ea *ea, unsigned ea_size, blk_t *result_block, unsigned *result_offset)
{
	int r;
	struct fnode_block *f;
	unsigned fp;
	unsigned wanted_size = FNODE_SIZE(strlen(filename), ea_size);
	blk_t subblk;
	blk_t c[2] = { 0, 0 };
	again:
	if (__unlikely(spad_stop_cycles(blk, &c))) {
		log_printf(0, "CYCLE DETECTED WHEN ADDING ENTRY AT BLOCK %"blk_format"X", blk);
		return 1;
	}
	f = read_buffer(blk, 1, 0);
	if (__unlikely(!f)) {
		read_err:
		log_printf(0, "ERROR READING FNODE BLOCK %"blk_format"X: %s", blk, buffer_error);
		log_printf(0, "THE DEVICE IS UNRELIABLE, FNODE HAS BEEN READ BEFORE");
		return 1;
	}
	if (__unlikely(SPAD2CPU32_LV(&f->magic) != magic(blk, FNODE_BLOCK_MAGIC))) {
		log_printf(0, "BAD MAGIC ON FNODE BLOCK %"blk_format"X: %08X", blk, (unsigned)SPAD2CPU32_LV(&f->magic));
		log_printf(0, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
		return 1;
	}
#define update_block							\
do {									\
	f->flags |= FNODE_BLOCK_CHECKSUM_VALID;				\
	f->checksum ^= CHECKSUM_BASE ^ __byte_sum(f, FNODE_BLOCK_SIZE);	\
	write_buffer(blk, 1, f);					\
	if (__unlikely(!(f = read_buffer(blk, 1, 0)))) goto read_err;	\
} while (0)
#define fn ((struct fnode *)((char *)f + fp))
	fp = SIZEOF_FNODE_BLOCK;
	do {
		unsigned size = SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_SIZE;
		if (cc_valid(fn->cc, fn->txc) && SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_FREE) {
			int need_update = 0;
			next_join:
			fp += size;
			if (fp < FNODE_BLOCK_SIZE && cc_valid(fn->cc, fn->txc) && SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_FREE) {
				unsigned newsize = SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_SIZE;
				fp -= size;
				size += newsize;
				CPU2SPAD16_LV(&fn->next, (SPAD2CPU16_LV(&fn->next) & ~FNODE_NEXT_SIZE) | (size & FNODE_NEXT_SIZE));
				need_update = 1;
				goto next_join;
			}
			if (size >= wanted_size) {
				fp -= size;
				if (result_block) *result_block = blk;
				if (result_offset) *result_offset = fp;
				memset(fn, 0, wanted_size);
				memcpy(fn, new_fn, sizeof(struct fnode));
				CPU2SPAD16_LV(&fn->next, (SPAD2CPU16_LV(&fn->next) & ~(FNODE_NEXT_SIZE | FNODE_NEXT_FREE)) | wanted_size);
				fn->namelen = strlen(filename);
				memcpy(FNODE_NAME(fn), filename, strlen(filename));
				memcpy((char *)fn + FNODE_EA_POS(fn->namelen), ea, ea_size);
				fp += wanted_size;
				if (wanted_size != size) {
					CPU2SPAD16_LV(&fn->next, (size - wanted_size) | FNODE_NEXT_FREE);
					CPU2SPAD16_LV(&fn->cc, 0);
					CPU2SPAD32_LV(&fn->txc, 0);
				}
				need_update = 2;
			}
			if (need_update) update_block;
			if (need_update == 2) return 0;
			continue;
		}
		fp += size;
	} while (fp < FNODE_BLOCK_SIZE);
	if (!(f->flags & FNODE_BLOCK_LAST)) {
		blk++;
		goto again;
	}
	if (cc_valid(f->cc, f->txc)) {
		blk = MAKE_D_OFF(f->next0, f->next1);
		goto again;
	}
	if (__unlikely(r = alloc_space(0, "FNODE BLOCK", 1 << super.sectors_per_block_bits, (1 << super.sectors_per_block_bits) - 1, 0, &subblk))) return r;
	f->next0 = MAKE_PART_0(subblk);
	f->next1 = MAKE_PART_1(subblk);
	CPU2SPAD16_LV(&f->cc, 0);
	CPU2SPAD32_LV(&f->txc, 0);
	update_block;
	f = mem_alloc(1 << SSIZE_BITS << super.sectors_per_block_bits);
	if (__unlikely(!f)) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR FNODE BLOCK");
		return 2;
	}
	make_fnode_block(f, subblk, 1 << super.sectors_per_block_bits);
	f->prev0 = MAKE_PART_0(blk);
	f->prev1 = MAKE_PART_1(blk);
	write_buffer(subblk, 1 << super.sectors_per_block_bits, f);
	mem_free(f);
	blk = subblk;
	if (__unlikely(r = adjust_dir_size(dir_blk, dir_offset, 1 << SSIZE_BITS << super.sectors_per_block_bits))) return r;
	goto again;
#undef fn
#undef update_block
}

int create_empty_directory(blk_t *result, unsigned *size)
{
#if 0
	int r;
	struct fnode_block *f;

	*size = 1 << super.sectors_per_fnodepage_bits;
	if (__unlikely(r = alloc_space(0, "FNODE BLOCK", *size, (1 << super.sectors_per_page_bits) - 1, 0, result))) return r;
	f = mem_alloc(*size << SSIZE_BITS);
	if (__unlikely(!f)) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR FNODE BLOCK");
		return 2;
	}
	make_fnode_block(f, *result, *size);
	write_buffer(*result, *size, f);
	mem_free(f);
	return 0;
#else
	int r;
	struct dnode_page *d;

	*size = dnode_page_sectors;
	if (__unlikely(r = alloc_space(0, "DNODE PAGE", *size, (1 << super.sectors_per_page_bits) - 1, 0, result))) return r;
	d = mem_alloc(*size << SSIZE_BITS);
	if (__unlikely(!d)) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR DNODE PAGE");
		return 2;
	}
	make_dnode_page(d, *result);
	write_buffer(*result, *size, d);
	mem_free(d);
	return 0;
#endif
}

static int do_mkdir(blk_t blk, blk_t dir_blk, unsigned dir_offset, char *filename, blk_t *result_block, unsigned *result_offset, blk_t *result)
{
	int r;
	struct fnode dir_entry;
	unsigned new_dir_sectors;

	if (__unlikely(r = create_empty_directory(result, &new_dir_sectors))) return r;

	memset(&dir_entry, 0, sizeof dir_entry);
	dir_entry.ctime = dir_entry.mtime = CPU2SPAD32(time(NULL));
	dir_entry.size[0] = dir_entry.size[1] = SPAD2CPU32_LV(&super.flags_compat_ro) & FLAG_COMPAT_RO_DIRECTORY_SIZES ? CPU2SPAD64(new_dir_sectors << SSIZE_BITS) : 0;
	dir_entry.run10 = dir_entry.run20 = MAKE_PART_0(*result);
	dir_entry.run11 = dir_entry.run21 = MAKE_PART_1(*result);
	dir_entry.flags = FNODE_FLAGS_DIR;
	if (__unlikely(r = add_entry(blk, dir_blk, dir_offset, &dir_entry, filename, NULL, 0, result_block, result_offset))) {
		if (r != 2) {
			int rr;
			if ((rr = free_blocks(*result, new_dir_sectors))) return rr;
		}
		return r;
	}
	return 0;
}

int add_fnode_to_dir(const char *path, const char *filename, const struct fnode *new_fnode, const struct fnode_ea *ea, unsigned ea_size, blk_t *result_block, unsigned *result_offset)
{
	int r;
	blk_t dir_blk;
	unsigned dir_offset;
	blk_t blk;
	char dirname_orig[257];
	char dirname[257];
	int chg_name;
	char *p;
	struct fnode *fn;
	blk_t pg;

	dir_blk = root_fnode;
	dir_offset = root_offset;
	blk = root_dir;

	gosub:
	while (*path == '/') path++;
	if (!*path) goto end_of_path;
	strlcpy(dirname, path, 257);
	if ((p = strchr(dirname, '/'))) {
		*p = 0;
		path += p - dirname;
	} else {
		path = "";
	}
	if (__unlikely(strlen(dirname) == 256)) {
		log_printf(0, "TOO LONG DIRECTORY NAME IN PATH");
		return 1;
	}
	strcpy(dirname_orig, dirname);
	chg_name = 0;
	find_dir_again:
	if (__unlikely(r = lookup(blk, dir_blk, dir_offset, dirname, &fn, &pg, &dir_blk, &dir_offset))) return r;
	if (__unlikely(!fn)) {
		if (__unlikely(r = do_mkdir(pg, dir_blk, dir_offset, dirname, &dir_blk, &dir_offset, &blk))) return r;
		if (__unlikely(chg_name)) log_printf(0, "DIRECTORY NAME \"%s\" RENAMED TO \"%s\" BECAUSE OF EXISTING FILE WITH THE SAME NAME", escape_name(dirname_orig), escape_name(dirname));
		goto gosub;
	} else {
		if (__unlikely(!(fn->flags & FNODE_FLAGS_DIR))) {
			increase_filename(dirname);
			chg_name = 1;
			goto find_dir_again;
		}
		blk = cc_valid(fn->cc, fn->txc) ? MAKE_D_OFF(fn->run10, fn->run11) : MAKE_D_OFF(fn->run20, fn->run21);
		goto gosub;
	}
	end_of_path:
	if (__unlikely(strlen(filename) >= 256)) {
		log_printf(0, "TOO LONG FILENAME: %u BYTES", (unsigned)strlen(filename));
		return 1;
	}
	strcpy(dirname, filename);
	chg_name = 0;
	find_file_again:
	if (__unlikely(r = lookup(blk, dir_blk, dir_offset, dirname, &fn, &pg, NULL, NULL))) return r;
	if (__unlikely(fn != NULL)) {
		increase_filename(dirname);
		chg_name = 1;
		goto find_file_again;
	}
	if (__unlikely(r = add_entry(pg, dir_blk, dir_offset, new_fnode, dirname, ea, ea_size, result_block, result_offset))) return r;
	if (__unlikely(chg_name)) log_printf(0, "%s NAME \"%s\" RENAMED TO \"%s\" BECAUSE OF EXISTING DIRECTORY OR FILE WITH THE SAME NAME", new_fnode->flags & FNODE_FLAGS_DIR ? "DIRECTORY" : "FILE", escape_name(filename), escape_name(dirname));
	return 0;
}

int create_fixed_fnode(const struct fnode *new_fnode, const struct fnode_ea *ea, unsigned ea_size, blk_t *result)
{
	int r;
	struct fixed_fnode_block fx;
	if (__unlikely(r = alloc_space(0, "FIXED FNODE BLOCK", 1 << super.sectors_per_block_bits, (1 << super.sectors_per_block_bits) - 1, 0, result))) return r;
	memset(&fx, 0, sizeof fx);
	CPU2SPAD32_LV(&fx.magic, magic(*result, FIXED_FNODE_BLOCK_MAGIC));
	fx.flags = 0;
	CPU2SPAD16_LV(&fx.cc, 0);
	CPU2SPAD32_LV(&fx.txc, 0);
	CPU2SPAD64_LV(&fx.nlink0, 1);	/* will be updated later */
	memcpy(fx.fnode0, new_fnode, sizeof(struct fnode));
	CPU2SPAD16_LV(&((struct fnode *)&fx.fnode0)->next, FNODE_SIZE(0, ea_size));
	((struct fnode *)&fx.fnode0)->namelen = 0;
	memcpy((char *)fx.fnode0 + FNODE_EA_POS(0), ea, ea_size);
	fx.checksum ^= CHECKSUM_BASE ^ __byte_sum(&fx, FIXED_FNODE_BLOCK_SIZE);
	if (__unlikely(!update_fixed_fnode_block(&fx, *result))) {
		if (free_blocks(*result, 1 << super.sectors_per_block_bits) == 2) return 2;
		return 1;
	}
	return 0;
}

int file_exists_in_root(const char *file)
{
	struct fnode *fn;

	while (*file == '/') file++;

	if (__unlikely(lookup(root_dir, root_fnode, root_offset, file, &fn, NULL, NULL, NULL))) return 0;
	return !!fn;
}
