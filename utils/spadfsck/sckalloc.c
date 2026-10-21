#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#define BUCKET_SIZE_BITS	8
#define BUCKET_SIZE		(1 << BUCKET_SIZE_BITS)

typedef union __dire DIRE;

union __dire {
	DIRE *sub[BUCKET_SIZE];
	__u32 data[BUCKET_SIZE / 32];
};

#define DIR_EMPTY	((DIRE *)NULL)
#define DIR_FULL	((DIRE *)1)

static DIRE root;
static int levels;

static int swaph = -1;
static int swap_existed = 0;
int swap_allocmap = 0;

int alloc_over_blocks;
blk_t newalloc;
blk_t blocks_allocated;

unsigned long map_allocated = 0;

char alloc_error[__MAX_STR_LEN];

int allocation_map_ok;

static struct stat swapst;

static void free_alloc_mem(void);

void init_alloc(void)
{
	memset(&root, 0, sizeof root);
	levels = 0;
	alloc_over_blocks = 0;
	newalloc = 0;
	swap_allocmap = 0;
	map_allocated = 0;
	blocks_allocated = 0;
	allocation_map_ok = 0;
}

static int alloc_internal_error(char *s1, char *s2)
{
	if (!swap_allocmap) panic(s1, s2);
	log_printf(1, s1, s2);
	if (swaph != -1) log_printf(1, "SWAP FILE IS UNRELIABLE");
	else log_printf(1, "ALLOCATION ERROR WHEN SWAPPING TO FILESYSTEM. DEVICE IS UNRELIABLE");
	return 2;
}

int init_swapfile(void)
{
	blk_t size_align;
	void *z;
	blk_t zp;
	if (__likely(!swapfile)) return 0;
	swap_existed = !stat(swapfile, &swapst);
	swaph = open(swapfile, O_RDWR | O_CREAT, 0600);
	if (swaph == -1) {
		log_printf(0, "UNABLE TO OPEN OR CREATE SWAP FILE \"%s\": %s", escape_name(swapfile), strerror(errno));
		return 2;
	}
	if (__unlikely(fstat(swaph, &swapst))) {
		log_printf(0, "UNABLE TO FSTAT SWAP FILE: %s", strerror(errno));
		clre:
		close(swaph);
		if (!swap_existed) unlink(swapfile);
		swaph = -1;
		return 2;
	}
	size_align = ((SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits) + BUFFER_SIZE * 8 - 1) & ~(__u64)(BUFFER_SIZE * 8 - 1);
	size_align >>= 3;
	if (S_ISREG(swapst.st_mode) && __unlikely(swapst.st_size > size_align)) {
		/* do not clear the whole swapfile with
		   ftruncate(0); ftruncate(size_align) ... on unix it would
		   create holes in file and fsck could fail on insufficient
		   disk space during operation */
		if (__unlikely(ftruncate(swaph, size_align))) {
			log_printf(0, "UNABLE TO TRUNCATE SWAP FILE TO %"blk_format"d BYTES: %s", size_align, strerror(errno));
			goto clre;
		}
	}
	z = dummy_buffer();
	memset(z, 0, BUFFER_SIZE);
	for (zp = 0; zp < size_align; zp += BUFFER_SIZE) {
		if (__unlikely(!io(pwrite, swaph, z, BUFFER_SIZE, zp))) {
			if (!errno) log_printf(0, "SHORT WRITE TO SWAP FILE AT POSITION %"blk_format"d", zp);
			else log_printf(0, "UNABLE TO WRITE TO SWAP FILE POSITION %"blk_format"d: %s", zp, strerror(errno));
			goto clre;
		}
	}
	swap_allocmap = 1;
	return 0;
}

#define APAGE_SWAPSTART		((sizeof(struct apage_head) - sizeof(struct apage_subhead) + sizeof(unsigned long) - 1) & ~(sizeof(unsigned long) - 1))

	/* arguments and internals scaled down */
