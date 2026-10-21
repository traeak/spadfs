#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#ifdef TESTCODE
#include <sys/mman.h>
#endif

#ifdef LINUX_AIO
#define N_AIO_OPERATIONS	256
static io_context_t aio_ctx;
static char linux_aio_enabled;
#endif

typedef struct {
	LIST_ENTRY hash;
	LIST_ENTRY lru;
	blk_t blk;
	char *data;
	int h;
	char flags;
	char dirty;
#ifdef AIO
	char readahead;
#endif
#ifdef POSIX_AIO
	struct aiocb aio;
#endif
#ifdef LINUX_AIO
	struct iocb io_cb;
#endif
} BUFFER;

#define HASH(hn, x)		(((x) / SECTORS_PER_BUFFER + (unsigned)(hn) * (HASH_SIZE / 2)) & (HASH_SIZE - 1))

static XLIST_HEAD buffer_hash[HASH_SIZE];
static LIST_HEAD buffer_lru;
#ifdef __GNUC__
__attribute__((__aligned__(32768)))
#endif
static __u64 xdata[BUFFER_SIZE / 8];
static BUFFER *pinned_buffer;
static BUFFER *just_freed;
static unsigned long buffer_memory;

char buffer_error[__MAX_STR_LEN];

static void free_buffer(BUFFER *b);

static int is_readahead(BUFFER *b)
{
#ifdef POSIX_AIO
	if (__unlikely(b->readahead)) return aio_error(&b->aio) == EINPROGRESS;
#endif
	return 0;
}

static int wait_on_readahead(BUFFER *b)
{
#ifdef LINUX_AIO
	if (__likely(linux_aio_enabled)) {
		if (!b || __unlikely(b->readahead)) {
			static struct io_event ev[N_AIO_OPERATIONS];
			int r, i, found;
			struct timespec ts;
wait_again:
			ts.tv_sec = ts.tv_nsec = 0;
			r = io_getevents(aio_ctx, 1, N_AIO_OPERATIONS, ev, b ? NULL : &ts);
			if (__unlikely(r < 0)) {
				if (r == -EINTR)
					goto wait_again;
				log_printf(1, "ASYNC IO ERROR: io_getevents %d", r);
				exit(8);	/* there is no reasonable way to recover */
			}
			found = 0;
			for (i = 0; i < r; i++) {
				BUFFER *cb = __LIST_STRUCT_NOCHECK(ev[i].obj, BUFFER, io_cb);
				if (cb->readahead != 1) panic("wait_on_readahead: buffer not under readahead: %d", cb->readahead);
				cb->readahead = 0;
				if (__unlikely(cb->io_cb.u.c.nbytes != BUFFER_SIZE)) {
					if (cb == b)
						found = 2;
					free_buffer(cb);
				} else {
					if (cb == b)
						found = 1;
				}
			}
			if (b) {
				if (__unlikely(found == 2))
					return 1;
				if (!found)
					goto wait_again;
			}
		}
		return 0;
	}
#endif
#ifdef POSIX_AIO
	if (__unlikely(b->readahead)) {
		int er;
		ssize_t ret;
		while ((er = aio_error(&b->aio)) == EINPROGRESS) {
			const struct aiocb *ptr = &b->aio;
			aio_suspend(&ptr, 1, NULL);
		}
		b->readahead = 0;
		ret = aio_return(&b->aio);
		if (er || ret != BUFFER_SIZE) {
			free_buffer(b);
			return 1;
		}
	}
#endif
	return 0;
}

static void claim_write_error(int h, blk_t blk, unsigned len, int err)
{
	was_write_error = 1;
	if (!err) _snprintf(buffer_error, sizeof buffer_error, "WRITING BEYOND END OF DEVICE (%"blk_format"X,%X)", blk, SECTORS_PER_BUFFER), err = EEOF;
	else _snprintf(buffer_error, sizeof buffer_error, "ERROR WRITING TO SECTORS (%"blk_format"X,%X): %s", blk, SECTORS_PER_BUFFER, strerror(err));
	log_printf(1, "%s", buffer_error);
}

static __finline__ char *alloc_buffer_data(void)
{
	return valloc(BUFFER_SIZE);
}

static __finline__ void free_buffer_data(char *ptr)
{
	free(ptr);
}

