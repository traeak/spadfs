#ifndef _SPADFS_SPADFSCK_SCK_H
#define _SPADFS_SPADFSCK_SCK_H

#ifdef __SPAD__
#include <SYS/TYPES.H>
#include <ARCH/BT.H>
#include <ARCH/BSF.H>
#include <SPAD/LIST.H>
#include <SPAD/LIBC.H>
#include <SPAD/SYNC.H>
#include <SPAD/VM.H>
#include <STDLIB.H>
#include <UNISTD.H>
#include <STRING.H>
#include <TIME.H>
#include <SYS/STAT.H>
#include <ERRNO.H>
#include <FCNTL.H>
#include <STDARG.H>
#include <VALUES.H>
#include "../COMMON.H"
#include "../MOUNTED.H"
#else
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS	64
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include "../common.h"
#include "../mounted.h"
#endif

#if !defined(__linux__)
#undef LINUX_AIO
#endif
#if !defined(_POSIX_ASYNCHRONOUS_IO)
#undef POSIX_AIO
#endif

#ifdef LINUX_AIO
#include <libaio.h>
#endif
#ifdef POSIX_AIO
#include <aio.h>
#endif
#if defined(LINUX_AIO) || defined(POSIX_AIO)
#define AIO
#endif

#if defined(__linux__) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 3))
#define PREFETCH
#endif


#define HASH_SIZE		4096
#define BUFFER_SIZE		131072	/* maximum page size is 131072 */
#define SECTORS_PER_BUFFER	(BUFFER_SIZE >> SSIZE_BITS)
typedef __u64 blk_t;
#define blk_format		__64_format

#define MAX_CC			0xff00

/* this will enable options that have no practical use --- they can be used just
   to test fsck itself */
/*#define TESTCODE*/

#ifdef __linux__
#if !defined(__GLIBC__) || __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 1)
#include <asm/unistd.h>
#ifdef __NR__llseek
#define use_llseek
#endif
#endif
#endif

#ifdef use_llseek
#ifdef pread
#undef pread
#endif
#ifdef pwrite
#undef pwrite
#endif
#define pread xpread
#define pwrite xpwrite
ssize_t xpread(int h, void *buf, size_t count, __u64 off);
ssize_t xpwrite(int h, void *buf, size_t count, __u64 off);
typedef __u64 io_t;
#else
typedef off_t io_t;
#endif

typedef struct __recovery RECOVERY;

#ifndef O_DIRECT
#define O_DIRECT	0
#elif !defined(__SPAD__)
#define UNRELIABLE_O_DIRECT
#endif

#define raw_io(fn, h, ptr, size, pos)	(errno = 0, fn(h, ptr, size, pos) == (size))
#ifndef UNRELIABLE_O_DIRECT
#define io				raw_io
#else
#define io(fn, h, ptr, size, pos)					\
	((__likely((h) == device_handle) &&				\
	  __likely(device_handle_unreliable >= 0) &&			\
	  __likely(raw_io(fn, device_handle_unreliable, ptr, size, pos))) ||\
	 __likely(raw_io(fn, h, ptr, size, pos)))
#endif

/* SCK.C */

extern int ro;
extern int y;
extern int preen;
extern int verbose;
extern int force;
extern int mark_fsck;
extern int extend;
extern int rebuild_apages;
extern int reset_cc;
extern int zero_file_tails;
extern int undelete;
extern int undelete_scan_all;
extern int dont_store_cross_links;
extern const char *swapfile;
extern unsigned long memory_limit;
extern const char *logfile;
extern int sync_writes;
extern __s64 reserve;
extern int nodirect;
extern int nocache;
extern int noprefetch;
extern int noaio;
#ifdef TESTCODE
extern int recover_all_files;
extern int recover_all_directories;
extern int fragment_recovered_files;
extern int move_recovered_files;
extern int dont_sort_recovered_files;
extern int make_apage_bitmaps;
extern int always_swap;
extern int debug_malloc;
#endif
extern const char *dev;
extern int device_handle;
#ifdef UNRELIABLE_O_DIRECT
extern int device_handle_unreliable;
#endif
extern struct stat st;
extern char st_valid;

extern char modified;
extern char reboot_needed;
extern char answered_yes;
extern char answered_no;
extern char was_out_of_memory;
extern char was_read_error;
extern char was_write_error;
extern char was_stdin_error;

/* SCKFN */

void *mem_alloc(size_t s);
void mem_free(void *p);
void mem_done(void);

/* SCKLOG.C */

#define MAX_SAME_ERRORS	5