static int map_swapper_block(blk_t b, int wr, unsigned long **data, unsigned *len)
{
	if (swaph != -1) {
		unsigned p = (b & (BUFFER_SIZE * 8 - 1)) / (sizeof(unsigned long) * 8);
		unsigned long *d = rw_swap_buffer(swaph, (b & ~(blk_t)(BUFFER_SIZE * 8 - 1)) >> (SSIZE_BITS + 3), wr);
		if (__unlikely(!d)) {
			log_printf(1, "UNABLE TO READ SWAP FILE: %s", buffer_error);
			return 2;
		}
		*data = d + p;
		*len = ((BUFFER_SIZE * 8) / (sizeof(unsigned long) * 8)) - p;
		return 0;
	} else {
		unsigned long *d;
		unsigned div = ((1 << (SSIZE_BITS - 1) << super.sectors_per_page_bits) - APAGE_SWAPSTART) * 8;
		unsigned offs = b % div;
		unsigned idx = b / div;
		unsigned long p = APAGE_SWAPSTART / sizeof(unsigned long) + offs / (sizeof(unsigned long) * 8);
		if (__unlikely(idx >= n_apages))
			panic("map_swapper_block: access beyond last apage, block %"blk_format"x", b);
		d = rw_swap_buffer(device_handle, swapspace[idx] & ~(blk_t)(SECTORS_PER_BUFFER - 1), !!wr);
		if (__unlikely(!d)) {
			log_printf(1, "UNABLE TO READ ALLOCATION MAP: %s", buffer_error);
			return 2;
		}
		d += ((unsigned)swapspace[idx] & (SECTORS_PER_BUFFER - 1)) * ((1 << SSIZE_BITS) / sizeof(unsigned long));
		*data = d + p;
		*len = ((1 << (SSIZE_BITS - 1)) / sizeof(unsigned long) << super.sectors_per_page_bits) - p;
		return 0;
	}
}

	/* arguments are scaled down by super.sectors_per_block_bits */
static int alloc_free_blocks_swapper(blk_t b, blk_t l, int do_alloc)
{
	blk_t orig_b = b;
	if (__unlikely(b + l > SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits)) panic("alloc_free_blocks_swapper: %s over filesystem length %"blk_format"X + %"blk_format"X > %"blk_format"X", do_alloc ? "alloc" : "free", b, l, (blk_t)SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits);
	while (l) {
		unsigned long *data;
		unsigned len, bp, tl;
		if (__unlikely(map_swapper_block(b, 1, &data, &len))) return 2;
		bp = (unsigned)b & (sizeof(unsigned long) * 8 - 1);
		tl = l > (len * (sizeof(unsigned long) * 8)) - bp ? (len * (sizeof(unsigned long) * 8)) - bp : l;
		/*__debug_printf("%p: %d,%d\n", data, bp, tl);*/
		l -= tl;
		b += tl;
		if (__likely(do_alloc)) blocks_allocated += tl;
		else blocks_allocated -= tl;
		while (bp & (sizeof(unsigned long) * 8 - 1) && tl) {
			ibits:
			if (__likely(do_alloc)) {
				if (__unlikely(__BTS(data, bp))) {
					blocks_allocated -= tl;
					_snprintf(alloc_error, sizeof alloc_error, "CROSS LINKED AT %"blk_format"X", b - tl);
					if (free_blocks(orig_b << super.sectors_per_block_bits, (b - tl - orig_b) << super.sectors_per_block_bits) == 2) return 2;
					return 1;
				}
			} else {
				if (__unlikely(!__BTR(data, bp))) {
					blocks_allocated += tl;
					_snprintf(alloc_error, sizeof alloc_error, "FREE OF NON-ALLOCATED BLOCK %"blk_format"X", b - tl);
					return 1;
				}
			}
			bp++;
			tl--;
		}
		data += bp / (sizeof(unsigned long) * 8);
		bp = 0;
		if (__likely(do_alloc)) {
			while (tl >= sizeof(unsigned long) * 8) {
				if (__unlikely(*data != 0UL)) goto ibits;
				*data++ = ~0UL;
				tl -= sizeof(unsigned long) * 8;
			}
		} else {
			while (tl >= sizeof(unsigned long) * 8) {
				if (__unlikely(*data != ~0UL)) goto ibits;
				*data++ = 0UL;
				tl -= sizeof(unsigned long) * 8;
			}
		}
		if (__unlikely(tl)) goto ibits;
	}
	return 0;
}

	/* arguments & return value is scaled down by super.sectors_per_block_bits */