static __finline__ void check_buffer_memory(void)
{
	BUFFER *b;
/*{
	static unsigned long am = 0;
	if (map_allocated > am) __debug_printf("%ld   \n", map_allocated), am = map_allocated;
}*/
	if (
#ifdef TESTCODE
		always_swap ||
#endif
		(__unlikely(buffer_memory + map_allocated >= memory_limit) &&
		 __unlikely(map_allocated > memory_limit / 2))) enable_swapping();
	test_again:
	b = LIST_STRUCT(buffer_lru.next, BUFFER, lru);
	if (__unlikely(b == pinned_buffer)) b = LIST_STRUCT(b->lru.next, BUFFER, lru);
	if (__unlikely(b == __LIST_STRUCT_NOCHECK(&buffer_lru, BUFFER, lru))) return;
	if (__unlikely(b->flags & BUFFER_FREE_SOON) && __likely(!is_readahead(b))) {
		free_buffer(b);
		goto test_again;
	}
	if (__unlikely(buffer_memory + map_allocated >= memory_limit)) {
		if (__unlikely(buffer_memory < memory_limit / 4)) return;
		free_buffer(b);
		goto test_again;
	}
}

static BUFFER *find_buffer(int hn, blk_t blk, int wait)
{
	BUFFER *b;
	XLIST_FOR_EACH(b, &buffer_hash[HASH(hn, blk)], BUFFER, hash) if (__likely(b->blk == (blk & ~(blk_t)(SECTORS_PER_BUFFER - 1))) && __likely(b->h == hn)) {
		if (!wait) return b;
		if (__unlikely(wait_on_readahead(b))) return NULL;
		DEL_FROM_LIST(&b->lru);
		if (!(b->flags & BUFFER_FREE_SOON))
			ADD_TO_LIST_END(&buffer_lru, &b->lru);
		else
			ADD_TO_LIST(&buffer_lru, &b->lru);
		/*fprintf(stderr, "find: %Lx\n", b->blk);*/
		return b;
	}
	return NULL;
}

static int io_near_end(blk_t blk, blk_t len)
{
	return blk + len < blk ||
		blk + len >= SPAD2CPU64_LV(&super.size);
}

static BUFFER *alloc_buffer(int hn, blk_t blk, int flags)
{
	BUFFER *b;
	check_buffer_memory();
	if (__unlikely(flags & BUFFER_NOCACHE)) {
		errno = EOPNOTSUPP;
		return NULL;
	}
	blk &= ~(blk_t)(SECTORS_PER_BUFFER - 1);
	if (!(flags & BUFFER_SWAP) && io_near_end(blk, SECTORS_PER_BUFFER)) {
		errno = EOPNOTSUPP;
		return NULL;
	}
	if (just_freed) {
		b = just_freed;
		just_freed = NULL;
	} else {
		b = mem_alloc(sizeof(BUFFER));
		if (__unlikely(!b)) goto ret_null;
		b->data = alloc_buffer_data();
	}
	b->blk = blk;
	b->h = hn;
	b->flags = flags;
	b->dirty = 0;
#ifdef AIO
	b->readahead = 0;
#endif
	if (__unlikely(!b->data)) goto free_b_ret_null;
	buffer_memory += BUFFER_SIZE;
	if (__likely(!(flags & BUFFER_FREE_SOON)))
		ADD_TO_LIST_END(&buffer_lru, &b->lru);
	else
		ADD_TO_LIST(&buffer_lru, &b->lru);
	ADD_TO_XLIST(&buffer_hash[HASH(hn, blk)], &b->hash);
	return b;

	free_b_ret_null:
	mem_free(b);
	ret_null:
	return NULL;
}

static BUFFER *read_buffer_internal(int hn, blk_t blk, int flags)
{
	BUFFER *b;
	if (__likely((b = find_buffer(hn, blk, 1)) != NULL)) {
		return b;
	}

	b = alloc_buffer(hn, blk, flags);
	if (__unlikely(!b))
		return NULL;

	if (__unlikely(!io(pread, hn, b->data, BUFFER_SIZE, (io_t)b->blk << SSIZE_BITS))) {
		free_buffer(b);
		return NULL;
	}

	return b;
}

void *read_buffer(blk_t blk, unsigned n, int flags)
{
	BUFFER *b;
	void *bdata;
	if (__unlikely(((unsigned)blk & (SECTORS_PER_BUFFER - 1)) + n > SECTORS_PER_BUFFER)) {
		_snprintf(buffer_error, sizeof buffer_error, "UNALIGNED READ (%"blk_format"X,%X)", blk, n);
		return NULL;
	}
	pinned_buffer = NULL;
	b = read_buffer_internal(device_handle, blk, flags | (nocache * BUFFER_NOCACHE));
	if (__likely(b != NULL)) {
		pinned_buffer = b;
		return b->data + (((unsigned)blk & (SECTORS_PER_BUFFER - 1)) << SSIZE_BITS);
	}
	bdata = dummy_buffer();
	if (__unlikely(!io(pread, device_handle, bdata, n << SSIZE_BITS, (io_t)blk << SSIZE_BITS))) {
		/*_eprintf("invalid access: %Lx,%x (max %Lx)\n", blk, n, SPAD2CPU64_LV(&super.size));
		KERNEL$STACK_DUMP();*/
		if (!(flags & BUFFER_TOLERATE_ERROR)) {
			was_read_error = 1;
			if (!errno) _snprintf(buffer_error, sizeof buffer_error, "ACCESS BEYOND END OF DEVICE (%"blk_format"X,%X)", blk, n);
			else _snprintf(buffer_error, sizeof buffer_error, "ERROR %s (%"blk_format"X,%X)", strerror(errno), blk, n);
		}
		return NULL;
	}
	return bdata;
}