void __PRINTF_ATTR__(2,3) log_printf(int important, char *msg, ...);
void __PRINTF_ATTR__(2,3) status_printf(int r, char *msg, ...);
#define qprefix __FILE__, __LINE__,
#define Q_IMPORTANT		1
#define Q_NOFIX			2
int __PRINTF_ATTR__(4,5) query(char *file, long line, int flags, char *msg, ...);
void print_progress(blk_t total, blk_t partial, int exact);
void log_suppress(void);
int init_log(void);
int done_log(void);

/* SCKBUF.C */

#define BUFFER_NOCACHE		1
#define BUFFER_FREE_SOON	2
#define BUFFER_TOLERATE_ERROR	4
#define BUFFER_SWAP		8
void *dummy_buffer(void);
void *read_buffer(blk_t blk, unsigned n, int flags);
#if defined(AIO)
#define prefetch_supported	(!noprefetch)
#elif defined(PREFETCH)
#define prefetch_supported	(!noprefetch && nodirect)
#else
#define prefetch_supported	0
#endif
void prefetch_buffer(blk_t blk, int flags);
int write_buffer(blk_t blk, unsigned n, const void *ptr);
int flush_cache(void);
void *rw_swap_buffer(int hn, blk_t blk, int wr);
extern char buffer_error[__MAX_STR_LEN];
void init_buffers(void);
void done_buffers(void);

/* SCKALLOC.C */

extern char alloc_error[__MAX_STR_LEN];
extern int allocation_map_ok;
extern int alloc_over_blocks;
extern blk_t newalloc;
extern int swap_allocmap;
extern unsigned long map_allocated;
extern blk_t blocks_allocated;
void init_alloc(void);
int init_swapfile(void);
void enable_swapping(void);
int check_allocation(blk_t b, blk_t l);
int alloc_blocks(blk_t b, blk_t l);
int free_blocks(blk_t b, blk_t l);
int get_blockrun(blk_t b, int alloced, blk_t *e);
int force_alloc_blocks(blk_t b, blk_t l);
void suggest_free_space(blk_t start, blk_t len);
int alloc_space(int pri, char *message, blk_t n_sec, int mask, int first_fit, blk_t *result);
int alloc_try(blk_t b, blk_t n_sec);
int reset_allocations(void);
void done_alloc(void);

/* SCKXL.C */

extern int cross_link_pass;
extern int were_cross_links;
void init_cross_links(void);
void done_cross_links(void);
int sys_alloc(blk_t blk, blk_t l, int pass, char *name, blk_t *off, const char *(*get_filename)(void *p), void *p);
int sys_free(blk_t blk, blk_t l, int pass);

/* SCKSUPER.C */

extern __u64 super_ptr[64];
#define super (*(struct superblock *)super_ptr)
extern __u64 tx_ptr[64];
#define tx (*(struct txblock *)tx_ptr)
extern __u64 user_ptr[64];
#define user (*(struct userblock *)user_ptr)
extern char tx_valid;
extern unsigned block_mask;
extern unsigned dnode_hash_bits;
extern unsigned dnode_data_size;
extern unsigned dnode_page_sectors;
extern blk_t total_sectors_allocated;

void init_super(void);
int check_super(void);
int recover_super_allocs(void);
int fix_txblock(void);
int fix_errorflag(void);
void write_super(void);
void write_txblock(void);
void done_super(void);

#define magic(block, magic)	((magic) ^ ((__likely(SPAD2CPU32_LV(&super.flags_compat_none) & FLAG_COMPAT_NONE_DYNAMIC_MAGIC)) ? (__u32)(block) : 0))

/* SCKUTIL.C */

const char *escape_name(const char *name);
extern char *ptr_error;
int ptr_valid(blk_t blk, unsigned n_blks);
void done_util(void);

/* SCKCCT.C */

extern __s32 cct[65536];
int check_cct(void);
int check_cct_crc(void);
int check_cct_backup(void);
int repair_cct_backup(void);
int cct_primary_bad(void);
int recover_cct_allocs(void);
int fix_cct(void);
int cc_valid(__u16 cc, __s32 txc);
int cc_check(__u16 *cc, __s32 *txc, char *str1, const char *(*get_filename)(void *p), void *p, blk_t blk);

/* SCKAPAGE.C */

extern unsigned n_apages;
extern blk_t *swapspace;
extern int allow_swapping;
void init_apages(void);
int check_apages(void);
int recover_apage_allocs(void);
int lost_scan(void);
int fix_apages(void);
void done_apages(void);

/* SCKFBLK.C */

typedef struct {
	LIST_ENTRY list;
	blk_t start;
	unsigned len;
	int dno;
} FNODE_DESC;

int get_fnode_description(blk_t blk, LIST_HEAD *l, const char *(*get_filename)(void *p), void *p);
void free_fnode_description(LIST_HEAD *l);
int dealloc_and_free_fnode_description(LIST_HEAD *l);

/* SCKFXFN.C */