static int get_blockrun_swapper(blk_t b, int alloced, blk_t *e)
{
	again:
	if (__unlikely(b >= SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits)) {
		sup_size:
		*e = SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits;
		return 0;
	} else {
		unsigned long *data;
		unsigned bp, tl;
		if (__unlikely(map_swapper_block(b, 1, &data, &tl))) return 2;
		bp = (unsigned)b & (sizeof(unsigned long) * 8 - 1);
		tl = tl * (sizeof(unsigned long) * 8) - bp;
		b += tl;
		while (bp & (sizeof(unsigned long) * 8 - 1) && tl) {
			ibits:
			if (__unlikely(__BT(data, bp) != alloced)) {
				if (__unlikely((*e = b - tl) > SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits)) goto sup_size;
				return 0;
			}
			bp++;
			tl--;
		}
		data += bp / (sizeof(unsigned long) * 8);
		bp = 0;
		if (alloced) {
			while (tl >= sizeof(unsigned long) * 8) {
				if (__unlikely(*data != ~0UL)) goto ibits;
				data++;
				tl -= sizeof(unsigned long) * 8;
			}
		} else {
			while (tl >= sizeof(unsigned long) * 8) {
				if (__unlikely(*data != 0UL)) goto ibits;
				data++;
				tl -= sizeof(unsigned long) * 8;
			}
		}
		if (__unlikely(tl)) goto ibits;
	}
	goto again;
}

	/* internals are scaled. Attention for get_blockrun call */
void enable_swapping(void)
{
	blk_t b;
	if (swap_allocmap || !allow_swapping || ro) return;
	allow_swapping = 0;
	b = 0;
	while (b < SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits) {
		unsigned long *data;
		unsigned len;
		if (__unlikely(map_swapper_block(b, 2, &data, &len))) {
			return;
		}
		memset(data, 0, len * sizeof(unsigned long));
		b += len * (sizeof(unsigned long) * 8);
	}
	b = 0;
	while (b < SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits) {
		blk_t b_e, b_a;
		int r;
		if (__unlikely(get_blockrun(b << super.sectors_per_block_bits, 1, &b_e))) return;
		b_e >>= super.sectors_per_block_bits;
		b_a = blocks_allocated;
		r = alloc_free_blocks_swapper(b, b_e - b, 1);
		blocks_allocated = b_a;
		if (__unlikely(r)) {
			log_printf(1, "CAN'T ENABLE SWAPPING: %s", alloc_error);
			log_printf(1, "THE DEVICE IS UNRELIABLE");
			return;
		}
		if (__unlikely(get_blockrun(b_e << super.sectors_per_block_bits, 0, &b))) return;
		b >>= super.sectors_per_block_bits;
	}
	free_alloc_mem();
	swap_allocmap = 1;
	status_printf(0, "started swapping into unused parts of apages.");
}

	/* scaled down */
static void collapse_tree(blk_t b, DIRE *fill)
{
	int collapse_level = 0;
	int lev;
	DIRE *dir, **sub;
	unsigned k;
	collapse_next_level:
	if (levels == collapse_level) return;
	dir = &root;
	lev = levels;
	while (1) {
		unsigned off = (unsigned)(b >> (lev * BUCKET_SIZE_BITS)) & (BUCKET_SIZE - 1);
		sub = &dir->sub[off];
		if (--lev == collapse_level) break;
		dir = *sub;
	}
	mem_free(*sub);
	map_allocated -= __likely(!collapse_level) ? BUCKET_SIZE / 8 : sizeof(DIRE);
	*sub = fill;
	for (k = 0; k < BUCKET_SIZE; k++) if (dir->sub[k] != fill) return;
	collapse_level++;
	goto collapse_next_level;
}

	/* arguments are unscaled, internals are scaled */
