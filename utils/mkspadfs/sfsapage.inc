#ifndef __SPAD__
#include "sfsapage.h"
#else
#include "SFSAPAGE.H"
#endif

char APAGE_ERROR[__MAX_STR_LEN];

int SPAD_APAGE_FREE(struct apage_head *a, __d_off off, __u32 len)
{
	int depth = APAGE_SIZE(a->s.u.l.flags);
	if (__likely(!(a->s.u.l.flags & APAGE_BITMAP))) {
#define aa(i) (*(struct aentry *)((char *)a + (i)))
		int newblk, preblk, postblk;
		preblk = SPAD_APAGE_FIND_BLOCK_BEFORE(a, off, depth);
		if (__unlikely(preblk == -1)) goto error_over;
		postblk = APTR_ALIGN(SPAD2CPU16_LV(&aa(preblk).next));
		/*
	not needed, already checked in SPAD_APAGE_FIND_BLOCK_BEFORE
		if (__unlikely(postblk >= depth)) goto error_over;
		*/
		if (__likely(preblk != 0)) {
			if ((__d_off)SPAD2CPU64_LV(&aa(preblk).start) + SPAD2CPU32_LV(&aa(preblk).len) == off) {
				if (__unlikely((__u32)(SPAD2CPU32_LV(&aa(preblk).len) + len) < (__u32)len)) goto nj1;
				if (__likely(postblk) != 0) {
					if (off + len == (__d_off)SPAD2CPU64_LV(&aa(postblk).start)) {
						if (__unlikely((__u32)(SPAD2CPU32_LV(&aa(preblk).len) + len + SPAD2CPU32_LV(&aa(postblk).len)) < (__u32)SPAD2CPU32_LV(&aa(postblk).len))) goto nj2;
						CPU2SPAD32_LV(&aa(preblk).len, SPAD2CPU32_LV(&aa(preblk).len) + len + SPAD2CPU32_LV(&aa(postblk).len));
						if (__unlikely(SPAD_DELETE_BLOCK(a, postblk, depth))) goto error_over;
						goto done;
					} else if (__unlikely(off + len > (__d_off)SPAD2CPU64_LV(&aa(postblk).start))) {
						goto post_over;
					}
				}
				nj2:
				CPU2SPAD32_LV(&aa(preblk).len, SPAD2CPU32_LV(&aa(preblk).len) + len);
				goto done;
			} else if (__unlikely((__d_off)SPAD2CPU64_LV(&aa(preblk).start) + SPAD2CPU32_LV(&aa(preblk).len) > off)) {
				postblk = preblk;
				goto post_over;
			}
		}
		nj1:
		if (__likely(postblk != 0)) {
			if (off + len == (__d_off)SPAD2CPU64_LV(&aa(postblk).start)) {
				if (__unlikely((__u32)(SPAD2CPU32_LV(&aa(postblk).len) + len) < (__u32)len)) goto nj3;
				CPU2SPAD64_LV(&aa(postblk).start, off);
				CPU2SPAD32_LV(&aa(postblk).len, SPAD2CPU32_LV(&aa(postblk).len) + len);
				goto done;
			} else if (__unlikely(off + len > (__d_off)SPAD2CPU64_LV(&aa(postblk).start))) {
				post_over:
				_snprintf(APAGE_ERROR, __MAX_STR_LEN, "FREE (%04X%08X,%08X) DOES OVERLAP WITH BLOCK (%04X%08X,%08X)", (int)(off >> 31 >> 1), (int)(off & 0xFFFFFFFF), (unsigned)len, (unsigned)(SPAD2CPU64_LV(&aa(postblk).start) >> 31 >> 1), (unsigned)(SPAD2CPU64_LV(&aa(postblk).start) & 0xFFFFFFFF), (unsigned)SPAD2CPU32_LV(&aa(postblk).len));
				return -1;
			}
		}
		nj3:
		if (__unlikely(!(newblk = APTR_ALIGN(SPAD2CPU16_LV(&a->s.u.l.freelist))))) return 1;
		if (newblk >= depth) goto error_over;
		a->s.u.l.freelist = aa(newblk).next;
		CPU2SPAD64_LV(&aa(newblk).start, off);
		CPU2SPAD32_LV(&aa(newblk).len, len);
		CPU2SPAD16_LV(&aa(newblk).prev, preblk);
		CPU2SPAD16_LV(&aa(newblk).next, postblk);
		CPU2SPAD16_LV(&aa(preblk).next, newblk);
		CPU2SPAD16_LV(&aa(postblk).prev, newblk);

		done:
		return 0;

		error_over:
		_snprintf(APAGE_ERROR, __MAX_STR_LEN, "POINTER OVERRUN");
		return -1;
	} else {
		unsigned bmpoff = BITMAP_OFFSET(a, off);
		len = BITMAP_LEN(a, len);
		if (bmpoff + len <= bmpoff || bmpoff + len > BITMAP_SIZE(depth)) {
			_snprintf(APAGE_ERROR, __MAX_STR_LEN, "BAD BITMAP OFFSET: %04X%08X,%08X FREE(%04X%08X,%08X)", (int)(MAKE_D_OFF(a->s.u.b.start0, a->s.u.b.start1) >> 31 >> 1), (int)(MAKE_D_OFF(a->s.u.b.start0, a->s.u.b.start1) & 0xffffffffu), bmpoff, (int)(off >> 31 >> 1), (int)(off & 0xffffffffu), (int)len);
			return -1;
		}
		do {
			BITMAP_CLEAR(a, bmpoff);
			bmpoff++;
		} while (--len);
		return 0;
	}
}



int SPAD_APAGE_MAKE(struct apage_head *a, unsigned n_entries, unsigned sectors_per_block_bits)
{
#define aa(i) (*(struct aentry *)((char *)a + (i)))
	int depth;
	unsigned i;
	memset(&a->s, 0, sizeof(struct apage_subhead) + sizeof(struct aentry) * (n_entries - 1));
	for (i = 0; 1 << i < n_entries * sizeof(struct aentry); i++) ;
	if (__unlikely(i - SSIZE_BITS > (APAGE_SIZE_BITS >> APAGE_SIZE_BITS_SHIFT))) return -1;
	if (__unlikely(sectors_per_block_bits > (APAGE_BLOCKSIZE_BITS >> APAGE_BLOCKSIZE_BITS_SHIFT))) return -1;
	a->s.u.l.flags = ((i - SSIZE_BITS) << APAGE_SIZE_BITS_SHIFT) | (sectors_per_block_bits << APAGE_BLOCKSIZE_BITS_SHIFT);
	depth = APAGE_SIZE(a->s.u.l.flags);
	for (i = sizeof(struct aentry); i < depth; i += sizeof(struct aentry)) {
		CPU2SPAD64_LV(&aa(i).start, 0);
		CPU2SPAD32_LV(&aa(i).len, 0);
		CPU2SPAD16_LV(&aa(i).prev, 0);
		CPU2SPAD16_LV(&aa(i).next, i + sizeof(struct aentry) < depth ? i + sizeof(struct aentry) : 0);
	}
	CPU2SPAD16_LV(&a->s.u.l.freelist, sizeof(struct aentry));
	CPU2SPAD16_LV(&a->s.u.l.first, 0);
	CPU2SPAD16_LV(&a->s.u.l.last, 0);
	return 0;
#undef aa
}