struct fixed_fnode_block *update_fixed_fnode_block(struct fixed_fnode_block *fx, blk_t blk);
struct fixed_fnode_block *reread_fixed_fnode_block(blk_t blk);
struct fnode *fixed_fnode_block_fnode(struct fixed_fnode_block *fx, int complementary);
int read_fixed_fnode(blk_t blk, int directory, struct fixed_fnode_block **result_fx, struct fnode **result_fnode, const char *(*get_filename)(void *p), void *p);

/* SCKINO.C */

int ino_claim(__u64 ino);
__u64 ino_get_max(void);

/* SCKHDLNK.C */

int hardlink_link(blk_t blk);
void hardlink_unlink(blk_t blk);
void hardlink_delete(blk_t blk);
void hardlink_set_disk_nlink(blk_t blk, blk_t disk_nlink);
void hardlink_set_recovery(blk_t blk, RECOVERY *rec);
RECOVERY *hardlink_get_recovery(blk_t blk);
int hardlink_search_for_bad_nlink(blk_t *fx_blk, blk_t *real_nlink);
void hardlink_init(void);
void hardlink_done(void);

/* SCKDIR.C */

extern blk_t root_fnode;	/* root fixed fnode */
extern unsigned root_offset;
extern blk_t root_dir;

unsigned rev_bits(unsigned x);
blk_t dnode_decode(struct dnode_page_entry *e);

int check_dir_tree(void);
int adjust_dir_size(blk_t dir_blk, unsigned dir_offset, int delta);
void prefetch_fnode_block(struct fnode_block *f);
void prefetch_dnode(blk_t dnode);
int claim_recovered_directory(char *path, char *filename, blk_t dnode, void **result);
void set_recovered_directory_position(void *result, blk_t blk, unsigned pos);
int undo_recovered_directory(void *result);
void done_dir(void);

/* SCKFILE.C */

typedef struct {
	LIST_ENTRY list;
	blk_t lbn;
	blk_t start;
	blk_t len;
	blk_t ano;
} ALLOC;

int check_last_block(__u64 size, ALLOC *al, const char *(*get_filename)(void *p), void *p);
int check_file(struct fnode *fn, LIST_HEAD *allocs, const char *(*get_filename)(void *p), void *p, __u32 prealloc);
int force_block_alloc_file_runs(LIST_HEAD *l);
int dealloc_blocklist(int r, LIST_HEAD *list, int pass);
int dealloc_and_free_blocklist(int r, LIST_HEAD *list);
void free_file_runs(LIST_HEAD *l);

/* SCKEA.C */

#define EA_TYPE_NAME		1
#define EA_TYPE_FILE		2
#define EA_TYPE_FILE_NAME	3
#define EA_ZERO_PREALLOC	4

char *check_ea(struct fnode *fnode, struct fnode_ea *ea, unsigned size, int type, __u32 *prealloc);
void fix_ea(struct fnode *fnode, struct fnode_ea *ea, unsigned *size, int type);

/* SCKRCV.C */

typedef struct {
	__u64 size;
	__u32 ctime;
	__u32 mtime;
	__u8 flags;
	blk_t dnode;
	__u32 run10;
	__u16 run11;
	__u16 run1n;
	__u32 run20;
	__u16 run21;
	__u16 run2n;
	__u32 anode0;
	__u16 anode1;
	LIST_HEAD runs;
} RECOVERY_FILEDESC;

int skip_recovery_blocks(blk_t b, blk_t l, blk_t *new_b);
void exclude_recovery_blocks(blk_t b, blk_t e, blk_t *new_b, blk_t *new_e);
void init_recovery(void);
int recover_file(RECOVERY_FILEDESC *rfd, const char *file_ea, unsigned file_ea_size, const char *path, const char *filename, const char *name_ea, unsigned name_ea_size, RECOVERY **output);
int add_recovery_hardlink(RECOVERY *rec, const char *path, const char *filename, const char *name_ea, unsigned name_ea_size);
int do_directories_recovery(void);
int do_files_recovery(void);
void done_recovery(void);

/* SCKCRT.C */

int add_fnode_to_dir(const char *path, const char *filename, const struct fnode *fn, const struct fnode_ea *ea, unsigned ea_size, blk_t *result_block, unsigned *result_offset);
int create_empty_directory(blk_t *result, unsigned *size);
int create_fixed_fnode(const struct fnode *new_fnode, const struct fnode_ea *ea, unsigned ea_size, blk_t *result);
int file_exists_in_root(const char *file);

/* SCKMASS.C */

void mass_scan_init(void);
void mass_scan_done(void);
int mass_scan_prepare(blk_t total_blocks);
void mass_scan_set_range(blk_t start, blk_t end);
void mass_scan_do(void);
void mass_scan_sort(void);
int lost_recover(void);

#endif