static int alloc_free_blocks(blk_t b, blk_t l, int do_alloc)
{
	blk_t orig_b;
	DIRE *dir;
	int lev;
	unsigned off, lx;

	allocation_map_ok = 0;

	b >>= super.sectors_per_block_bits;
	l >>= super.sectors_per_block_bits;
	if (swap_allocmap) return alloc_free_blocks_swapper(b, l, do_alloc);
	orig_b = b;
	repeat:
	while (__unlikely(b >= ((blk_t)1 << ((levels + 1) * BUCKET_SIZE_BITS)))) {
		unsigned sz = !levels ? BUCKET_SIZE / 8 : sizeof(DIRE);
		DIRE *new = mem_alloc(sz);
		if (__unlikely(!new)) goto no_mem;
		map_allocated += sz;
		memcpy(new, &root, sz);
		memset(&root, 0, sizeof root);
		root.sub[0] = new;
		levels++;
	}
	dir = &root;
	lev = levels;
	while (lev) {
		DIRE **sub;
		off = (unsigned)(b >> (lev * BUCKET_SIZE_BITS)) & (BUCKET_SIZE - 1);
		sub = &dir->sub[off];
		if (__unlikely(*sub == DIR_FULL)) {
			if (__likely(!do_alloc)) goto xfree;
			cross_link:
			_snprintf(alloc_error, sizeof alloc_error, "CROSS LINKED AT %"blk_format"X", b);
			if (free_blocks(orig_b << super.sectors_per_block_bits, (b - orig_b) << super.sectors_per_block_bits) == 2) return 2;
			return 1;
		}
		if (__unlikely(*sub == DIR_EMPTY)) {
			blk_t adv;
			if (__unlikely(!do_alloc)) {
				bad_free:
				_snprintf(alloc_error, sizeof alloc_error, "FREE OF NON-ALLOCATED BLOCK %"blk_format"X", b);
				return 1;
			}
			xfree:
			adv = (blk_t)1 << (lev * BUCKET_SIZE_BITS);
			if (!(b & (adv - 1)) && l >= adv) {
				if (__likely(do_alloc)) {
					*sub = DIR_FULL;
					blocks_allocated += adv;
				} else {
					*sub = DIR_EMPTY;
					blocks_allocated -= adv;
				}
				b += adv;
				l -= adv;
				goto cont;
			} else {
				if (__likely(lev == 1)) {
					*sub = mem_alloc(BUCKET_SIZE / 8);
					if (__unlikely(!*sub)) goto no_mem;
					map_allocated += BUCKET_SIZE / 8;
					if (__likely(do_alloc)) {
						memset(*sub, 0, BUCKET_SIZE / 8);
					} else {
						memset(*sub, 0xff, BUCKET_SIZE / 8);
					}
				} else {
					*sub = mem_alloc(sizeof(DIRE));
					if (__unlikely(!*sub)) goto no_mem;
					map_allocated += sizeof(DIRE);
					if (__likely(do_alloc)) {
						memset(*sub, 0, sizeof(DIRE));
					} else {
						unsigned k;
						for (k = 0; k < BUCKET_SIZE; k++) (*sub)->sub[k] = DIR_FULL;
					}
				}
			}
		}
		dir = *sub;
		lev--;
	}
	off = (unsigned)b & (BUCKET_SIZE - 1);
	lx = BUCKET_SIZE - off;
	if (lx > l) lx = l;
	b += lx;
	l -= lx;
	if (__likely(do_alloc)) {
		unsigned k;
		blocks_allocated += lx;
		do {
			if (__unlikely(__BTS(dir, off))) {
				blk_t nb = ((b - 1) & ~(blk_t)(BUCKET_SIZE - 1)) | off;
				blocks_allocated -= b - nb;
				b = nb;
				goto cross_link;
			}
			off++;
			lx--;
		} while (lx);
		for (k = 0; k < BUCKET_SIZE / 32; k++) if (dir->data[k] != 0xffffffff) goto skp1;
		collapse_tree(b - 1, DIR_FULL);
		skp1:;
	} else {
		unsigned k;
		blocks_allocated -= lx;
		do {
			if (__unlikely(!__BTR(dir, off))) {
				blk_t nb = ((b - 1) & ~(blk_t)(BUCKET_SIZE - 1)) | off;
				blocks_allocated += b - nb;
				b = nb;
				goto bad_free;
			}
			off++;
			lx--;
		} while (lx);
		for (k = 0; k < BUCKET_SIZE / 32; k++) if (dir->data[k]) goto skp2;
		collapse_tree(b - 1, DIR_EMPTY);
		skp2:;
	}

	cont:
	if (l) goto repeat;
	return 0;

	no_mem:
	log_printf(1, "CAN'T ALLOC MEMORY FOR ALLOCATION MAP");
	return 2;
}

	/* arguments are unscaled. Function internals are scaled */