void prefetch_buffer(blk_t blk, int flags)
{
#ifdef AIO
	BUFFER *b;
	int h = device_handle_unreliable >= 0 ? device_handle_unreliable : device_handle;
	if (__unlikely(flags & BUFFER_NOCACHE) || __unlikely(nocache) || __unlikely(noprefetch) || __unlikely(noaio))
		goto prefetch_syscall;
#if defined(LINUX_AIO) && !defined(POSIX_AIO)
	if (__unlikely(!linux_aio_enabled))
		goto prefetch_syscall;
#endif
	b = find_buffer(device_handle, blk, 0);
	if (__likely(b != NULL)) return;
	b = alloc_buffer(device_handle, blk, flags);
	if (__unlikely(!b)) return;
	DEL_FROM_LIST(&b->lru);
	ADD_TO_LIST_END(&buffer_lru, &b->lru);
	b->readahead = 1;
#ifdef LINUX_AIO
	if (linux_aio_enabled) {
		struct iocb *iocb;
		wait_on_readahead(NULL);
		iocb = &b->io_cb;
		memset(iocb, 0, sizeof b->io_cb);
		io_prep_pread(&b->io_cb, h, b->data, BUFFER_SIZE, (io_t)b->blk << SSIZE_BITS);
		if (__unlikely(io_submit(aio_ctx, 1, &iocb) != 1)) {
			b->readahead = 0;
			free_buffer(b);
		}
		return;
	}
#endif
#ifdef POSIX_AIO
	memset(&b->aio, 0, sizeof b->aio);
	b->aio.aio_fildes = h;
	b->aio.aio_buf = b->data;
	b->aio.aio_nbytes = BUFFER_SIZE;
	b->aio.aio_offset = (io_t)b->blk << SSIZE_BITS;
	b->aio.aio_sigevent.sigev_notify = SIGEV_NONE;
#ifdef _POSIX_PRIORITIZED_IO
	b->aio.aio_reqprio = 0;
#endif
	if (__unlikely(aio_read(&b->aio))) {
		b->readahead = 0;
		free_buffer(b);
	}
	return;
#endif
prefetch_syscall:;
#endif
#if defined(PREFETCH)
	{
		BUFFER *b;
		static blk_t last = 0;
		if (__unlikely(flags & BUFFER_NOCACHE) || __unlikely(!nodirect) || __unlikely(noprefetch))
			return;
		blk &= ~(blk_t)(SECTORS_PER_BUFFER - 1);
		b = find_buffer(device_handle, blk, 0);
		if (__likely(b != NULL)) return;
		if (blk == last) return;
		last = blk;
		readahead(device_handle, (__u64)blk << SSIZE_BITS, BUFFER_SIZE);
	}
#endif
}

