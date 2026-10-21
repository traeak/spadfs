#ifndef _SPADFS_COMMON_SFSAPAGE_H
#define _SPADFS_COMMON_SFSAPAGE_H

extern char APAGE_ERROR[];
int SPAD_APAGE_FREE(struct apage_head *a, __d_off off, __u32 len);
int SPAD_APAGE_MAKE(struct apage_head *a, unsigned n_entries, unsigned sectors_per_block_bits);

static __finline__ __u32 max(__u32 a, __u32 b)
{
	return a < b ? b : a;
}

/* note: APTR_ALIGN(returned_blk->next) MUST be valid (< depth).
   if not, check all places that call this function */
static __finline__ int SPAD_APAGE_FIND_BLOCK_BEFORE(struct apage_head *a, __d_off o, int depth)
{
#define aa(i) (*(struct aentry *)((char *)a + (i)))
	int i;
	int rlimit;
	int st = 0;
	int nx;
	__d_off mx = 0;
	i = 4 * sizeof(struct aentry);
	do {
		/* one cacheline ... 64 bytes			^^^^^ make it
		   alias after two cycles --- do not pollute cache much */
		int j;
		__PREFETCHT0((char *)a + i + 128 * sizeof(struct aentry));
#if defined(__GNUC__) && __GNUC__ >= 5
		/* avoid a bogus warning in gcc 5 and 6 */
		__asm__("":::"memory");
#endif
		for (j = 0; j < 4; j++) {
			if (SPAD2CPU32_LV(&aa(i).len) && SPAD2CPU64_LV(&aa(i).start) <= o && __unlikely(SPAD2CPU64_LV(&aa(i).start) >= mx)) mx = SPAD2CPU64_LV(&aa(i).start), st = i;
			i += sizeof(struct aentry);
		}
	} while ((int)(i += 124 * sizeof(struct aentry)) <= (int)(depth - (4 + 128) * sizeof(struct aentry)));
	rlimit = depth;
	do {
		nx = APTR_ALIGN(SPAD2CPU16_LV(&aa(st).next));
		if (__unlikely(nx >= depth)) return -1;
		if (__unlikely(!nx) || SPAD2CPU64_LV(&aa(nx).start) > o) return st;
		st = nx;
	} while (__likely(rlimit -= sizeof(struct aentry)));
	return -1;
#undef aa
}

static __finline__ int SPAD_DELETE_BLOCK(struct apage_head *a, int blk, int depth)
{
#define aa(i) (*(struct aentry *)((char *)a + (i)))
	int preblk = APTR_ALIGN(SPAD2CPU16_LV(&aa(blk).prev));
	int postblk = APTR_ALIGN(SPAD2CPU16_LV(&aa(blk).next));
	if (__unlikely(preblk >= depth) || __unlikely(postblk >= depth)) {
		return -1;
	}
	CPU2SPAD16_LV(&aa(preblk).next, postblk);
	CPU2SPAD16_LV(&aa(postblk).prev, preblk);
	CPU2SPAD64_LV(&aa(blk).start, 0);
	CPU2SPAD32_LV(&aa(blk).len, 0);
	CPU2SPAD16_LV(&aa(blk).prev, 0);
	aa(blk).next = a->s.u.l.freelist;
	CPU2SPAD16_LV(&a->s.u.l.freelist, blk);
	return 0;
#undef aa
}

#endif