int get_blockrun(blk_t b, int alloced, blk_t *e)
{
	unsigned off;
	DIRE *dir;
	int lev;
	b >>= super.sectors_per_block_bits;
	if (swap_allocmap) {
		int r = get_blockrun_swapper(b, alloced, e);
		*e <<= super.sectors_per_block_bits;
		return r;
	}
	again:
	if (__unlikely(b >= ((blk_t)1 << ((levels + 1) * BUCKET_SIZE_BITS)))) {
		if (__likely(!alloced)) *e = SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits;
		else {
			ret_zero:
			if (__unlikely(b > SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits)) b = SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits;
			*e = b;
		}
		*e <<= super.sectors_per_block_bits;
		return 0;
	}
	dir = &root;
	lev = levels;
	while (lev) {
		off = (unsigned)(b >> (lev * BUCKET_SIZE_BITS)) & (BUCKET_SIZE - 1);
		dir = dir->sub[off];
		if (__unlikely(dir == DIR_FULL)) {
			blk_t adv;
			if (__unlikely(!alloced)) goto ret_zero;
			do_adv:
			adv = (blk_t)1 << (lev * BUCKET_SIZE_BITS);
			b = (b & ~(adv - 1)) + adv;
			goto again;
		}
		if (__unlikely(dir == DIR_EMPTY)) {
			if (__unlikely(alloced)) goto ret_zero;
			goto do_adv;
		}
		lev--;
	}
	off = (unsigned)b & (BUCKET_SIZE - 1);
	do {
		if (__unlikely(__BT(dir, off) != alloced)) goto ret_zero;
		b++;
	} while (++off != BUCKET_SIZE);
	goto again;
}

	/* unscaled */
int check_allocation(blk_t b, blk_t l)
{
	if (__unlikely(!l)) {
		_snprintf(alloc_error, sizeof alloc_error, "ZERO-SIZE ALLOCATION AT %"blk_format"X", b);
		return 1;
	}
	if (__unlikely(b + l < b)) {
		_snprintf(alloc_error, sizeof alloc_error, "ALLOCATION WRAP AROUND (%"blk_format"X, %"blk_format"X)", b, l);
		return 1;
	}
	if (__unlikely(b + l > SPAD2CPU64_LV(&super.size))) {
		_snprintf(alloc_error, sizeof alloc_error, "ALLOCATION BEYOND END (%"blk_format"X + %"blk_format"X > %"blk_format"X)", b, l, SPAD2CPU64_LV(&super.size));
		return 1;
	}
	if (__unlikely(((unsigned)b | (unsigned)l) & ((1 << super.sectors_per_block_bits) - 1))) {
		_snprintf(alloc_error, sizeof alloc_error, "UNALIGNED ALLOCATION (%"blk_format"X, %"blk_format"X), BLOCK ALIGN %d", b, l, 1 << super.sectors_per_block_bits);
		return 1;
	}
	return 0;
}

	/* unscaled */
int alloc_blocks(blk_t b, blk_t l)
{
	if (__unlikely(check_allocation(b, l))) return 1;
	return alloc_free_blocks(b, l, 1);
}

	/* unscaled */
int free_blocks(blk_t b, blk_t l)
{
	int r;
	if (__unlikely(!l)) return 0;
	if (__unlikely(check_allocation(b, l))) return alloc_internal_error("free_blocks: bad allocation: %s", alloc_error);
	r = alloc_free_blocks(b, l, 0);
	if (__unlikely(r == 1)) return alloc_internal_error("free_blocks: alloc_free_blocks failed: %s", alloc_error);
	return r;
}

	/* unscaled */
int force_alloc_blocks(blk_t b, blk_t l)
{
	int r;
	blk_t b_e;
	if (__unlikely(check_allocation(b, l))) return 0;
	l += b;
	while (b < l) {
		if (__unlikely(r = get_blockrun(b, 0, &b_e))) return r;
		if (b_e > l) b_e = l;
		if (b_e != b) if (__unlikely(r = alloc_blocks(b, b_e - b))) {
			if (r == 1) return alloc_internal_error("force_alloc_blocks: alloc_blocks failed: %s", alloc_error);
			return r;
		}
		if (__unlikely(r = get_blockrun(b_e, 1, &b))) return r;
	}
	return 0;
}

static blk_t alloc_start = 0;
static blk_t alloc_len = 0;

void suggest_free_space(blk_t start, blk_t len)
{
	if (len > alloc_len) {
		if ((start | len) & ((1 << super.sectors_per_block_bits) - 1))
			panic("suggest_free_space: unaligned suggestion %"blk_format"x,%"blk_format"x", start, len);
		alloc_start = start;
		alloc_len = len;
	}
}

	/* unscaled */