void *dummy_buffer(void)
{
#ifdef TESTCODE
	if (__unlikely(debug_malloc)) {
		unsigned long addr;
		static void *mmapped_buffer = NULL;
		if (__likely(mmapped_buffer != NULL)) {
			munmap(mmapped_buffer, BUFFER_SIZE);
			mmapped_buffer = NULL;
		}
		addr = random() & ~(unsigned long)(getpagesize() - 1);
		mmapped_buffer = mmap((void *)addr, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (__likely(mmapped_buffer != MAP_FAILED))
			return mmapped_buffer;
		mmapped_buffer = NULL;
	}
#endif
	return xdata;
}

static void free_buffer(BUFFER *b)
{
	if (__unlikely(wait_on_readahead(b))) return;
	if (__unlikely(b == pinned_buffer)) pinned_buffer = NULL;
	if (__unlikely(b->dirty)) {
		if (__unlikely(!io(pwrite, b->h, b->data, BUFFER_SIZE, b->blk << SSIZE_BITS))) {
			claim_write_error(b->h, b->blk, SECTORS_PER_BUFFER, errno);
		}
	}
	DEL_FROM_LIST(&b->lru);
	DEL_FROM_LIST(&b->hash);
	buffer_memory -= BUFFER_SIZE;
	if (!just_freed) {
		just_freed = b;
	} else {
		free_buffer_data(b->data);
		mem_free(b);
	}
}

int write_buffer(blk_t blk, unsigned n, const void *ptr)
{
	int r;
	blk_t bblk;
	unsigned nn, nx, oof;
	const char *pptr;
	BUFFER *b;
	if (__unlikely(ro)) panic("write_buffer: writing when read-only");

	retry:
	bblk = blk, nn = n, pptr = ptr;
	next_prune:
	oof = (unsigned)bblk & (SECTORS_PER_BUFFER - 1);
	nx = SECTORS_PER_BUFFER - oof;
	if (__likely(nx >= nn)) nx = nn;
	if (__likely((b = find_buffer(device_handle, bblk, 1)) != NULL)) {
		if ((char *)pptr >= b->data && (char *)pptr < b->data + BUFFER_SIZE) {
			if (__unlikely((char *)pptr != b->data + (oof << SSIZE_BITS))) {
				void *tmp_data = alloca(n << SSIZE_BITS);
				memcpy(tmp_data, ptr, n << SSIZE_BITS);
				ptr = tmp_data;
				goto retry;
			}
			goto skip_prune;
		} else {
			memcpy(b->data + (oof << SSIZE_BITS), pptr, nx << SSIZE_BITS);
			/*free_buffer(b);*/
		}
	}
	if (__unlikely(nx < nn)) {
		nn -= nx;
		bblk += nx;
		pptr += nx << SSIZE_BITS;
		goto next_prune;
	}

	skip_prune:
	modified = 1;
	if (blk != USERBLOCK_SECTOR) reboot_needed = 1;
	if (__unlikely(!io(pwrite, device_handle, ptr, n << SSIZE_BITS, (io_t)blk << SSIZE_BITS))) {
		r = -errno;
		claim_write_error(device_handle, blk, n, -r);
		goto skip_sync;
	}
	if (__unlikely(sync_writes)) {
		if (__unlikely(fdatasync(device_handle))) {
			r = -errno;
			_snprintf(buffer_error, sizeof buffer_error, "ERROR FLUSHING CACHE AFTER WRITE TO SECTORS (%"blk_format"X,%X): %s", blk, n, strerror(-r));
			log_printf(1, "%s", buffer_error);
		}
	}
	r = 0;
	skip_sync:
	return r;
}

int flush_cache(void)
{
	if (__unlikely(fsync(device_handle))) {
		was_write_error = 1;
		log_printf(1, "ERROR FLUSHING CACHE: %s", strerror(errno));
		return 1;
	}
	return 0;
}

void *rw_swap_buffer(int hn, blk_t blk, int wr)
{
	BUFFER *b;
	b = read_buffer_internal(hn, blk, BUFFER_SWAP);
	if (__unlikely(!b)) {
		if (!errno) _snprintf(buffer_error, sizeof buffer_error, "ACCESS BEYOND END OF DEVICE (%"blk_format"X)", blk);
		else _snprintf(buffer_error, sizeof buffer_error, "ERROR %s (%"blk_format"X)", strerror(errno), blk);
		return NULL;
	}
	if (__likely(wr) && __unlikely(!b->dirty)) b->dirty = 1;
	return b->data;
}

void init_buffers(void)
{
	unsigned i;
	for (i = 0; i < HASH_SIZE; i++) INIT_XLIST(&buffer_hash[i]);
	INIT_LIST(&buffer_lru);
	pinned_buffer = NULL;
	just_freed = NULL;
	buffer_memory = 0;
#ifdef LINUX_AIO
	linux_aio_enabled = !nodirect &&
#ifdef UNRELIABLE_O_DIRECT
		device_handle_unreliable >= 0 &&
#endif
		!io_setup(N_AIO_OPERATIONS, &aio_ctx);
#endif
}

void done_buffers(void)
{
	unsigned i;
	for (i = 0; i < HASH_SIZE; i++) while (!XLIST_EMPTY(&buffer_hash[i])) {
		BUFFER *b = LIST_STRUCT(buffer_hash[i].next, BUFFER, hash);
		if (__unlikely(b->dirty == 1)) b->dirty = 0;
		free_buffer(b);
	}
	pinned_buffer = NULL;
	if (just_freed) {
		free_buffer_data(just_freed->data);
		mem_free(just_freed);
		just_freed = NULL;
	}
	if (__unlikely(buffer_memory != 0)) panic("done_buffers: %lx buffer memory leaked", buffer_memory);
	if (__unlikely(!LIST_EMPTY(&buffer_lru))) panic("done_buffers: buffer_lru not empty");
#ifdef LINUX_AIO
	if (linux_aio_enabled) {
		int er = io_destroy(aio_ctx);
		if (__unlikely(er)) {
			log_printf(1, "ASYNC IO ERROR: io_destroy %d", er);
		}
	}
#endif
}
