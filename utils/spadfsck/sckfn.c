#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#ifdef use_llseek

#include <asm/unistd.h>
static _syscall5(int, _llseek, uint, fd, ulong, hi, ulong, lo, loff_t *, res, uint, wh);

ssize_t xpread(int h, void *buf, size_t count, __u64 off)
{
	loff_t res;
	if (__unlikely(_llseek(h, off >> 32, off & 0xFFFFFFFF, &res, SEEK_SET) != 0)) return -1;
	return read(h, buf, count);
}

ssize_t xpwrite(int h, void *buf, size_t count, __u64 off)
{
	loff_t res;
	if (__unlikely(_llseek(h, off >> 32, off & 0xFFFFFFFF, &res, SEEK_SET) != 0)) return -1;
	return write(h, buf, count);
}

#endif

#ifdef TESTCODE

typedef struct {
	LIST_ENTRY list;
	size_t size;
	void *fn;
	unsigned magic;
} MALLOC_BLOCK;

#define MALLOC_BLOCK_SIZE	((sizeof(MALLOC_BLOCK) + 1 + 15) & ~15)

#define MALLOC_MAGIC	0xa110c
#define FREE_MAGIC	0xf4ee
#define REDZONE_BEFORE	'B'
#define REDZONE_AFTER	'R'

static DECL_XLIST(alloc_list);

#endif

void *mem_alloc(size_t s)
{
#ifdef TESTCODE
	if (__unlikely(debug_malloc)) {
		MALLOC_BLOCK *m = __sync_malloc(s + MALLOC_BLOCK_SIZE + 1);
		if (__unlikely(!m)) {
			was_out_of_memory = 1;
			return NULL;
		}
		m->size = s;
#if defined(__GNUC__)
		m->fn = __builtin_return_address(0);
#else
		m->fn = NULL;
#endif
		m->magic = MALLOC_MAGIC;
		ADD_TO_XLIST(&alloc_list, &m->list);
		((unsigned char *)m)[MALLOC_BLOCK_SIZE - 1] = REDZONE_BEFORE;
		((unsigned char *)m)[MALLOC_BLOCK_SIZE + s] = REDZONE_AFTER;
		return (unsigned char *)m + MALLOC_BLOCK_SIZE;
#if defined(__GNUC__) && (__GNUC__ <= 2)
		alloca(1);	/* GCC 2.7.2.1 has broken __builtin_return_address with -fomit-frame-pointer, force frame pointer */
#endif
	}
#endif
	{
		void *p = __sync_malloc(s);
		if (__unlikely(!p)) was_out_of_memory = 1;
		return p;
	}
}

void mem_free(void *p)
{
	if (__unlikely(!p)) return;
#ifdef TESTCODE
	if (__unlikely(debug_malloc)) {
		MALLOC_BLOCK *m = (MALLOC_BLOCK *)((unsigned char *)p - MALLOC_BLOCK_SIZE);
		if (__unlikely(m->magic != MALLOC_MAGIC))
			panic("mem_free: invalid block %p, magic %x, size %lx, allocated at %p", m, m->magic, (unsigned long)m->size, m->fn);
		if (__unlikely(((unsigned char *)m)[MALLOC_BLOCK_SIZE - 1] != REDZONE_BEFORE) ||
		    __unlikely(((unsigned char *)m)[MALLOC_BLOCK_SIZE + m->size] != REDZONE_AFTER))
			panic("mem_free: redzone damaged, block %p, size %lX, redzone before %02x, redzone after %02x, allocated at %p",
				m,
				(unsigned long)m->size,
				((unsigned char *)m)[MALLOC_BLOCK_SIZE - 1],
				((unsigned char *)m)[MALLOC_BLOCK_SIZE + m->size],
				m->fn);
		DEL_FROM_LIST(&m->list);
		m->magic = FREE_MAGIC;
		p = m;
	}
#endif
	free(p);
}

void mem_done(void)
{
#ifdef TESTCODE
	if (__unlikely(debug_malloc)) {
		if (__unlikely(!XLIST_EMPTY(&alloc_list))) {
			char badmem = 0;
			MALLOC_BLOCK *m;
			log_printf(1, "MEMORY LEAKED:");
			XLIST_FOR_EACH(m, &alloc_list, MALLOC_BLOCK, list) {
				_eprintf(" %p:%lX @ %p ", m, (unsigned long)m->size, m->fn);
				if (__unlikely(m->magic != MALLOC_MAGIC))
					_eprintf("BAD MAGIC %X!!! ", m->magic), badmem = 1;
				if (__unlikely(((unsigned char *)m)[MALLOC_BLOCK_SIZE - 1] != REDZONE_BEFORE) ||
				    __unlikely(((unsigned char *)m)[MALLOC_BLOCK_SIZE + m->size] != REDZONE_AFTER))
					_eprintf("BAD REDZONE %02X/%02X!!! ",
						((unsigned char *)m)[MALLOC_BLOCK_SIZE - 1],
						((unsigned char *)m)[MALLOC_BLOCK_SIZE + m->size]),
					badmem = 1;
			}
			_eprintf("\n");
			if (__unlikely(badmem)) panic("mem_done: memory corrupted");
		} else {
			status_printf(0, "memory ok.");
		}
	}
#endif
}