int alloc_space(int pri, char *message, blk_t n_sec, int mask, int first_fit, blk_t *result)
/* !!! FIXME: use first_fit */
{
	static blk_t over_offset = 0;
	int r;
	int pass = 0;
	blk_t b, b_e;
	n_sec = (n_sec + (1 << super.sectors_per_block_bits) - 1) & ~(blk_t)((1 << super.sectors_per_block_bits) - 1);

	next_pass:
	b = !pass ? alloc_start : 0;
	while (b < SPAD2CPU64_LV(&super.size)) {
		if (__unlikely(r = get_blockrun(b, 0, &b_e))) return r;
		if (b_e - b >= n_sec) {
			if (__unlikely(b + n_sec > SPAD2CPU64_LV(&super.size))) break;
			if (mask && ((unsigned)b & mask) + n_sec > mask + 1) {
				b = (b + mask) & ~(blk_t)mask;
				continue;
			}
			if (__unlikely(skip_recovery_blocks(b, n_sec, &b))) {
				b = (b + (1 << super.sectors_per_block_bits) - 1) & ~(blk_t)((1 << super.sectors_per_block_bits) - 1);
				continue;
			}
			if (__unlikely(r = alloc_blocks(b, n_sec))) {
				if (r == 1) return alloc_internal_error("alloc_space: alloc_blocks failed: %s", alloc_error);
				return r;
			}
			alloc_start = b + n_sec;
			*result = b;
			newalloc += n_sec;
			return 0;
		}
		if (__unlikely(r = get_blockrun(b_e, 1, &b))) return r;
	}
	if (!pass) {
		pass = 1;
		goto next_pass;
	}
	if (message) log_printf(0, "NO SPACE FOR RECOVERY OF %s", escape_name(message));
	if (!pri) {
		return -1;
	} else {
		if (SPAD2CPU64_LV(&super.size) < over_offset + n_sec + SUPERBLOCK_SECTOR * 2) {
			log_printf(0, "ABORTING");
			return 1;
		}
		if (!alloc_over_blocks && !query(qprefix 0, "ALLOC OVER EXISTING FILES?")) return 1;

		over_offset += n_sec;
		*result = SPAD2CPU64_LV(&super.size) - over_offset;
		*result &= ~(blk_t)mask;
		over_offset = SPAD2CPU64_LV(&super.size) - *result;

		if ((r = force_alloc_blocks(*result, n_sec))) return r;
		alloc_over_blocks = 1;
		newalloc += n_sec;
		return 0;
	}
}

	/* unscaled */
int alloc_try(blk_t b, blk_t n_sec)
{
	int r;
	blk_t b_e;
	n_sec = (n_sec + (1 << super.sectors_per_block_bits) - 1) & ~(blk_t)((1 << super.sectors_per_block_bits) - 1);
	if (__unlikely(b + n_sec < b) || __unlikely(b + n_sec > SPAD2CPU64_LV(&super.size))) return -1;
	if (__unlikely(r = get_blockrun(b, 0, &b_e))) return r;
	if (__likely(b_e < b + n_sec)) return -1;
	if (__unlikely(r = alloc_blocks(b, n_sec))) {
		if (r == 1) return alloc_internal_error("alloc_try: alloc_blocks failed: %s", alloc_error);
		return r;
	}
	return 0;
}

static void free_recursive(DIRE *e, int level)
{
	unsigned i;
	if (!level) return;
	level--;
	for (i = 0; i < BUCKET_SIZE; i++) {
		if (e->sub[i] != DIR_EMPTY && e->sub[i] != DIR_FULL) {
			free_recursive(e->sub[i], level);
			mem_free(e->sub[i]);
			map_allocated -= __likely(!level) ? BUCKET_SIZE / 8 : sizeof(DIRE);
		}
	}
}

	/* unscaled */
int reset_allocations(void)
{
	int r;
	blk_t b = 0;
	while (b < SPAD2CPU64_LV(&super.size)) {
		blk_t b_e;
		if (__unlikely(r = get_blockrun(b, 1, &b_e))) return r;
		if (__unlikely(r = free_blocks(b, b_e - b))) return r;
		if (__unlikely(r = get_blockrun(b_e, 0, &b))) return r;
	}
	if (__unlikely(r = recover_super_allocs())) return r;
	if (__unlikely(r = recover_cct_allocs())) return r;
	if (__unlikely(r = recover_apage_allocs())) return r;
	return 0;
}

static void free_alloc_mem(void)
{
	free_recursive(&root, levels);
	levels = 0;
	if (__unlikely(map_allocated != 0)) panic("free_alloc_mem: %lx bytes leaked", map_allocated);
}

void done_alloc(void)
{
	if (swaph != -1) {
		close(swaph);
		swaph = -1;
		if (!swap_existed) unlink(swapfile);
	}
	free_alloc_mem();
}


