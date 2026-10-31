#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

typedef struct __subdir SUBDIR;

struct __subdir {
	LIST_ENTRY list;
	LIST_ENTRY todolist;
	LIST_HEAD subdirs;
	LIST_HEAD blocks;
	blk_t this_fnode;
	unsigned this_offset;
	int file;
	__u64 dir_size;
	blk_t sub_fnode;
	SUBDIR *parent;
	long dir_depth;
	int dnode_depth;
	__u32 hash_val;
	__u32 hash_mask;
	char name[1];
};

blk_t root_fnode;
unsigned root_offset;
blk_t root_dir;

static SUBDIR root;
static LIST_HEAD todolist;

static int process_node(SUBDIR *d);
static int check_fnode_block(SUBDIR *d, blk_t blk, XLIST_HEAD *filenames);
static int check_dnode(SUBDIR *d, blk_t blk);
static int check_dir_size(SUBDIR *d);
static void free_node(SUBDIR *d);
static void free_tree(void);
static const char *get_filename(void *d_);

#define FILE_HASH_SIZE		(64 * 1024 * 1024)
static __u32 file_hash[FILE_HASH_SIZE / 32] = { 0 };

static void file_hash_init(void)
{
	unsigned i;
	for (i = 0; i < FILE_HASH_SIZE / 32; i++)
		if (__unlikely(file_hash[i] != 0))
			file_hash[i] = 0;
}

static int make_new_root(void)
{
	int r;
	blk_t fnode_block;
	blk_t fixed_fnode_block;
	struct fnode root;
	unsigned root_directory_size;

	if (!query(qprefix 0, "MAKE NEW ROOT?")) return 1;

	if (__unlikely(r = create_empty_directory(&fnode_block, &root_directory_size))) return r;

	memset(&root, 0, sizeof root);
	CPU2SPAD16_LV(&root.cc, 0);
	CPU2SPAD32_LV(&root.txc, 0);
	CPU2SPAD64_LV(&root.size[0], SPAD2CPU32_LV(&super.flags_compat_ro) & FLAG_COMPAT_RO_DIRECTORY_SIZES ? root_directory_size << SSIZE_BITS : 0);
	root.ctime = root.mtime = CPU2SPAD32(time(NULL));
	root.run10 = MAKE_PART_0(fnode_block);
	root.run11 = MAKE_PART_1(fnode_block);
	root.flags = FNODE_FLAGS_DIR;
	if (__unlikely(r = create_fixed_fnode(&root, NULL, 0, &fixed_fnode_block))) return r;
	CPU2SPAD64_LV(&super.root, fixed_fnode_block);
	write_super();

	/* they will be allocated right away */
	free_blocks(fixed_fnode_block, 1 << super.sectors_per_block_bits);
	free_blocks(fnode_block, root_directory_size);

	return 0;
}

static void print_alloc_progress(void)
{
	blk_t total = total_sectors_allocated >> super.sectors_per_block_bits;
	if (__unlikely(blocks_allocated > total)) total = SPAD2CPU64_LV(&super.size) >> super.sectors_per_block_bits;
	print_progress(total, blocks_allocated, 1);
}

int check_dir_tree(void)
{
	int r;
	struct fixed_fnode_block *fx;
	struct fnode *fnode;
	unsigned ea_size;
	char *ea_msg;
	blk_t blk, nlink;
	int root_recovered = 0;

	status_printf(0, "checking directories and files...");

	again:
	file_hash_init();
	mass_scan_init();
	hardlink_init();
	INIT_LIST(&todolist);
	memset(&root, 0, sizeof(SUBDIR));
	INIT_LIST(&root.subdirs);
	root.this_fnode = root_fnode = SPAD2CPU64_LV(&super.root);
	root.dir_size = 0;

	r = read_fixed_fnode(root.this_fnode, 1, &fx, &fnode, get_filename, &root);
	if (__unlikely(r)) {
		if (r != 2) {
			hardlink_done();
			mass_scan_done();
			log_printf(0, "CAN'T READ ROOT FNODE");
			recover_root:
			if (!root_recovered) {
				if (preen) {
					log_printf(1, "CAN'T REPAIR ROOT DIRECTORY IN AUTOMATIC MODE, RUN FSCK MANUALLY");
					return 1;
				}
				root_recovered = 1;
				r = make_new_root();
				if (!r) goto again;
			} else r = 1;
			return r;
		}
		done_ret_r:
		hardlink_done();
		mass_scan_done();
		return r;
	}
	root.this_offset = root_offset = (char *)fnode - (char *)fx;

	ea_size = (SPAD2CPU16_LV(&fnode->next) & FNODE_NEXT_SIZE) - FNODE_EA_POS(0);
	if (__unlikely((ea_msg = check_ea(fnode, (struct fnode_ea *)((char *)fnode + FNODE_EA_POS(0)), ea_size, EA_TYPE_FILE, NULL)) != NULL)) {
		log_printf(0, "%s: INVALID EXTENDED ATTRIBUTES: %s", get_filename(&root), ea_msg);
		if (query(qprefix 0, "FIX?")) {
			fix_ea(fnode, (struct fnode_ea *)((char *)fnode + FNODE_EA_POS(0)), &ea_size, EA_TYPE_FILE);
			CPU2SPAD16_LV(&fnode->next, (SPAD2CPU16_LV(&fnode->next) & ~FNODE_NEXT_SIZE) | (FNODE_EA_POS(0) + ea_size));
			if (__unlikely(!(fx = update_fixed_fnode_block(fx, SPAD2CPU64_LV(&super.root))))) {
				log_printf(0, "%s: ERROR UPDATING FIXED FNODE: %s", get_filename(&root), buffer_error);
				r = 1;
				goto done_ret_r;
			}
			fnode = fixed_fnode_block_fnode(fx, 0);
		}
	}

	if (cc_valid(fnode->cc, fnode->txc)) root_dir = MAKE_D_OFF(fnode->run10, fnode->run11);
	else root_dir = MAKE_D_OFF(fnode->run20, fnode->run21);
	root.sub_fnode = root_dir;

	r = get_fnode_description(root_dir, &root.blocks, get_filename, &root);
	if (__unlikely(r)) {
		if (r != 2) {
			hardlink_done();
			mass_scan_done();
			log_printf(0, "CAN'T ALLOCATE ROOT DIRECTORY");
			free_blocks(root.this_fnode, 1 << super.sectors_per_block_bits);
			goto recover_root;
		}
		goto done_ret_r;
	}

	root_recovered = 1;

	ADD_TO_LIST(&todolist, &root.todolist);
	process_todolist:
	while (!LIST_EMPTY(&todolist)) {
		SUBDIR *s = LIST_STRUCT(todolist.next, SUBDIR, todolist);
		DEL_FROM_LIST(&s->todolist);
		if (__unlikely(r = process_node(s))) {
			free_tree();
			goto done_ret_r;
		}
		while (LIST_EMPTY(&s->subdirs)) {
			SUBDIR *p;
			check_dir_size(s);
			if (__unlikely(s == &root)) break;
			p = s->parent;
			free_node(s);
			s = p;
		}
		print_alloc_progress();
	}
	if (__likely(!cross_link_pass)) {
	/* If we are read-only, cross link passes would cause infinite loop */
		if (!ro && __unlikely(were_cross_links)) {
			log_printf(0, "CROSS LINKED FILES DETECTED, PROCESSING THEM IN NEXT TWO PASSES");
			cross_link_pass = 1;
			free_tree();
			hardlink_done();
			mass_scan_done();
			if (__unlikely(r = reset_allocations())) return r;
			status_printf(0, "checking for cross linked metadata...");
			goto again;
		}
	}
	if (__unlikely(cross_link_pass == 1)) {
		cross_link_pass = 2;
		free_tree();
		hardlink_done();
		status_printf(0, "checking for cross linked data...");
		goto again;
	}
	free_tree();

	/* Note: once we recover something, we may no longer do any cross-link
	   passes, this would jam allocations and corrupt recovered data */
	if (cross_link_pass < 3) cross_link_pass = 3;
	if (__unlikely(r = do_directories_recovery())) goto done_ret_r;
	if (__unlikely(!LIST_EMPTY(&todolist))) {
		status_printf(0, "checking recently recovered directories...");
		goto process_todolist;
	}

	if (cross_link_pass == 3) {
		cross_link_pass = 4;
		if (__unlikely(r = lost_scan())) goto done_ret_r;
	}

	if (cross_link_pass == 4) {
		if (__unlikely(r = lost_recover())) goto done_ret_r;
		if (__unlikely(r = do_directories_recovery())) goto done_ret_r;
		if (__unlikely(!LIST_EMPTY(&todolist))) {
			status_printf(0, "checking lost and recovered directories...");
			goto process_todolist;
		}
	}

	while (__unlikely(hardlink_search_for_bad_nlink(&blk, &nlink))) {
		struct fixed_fnode_block *fx;
		struct fnode *fx_fn;
		fx = reread_fixed_fnode_block(blk);
		if (__unlikely(!fx)) {
			log_printf(1, "ERROR READING FIXED FNODE BLOCK %"blk_format"X: %s", blk, buffer_error);
			log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN CORRECT BEFORE");
			continue;
		}
		fx_fn = fixed_fnode_block_fnode(fx, 0);
		log_printf(0, "WRONG NLINK FOR FIXED FNODE BLOCK %"blk_format"X: %"blk_format"X, SHOULD BE %"blk_format"X", blk, (blk_t)SPAD2CPU64_LV(FIXED_FNODE_NLINK_PTR(fx_fn)), nlink);
		if (__unlikely(SPAD2CPU64_LV(FIXED_FNODE_NLINK_PTR(fx_fn)) == nlink)) {
			log_printf(1, "SOMETHING WEIRD HAPPENED, LINK COUNT WAS FIXED UNDER US");
			log_printf(1, "THE DEVICE IS UNRELIABLE");
		}
		if (query(qprefix 0, "FIX?")) {
			CPU2SPAD64_LV(FIXED_FNODE_NLINK_PTR(fx_fn), nlink);
			if (__unlikely(!update_fixed_fnode_block(fx, blk))) {
				log_printf(1, "ERROR UPDATING FIXED FNODE BLOCK %"blk_format"X: %s", blk, buffer_error);
				log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN CORRECT BEFORE");
				continue;
			}
		}
	}
	hardlink_done();
	mass_scan_done();
	if (__unlikely(r = do_files_recovery())) return r;
	return 0;
}

static int process_node(SUBDIR *d)
{
	int r;
	DECL_XLIST(filenames);
	FNODE_DESC *fd;
	SUBDIR *base = d;
	while (base->dnode_depth) base = base->parent;
	LIST_FOR_EACH(fd, &d->blocks, FNODE_DESC, list) {
		unsigned c;
		if (__unlikely(fd->dno)) {
			base->dir_size += dnode_page_sectors << SSIZE_BITS;
			r = check_dnode(d, fd->start);
			goto free_ret_r;
		}
		base->dir_size += fd->len << SSIZE_BITS;
		for (c = 0; c < fd->len; c++) {
			r = check_fnode_block(d, fd->start + c, &filenames);
			if (__unlikely(r)) goto free_ret_r;
		}
	}
	r = 0;
	free_ret_r:
	while (!XLIST_EMPTY(&filenames)) {
		SUBDIR *sd = LIST_STRUCT(filenames.next, SUBDIR, list);
		DEL_FROM_LIST(&sd->list);
		mem_free(sd);
	}
	return r;
}

static int check_fnode_block(SUBDIR *d, blk_t blk, XLIST_HEAD *filenames)
{
	int recover_all = 0;
	unsigned fp;
	struct fnode_block *f = read_buffer(blk, 1, 0);
	if (__unlikely(!f)) {
		read_err:
		log_printf(1, "%s: ERROR ACCESSING FNODE BLOCK %"blk_format"X: %s", get_filename(d), blk, buffer_error);
		log_printf(1, "THE DEVICE IS UNRELIABLE, FNODE HAS BEEN SUCCESSFULY READ BEFORE");
		return 1;
	}
#define reread_block							\
do {									\
	if (__unlikely(!(f = read_buffer(blk, 1, 0)))) goto read_err;	\
} while (0)
#define update_block							\
do {									\
	f->flags |= FNODE_BLOCK_CHECKSUM_VALID;				\
	f->checksum ^= CHECKSUM_BASE ^ __byte_sum(f, FNODE_BLOCK_SIZE);	\
	if (__unlikely(write_buffer(blk, 1, f))) goto read_err;		\
	reread_block;							\
} while (0)
	if (__unlikely(SPAD2CPU32_LV(&f->magic) != magic(blk, FNODE_BLOCK_MAGIC))) {
		log_printf(1, "%s: BAD MAGIC WHEN RE-READING FNODE BLOCK %"blk_format"X: %08X", get_filename(d), blk, (unsigned)SPAD2CPU32_LV(&f->magic));
		log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
		return 1;
	}
	if (f->flags & FNODE_BLOCK_CHECKSUM_VALID && __unlikely(__byte_sum(f, FNODE_BLOCK_SIZE) != CHECKSUM_BASE)) {
		log_printf(0, "%s: BAD CHECKSUM ON FNODE BLOCK %"blk_format"X: %02X", get_filename(d), blk, __byte_sum(f, FNODE_BLOCK_SIZE) ^ CHECKSUM_BASE);
		if (query(qprefix 0, "FIX?")) {
			update_block;
		}
		if (query(qprefix Q_NOFIX, "DELAY PROCESSING ALL REFERENCES FROM THIS BLOCK?")) {
			recover_all |= 1;
		}
	}
	fp = SIZEOF_FNODE_BLOCK;
#define fn ((struct fnode *)((char *)f + fp))
	do {
		int i;
		int dot;
		__u32 hash;
		unsigned size;
		unsigned eapos;
		static char filename[MAX_NAME_LEN + 1];
		SUBDIR *subdir;
		int different_hash_error, duplicate_filename_error, ea_error;
		blk_t link;
		int link_free_space;
		struct fixed_fnode_block *fx;
		struct fnode *fx_fn;
		static struct fnode f1;
		static RECOVERY_FILEDESC rfd;
		static struct {
			__u64 force_alignment;
			char ea[FNODE_MAX_EA_SIZE];
		} ea;
		unsigned ea_size;
		static struct {
			__u64 force_alignment;
			char name_ea[FNODE_MAX_EA_SIZE];
		} name_ea;
		unsigned name_ea_size;
		char *ea_msg;
		__u32 prealloc;

		link = 0;
		link_free_space = 0;

		size = SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_SIZE;

		if (__unlikely(cc_check(&fn->cc, &fn->txc, "FNODE", get_filename, d, blk))) update_block;
		if (cc_valid(fn->cc, fn->txc) && SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_FREE) {
			if (__unlikely(size < FNODE_HEAD_SIZE) || __unlikely(fp + size > FNODE_BLOCK_SIZE)) {
				if (recover_all & 2) goto delete_invalid_fnode;
				log_printf(0, "%s: FNODE BLOCK %"blk_format"X: INVALID FREE FNODE SIZE %X AT %X", get_filename(d), blk, SPAD2CPU16_LV(&fn->next), fp);
				if (query(qprefix 0, "FIX?")) {
					delete_invalid_fnode:
					CPU2SPAD16_LV(&fn->cc, 0);
					CPU2SPAD32_LV(&fn->txc, 0);
					size = FNODE_BLOCK_SIZE - fp;
					if (recover_all & 2 || query(qprefix Q_NOFIX, "TRY TO FIND MORE DIRECTORY ENTRIES IN THIS BLOCK?")) {
						recover_all |= 2;
						size = FNODE_EMPTY_MIN_SIZE;
					}
					CPU2SPAD16_LV(&fn->next, (SPAD2CPU16_LV(&fn->next) & ~FNODE_NEXT_SIZE) | size | FNODE_NEXT_FREE);
					update_block;
					goto cont;
				}
				return 0;
			}
			goto cont;
		}
		if (__unlikely(size <= FNODE_NAME_POS) || __unlikely(fp + size > FNODE_BLOCK_SIZE) || __unlikely(FNODE_NAME_POS + fn->namelen > size) || __unlikely(!fn->namelen) || __unlikely(size - FNODE_EA_POS(fn->namelen) > FNODE_MAX_EA_SIZE)) {
			if (recover_all & 2) goto delete_invalid_fnode;
			log_printf(0, "%s: FNODE BLOCK %"blk_format"X: INVALID FNODE SIZE %X AT %X, NAMELEN %X", get_filename(d), blk, SPAD2CPU16_LV(&fn->next), fp, fn->namelen);
			if (query(qprefix 0, "DELETE?")) goto delete_invalid_fnode;
			return 0;
		}
		if (__unlikely((fn->flags & (FNODE_FLAGS_DIR | FNODE_FLAGS_HARDLINK)) == (FNODE_FLAGS_DIR | FNODE_FLAGS_HARDLINK))) {
			if (recover_all & 2) goto delete_invalid_fnode;
			log_printf(0, "%s: FNODE BLOCK %"blk_format"X: INVALID FNODE FLAGS %02X AT %X", get_filename(d), blk, (unsigned)fn->flags, fp);
			goto query_delete;
		}
		different_hash_error = 0;
		duplicate_filename_error = 0;
		ea_error = 0;
		memcpy(filename, FNODE_NAME(fn), fn->namelen);
		filename[fn->namelen] = 0;
		for (i = 0; i < fn->namelen; i++) if (__unlikely(INVALID_FILENAME_CHARACTER(i, FNODE_NAME(fn)[i], SPAD2CPU32_LV(&super.flags_compat_none) & FLAG_COMPAT_NONE_UNIX_NAMES))) {
			dot = 0;
			invl_name:
			log_printf(0, "%s: FNODE BLOCK %"blk_format"X: FNODE AT %X HAS INVALID FILENAME \"%s\"", get_filename(d), blk, fp, escape_name(filename));
			for (; i < fn->namelen; i++) if (dot || INVALID_FILENAME_CHARACTER(i, FNODE_NAME(fn)[i], SPAD2CPU32_LV(&super.flags_compat_none) & FLAG_COMPAT_NONE_UNIX_NAMES)) {
				if (FNODE_NAME(fn)[i] >= 'a' && FNODE_NAME(fn)[i] <= 'z') filename[i] -= 0x20;
				else filename[i] = '_';
			}
			duplicate_filename_error = 1;
			goto name_ok;
		}
		if (FNODE_NAME(fn)[0] == '.' && (fn->namelen == 1 || ((FNODE_NAME(fn)[1] == '.' && fn->namelen == 2)))) {
			dot = 1;
			i = 0;
			goto invl_name;
		}
		hash = name_hash(filename);
		if (__unlikely((hash & d->hash_mask) != (d->hash_val & d->hash_mask))) {
			log_printf(0, "%s: FILENAME \"%s\" BELONGS TO DIFFERENT HASH PAGE", get_filename(d), escape_name(filename));
			different_hash_error = 1;
		}
		if (!(file_hash[(hash & (FILE_HASH_SIZE - 1)) / 32] & (1 << (hash & 31)))) {
			file_hash[(hash & (FILE_HASH_SIZE - 1)) / 32] |= 1 << (hash & 31);
			goto name_ok;
		}
		LIST_FOR_EACH(subdir, &d->subdirs, SUBDIR, list) {
			if (__unlikely(!strcmp(subdir->name, filename))) {
				duplicate:
				log_printf(0, "%s: DUPLICATE FILENAME \"%s\"", get_filename(d), escape_name(filename));
				duplicate_filename_error = 1;
				goto name_ok;
			}
		}
		XLIST_FOR_EACH(subdir, filenames, SUBDIR, list) {
			if (__unlikely(!strcmp(subdir->name, filename))) {
				goto duplicate;
			}
		}
		name_ok:
		subdir = mem_alloc(sizeof(SUBDIR) + strlen(filename));
		if (__unlikely(!subdir)) {
			log_printf(1, "CAN'T ALLOC MEMORY FOR SUBDIRECTORY DESCRIPTION FOR FILE %s/%s", get_filename(d), escape_name(filename));
			return 2;
		}
		INIT_LIST(&subdir->subdirs);
		subdir->this_fnode = blk;
		subdir->this_offset = fp;
		subdir->dir_size = 0;
		subdir->dir_depth = d->dir_depth + 1;
		subdir->dnode_depth = 0;
		strcpy(subdir->name, filename);
		subdir->parent = d;
		name_ea_size = 0;
		eapos = FNODE_EA_POS(fn->namelen);
		ea_size = size - eapos;
		memcpy(ea.ea, (char *)fn + eapos, ea_size);
		if (__unlikely((ea_msg = check_ea(fn, (struct fnode_ea *)ea.ea, ea_size, __likely(!(fn->flags & FNODE_FLAGS_HARDLINK)) ? EA_TYPE_FILE_NAME : EA_TYPE_NAME, &prealloc)) != NULL)) {
			log_printf(0, "%s: FNODE BLOCK %"blk_format"X: INVALID EXTENDED ATTRIBUTES: %s", get_filename(subdir), blk, ea_msg);
			ea_error = 1;
		}
		if (__unlikely(fn->flags & FNODE_FLAGS_DIR)) {
			int r;
			blk_t subblk;
			if (cc_valid(fn->cc, fn->txc)) subblk = MAKE_D_OFF(fn->run10, fn->run11);
			else subblk = MAKE_D_OFF(fn->run20, fn->run21);
			subdir->sub_fnode = subblk;
			subdir->file = 0;
			subdir->hash_val = 0;
			subdir->hash_mask = 0;
			r = get_fnode_description(subblk, &subdir->blocks, get_filename, subdir);
			if (__unlikely(r == 2)) return 2;
			reread_block;
			if (__unlikely(r)) {
				const char *filen;
				filen = get_filename(subdir);
				mem_free(subdir);
				if (r == -1) goto cont;
				log_printf(0, "%s: INVALID DIRECTORY AT %"blk_format"X (FROM %"blk_format"X/%X)", filen, subblk, blk, fp);
				goto query_delete;
			}
			if (__unlikely(recover_all)) goto recover_directory;
			if (__unlikely(different_hash_error | duplicate_filename_error | ea_error)) {
#ifdef TESTCODE
				test_recover_directory:
#endif
				if (duplicate_filename_error) {
					if (!query(qprefix 0, "RENAME?")) goto dir_nofix;
				} else {
					if (!query(qprefix 0, "FIX?")) goto dir_nofix;
				}
				recover_directory:
				r = dealloc_and_free_fnode_description(&subdir->blocks);
				mem_free(subdir);
				if (__unlikely(r)) return r;
				memset(&rfd, 0, sizeof rfd);
				rfd.size = SPAD2CPU64(fn->size[!cc_valid(fn->cc, fn->txc)]);
				if (rfd.size >= prealloc) rfd.size -= prealloc;
				rfd.ctime = SPAD2CPU32_LV(&fn->ctime);
				rfd.mtime = SPAD2CPU32_LV(&fn->mtime);
				rfd.flags = fn->flags;
				rfd.dnode = subblk;
				rfd.run10 = SPAD2CPU32(fn->run10);
				rfd.run11 = SPAD2CPU16(fn->run11);
				rfd.run1n = SPAD2CPU16(fn->run1n);
				rfd.run20 = SPAD2CPU32(fn->run20);
				rfd.run21 = SPAD2CPU16(fn->run21);
				rfd.run2n = SPAD2CPU16(fn->run2n);
				rfd.anode0 = SPAD2CPU32(fn->anode0);
				rfd.anode1 = SPAD2CPU16(fn->anode1);
				if (__unlikely(r = recover_file(&rfd, ea.ea, ea_size, get_filename(d), filename, NULL, 0, NULL))) return r;
				goto skip_this_q;
			}
#ifdef TESTCODE
			if (recover_all_directories) {
				log_printf(0, "%s: TESTING RECOVERY OF DIRECTORY \"%s\"", get_filename(d), escape_name(filename));
				goto test_recover_directory;
			}
#endif
			dir_nofix:
			ADD_TO_LIST(&d->subdirs, &subdir->list);
			ADD_TO_LIST(&todolist, &subdir->todolist);
		} else if (__unlikely(fn->flags & FNODE_FLAGS_HARDLINK)) {
			int r;
			subdir->file = 1;
			INIT_LIST(&subdir->blocks);
			link = MAKE_D_OFF(fn->anode0, fn->anode1);
			memcpy(name_ea.name_ea, ea.ea, ea_size);
			name_ea_size = ea_size;
			ea_size = 0;
			if (__unlikely(!link)) {
				log_printf(0, "%s: INVALID HARDLINK POINTER", get_filename(subdir));
				mem_free(subdir);
				goto query_delete;
			}
			r = hardlink_link(link);
			if (r == 1) {
				ADD_TO_XLIST(filenames, &subdir->list);
				if (__unlikely(different_hash_error | duplicate_filename_error/* | ea_error | recover_all*/)) {
	/* What if we should recover this one? */
	/* I couldn't think of a method how to handle this without major
	   code bloat, so I just kill the reference. File won't be lost,
	   just a hardlink on it will be */
					if (query(qprefix 0, "DELETE REFERENCE?")) {
						hardlink_unlink(link);
						link = 0;
						goto skip_this;
					}
				}
				if (ea_error && query(qprefix 0, "FIX?")) {
	/* EAs can be fixed in place */
					unsigned new_name_ea_size = name_ea_size;
					fix_ea(fn, (struct fnode_ea *)name_ea.name_ea, &new_name_ea_size, EA_TYPE_NAME);
					memcpy((char *)fn + eapos, name_ea.name_ea, new_name_ea_size);
					if (new_name_ea_size != name_ea_size) {
						struct fnode *xfn;
						CPU2SPAD16_LV(&fn->next, (SPAD2CPU16_LV(&fn->next) & ~FNODE_NEXT_SIZE) | (eapos + new_name_ea_size));
						xfn = (struct fnode *)((char *)fn + eapos + new_name_ea_size);
						CPU2SPAD16_LV(&xfn->next, FNODE_NEXT_FREE | (name_ea_size - new_name_ea_size));
						CPU2SPAD16_LV(&xfn->cc, 0);
						CPU2SPAD32_LV(&xfn->txc, 0);
					}
					update_block;
				}
				goto cont;
			}
			if (__unlikely(r == 2)) {
				mem_free(subdir);
				return 2;
			}
			if (__unlikely(r == -1)) {
				RECOVERY *rec = hardlink_get_recovery(link);
				r = add_recovery_hardlink(rec, get_filename(d), filename, name_ea.name_ea, name_ea_size);
				mem_free(subdir);
				if (__unlikely(r)) return r;
				link = 0;
				goto skip_this_q;
			}
			r = read_fixed_fnode(link, 0, &fx, &fx_fn, get_filename, subdir);
			if (__unlikely(r == 2)) {
				mem_free(subdir);
				return 2;
			}
			if (__unlikely(r == 1)) {
				hardlink_delete(link);
				link = 0;
				mem_free(subdir);
				goto query_delete;
			}
			if (__unlikely(r == -1)) {
				mem_free(subdir);
				goto cont;
			}
			link_free_space = 1;
			hardlink_set_disk_nlink(link, SPAD2CPU64_LV(FIXED_FNODE_NLINK_PTR(fx_fn)));
			memcpy(&f1, fx_fn, sizeof(struct fnode));
			ea_size = (SPAD2CPU16_LV(&fx_fn->next) & FNODE_NEXT_SIZE) - FNODE_EA_POS(0);
			memcpy(ea.ea, (char *)fx_fn + FNODE_EA_POS(0), ea_size);
			if (__unlikely((ea_msg = check_ea(fx_fn, (struct fnode_ea *)ea.ea, ea_size, EA_TYPE_FILE, &prealloc)) != NULL)) {
				log_printf(0, "%s: FIXED FNODE BLOCK %"blk_format"X: INVALID EXTENDED ATTRIBUTES: %s", get_filename(subdir), link, ea_msg);
				ea_error = 1;
			}
			goto do_check_file;
		} else {
			int r;
			link = 0;
			subdir->file = 1;
			INIT_LIST(&subdir->blocks);
			memcpy(&f1, fn, sizeof(struct fnode));
			do_check_file:
			r = check_file(&f1, &rfd.runs, get_filename, subdir, prealloc);
			ADD_TO_XLIST(filenames, &subdir->list);
			if (__unlikely(r == 2)) goto free_runs_ret_r;
			reread_block;
			if (__unlikely(r == -2)) {
				free_file_runs(&rfd.runs);
				goto query_delete;
			}
			if (__unlikely(r == 1) || __unlikely(different_hash_error | duplicate_filename_error | ea_error | recover_all)) {
				if (!r) if ((r = dealloc_blocklist(0, &rfd.runs, 2))) {
					free_runs_ret_r:
					free_file_runs(&rfd.runs);
					return r;
				}
				if (recover_all || (!r && duplicate_filename_error ?
/* do not reformat: code line of qprefix is important for tracking
   "SAME" option behaviour */
				    query(qprefix 0, "RENAME?") :
				    query(qprefix 0, "FIX?"))) {
					RECOVERY *rec;
					rfd.size = SPAD2CPU64(f1.size[!cc_valid(f1.cc, f1.txc)]);
					if (rfd.size >= prealloc) rfd.size -= prealloc;
					rfd.ctime = SPAD2CPU32_LV(&f1.ctime);
					rfd.mtime = SPAD2CPU32_LV(&f1.mtime);
					rfd.flags = f1.flags;
					rfd.run10 = SPAD2CPU32(f1.run10);
					rfd.run11 = SPAD2CPU16(f1.run11);
					rfd.run1n = SPAD2CPU16(f1.run1n);
					rfd.run20 = SPAD2CPU32(f1.run20);
					rfd.run21 = SPAD2CPU16(f1.run21);
					rfd.run2n = SPAD2CPU16(f1.run2n);
					rfd.anode0 = SPAD2CPU32(f1.anode0);
					rfd.anode1 = SPAD2CPU16(f1.anode1);
					if (__unlikely(r = recover_file(&rfd, ea.ea, ea_size, get_filename(d), filename, name_ea.name_ea, name_ea_size, &rec))) goto free_runs_ret_r;
					if (link) hardlink_set_recovery(link, rec);
					goto skip_this_q;
				}
				r = force_block_alloc_file_runs(&rfd.runs);
				free_file_runs(&rfd.runs);
				if (__unlikely(r)) return r;
				goto cont;
			}
			free_file_runs(&rfd.runs);
			if (__unlikely(r == -1)) goto cont;
			if (__likely(!link)) {
				if (__unlikely(memcmp(fn, &f1, sizeof(struct fnode)))) {
					memcpy(fn, &f1, sizeof(struct fnode));
					update_block;
				}
			} else {
				fx = reread_fixed_fnode_block(link);
				if (__unlikely(!fx)) {
					log_printf(1, "%s: ERROR RE-READING FIXED FNODE BLOCK %"blk_format"X: %s", get_filename(d), link, buffer_error);
					log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN CORRECT BEFORE");
					return 1;
				}
				fx_fn = fixed_fnode_block_fnode(fx, 0);
				if (__unlikely(memcmp(fx_fn, &f1, sizeof(struct fnode)))) {
					memcpy(fx_fn, &f1, sizeof(struct fnode));
					if (__unlikely(!update_fixed_fnode_block(fx, link))) {
						log_printf(1, "%s: ERROR UPDATING FIXED FNODE BLOCK %"blk_format"X: %s", get_filename(d), link, buffer_error);
						log_printf(1, "THE DEVICE IS UNRELIABLE, BLOCK HAS BEEN CORRECT BEFORE");
						return 1;
					}
				}
				reread_block;
			}
		}

		if (0) {
			skip_this_q:
			{
	/* !!! TODO: delay this step until recovery time */
				static int skip_query = 0;
				if (!ro && !skip_query) {
					log_printf(0, "%s: %s \"%s\" WILL BE DELETED. IT WILL BE RECOVERED AT THE END. IF YOU BREAK FSCK, IT WILL BE LOST", get_filename(d), fn->flags & FNODE_FLAGS_DIR ? "DIRECTORY" : "FILE", escape_name(filename));
					if (!query(qprefix 0, "CONTINUE?")) return 1;
					skip_query = 1;
				}
			}
			goto skip_this;
		}
		if (0) {
			query_delete:
			if (query(qprefix 0, "DELETE?")) {
				skip_this:
				if (link) {
					hardlink_delete(link);
					if (link_free_space) {
						/*sys_free(link, 1 << super.sectors_per_block_bits, 1);*/
						free_blocks(link, 1 << super.sectors_per_block_bits);
						link_free_space = 0;
					}
					link = 0;
				}
				reread_block;
				CPU2SPAD16_LV(&fn->next, SPAD2CPU16_LV(&fn->next) | FNODE_NEXT_FREE);
				CPU2SPAD16_LV(&fn->cc, 0);
				CPU2SPAD32_LV(&fn->txc, 0);
				update_block;
			} else {
				reread_block;
			}
		}

		cont:
		print_alloc_progress();
		fp += size;
	} while (fp < FNODE_BLOCK_SIZE);
#undef fn
#undef update_block
	return 0;
}

void prefetch_fnode_block(struct fnode_block *f)
{
	int fp;
	if (__unlikely(!prefetch_supported)) return;
	fp = SIZEOF_FNODE_BLOCK;
#define fn ((struct fnode *)((char *)f + fp))
	do {
		unsigned size = SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_SIZE;
		if (__unlikely(fp + size > FNODE_BLOCK_SIZE)) {
			/* This will be fixed in check_fnode_block */
			return;
		}
		if (cc_valid(fn->cc, fn->txc) && SPAD2CPU16_LV(&fn->next) & FNODE_NEXT_FREE) {
			if (__unlikely(size < FNODE_HEAD_SIZE)) {
				/* This will be fixed in check_fnode_block */
				return;
			}
			goto next;
		}
		if (__unlikely(size <= FNODE_NAME_POS)) {
			/* This will be fixed in check_fnode_block */
			return;
		}
		if (__unlikely(fn->flags & FNODE_FLAGS_DIR)) {
			blk_t subblk;
			if (cc_valid(fn->cc, fn->txc)) subblk = MAKE_D_OFF(fn->run10, fn->run11);
			else subblk = MAKE_D_OFF(fn->run20, fn->run21);
			/*fprintf(stderr, "prefetch subdir: %Lx\n", subblk);*/
			prefetch_buffer(subblk, 0);
		} else {
			/* Works for both hardlinks and anodes */
			blk_t link = MAKE_D_OFF(fn->anode0, fn->anode1);
			if (__unlikely(link != 0)) prefetch_buffer(link, 0);
			/*fprintf(stderr, "prefetch subblk: %Lx\n", link);*/
		}
		next:
		fp += size;
	} while (fp < FNODE_BLOCK_SIZE);
#undef fn
}

unsigned rev_bits(unsigned x)
{
	int i = dnode_hash_bits;
	unsigned y = 0;
	do {
		y <<= 1;
		y |= x & 1;
		x >>= 1;
	} while (--i);
	return y;
}

blk_t dnode_decode(struct dnode_page_entry *e)
{
	return MAKE_D_OFF3(e->b0, e->b1, e->b2);
}

#define dnode_entry(x)	((struct dnode_page_entry *)((char *)dn + data) + (x))
#define dnode_val(x)	dnode_decode(dnode_entry(x))
#define dnode_set(x, v)	dnode_entry(x)->b0 = MAKE_PART_30(v), dnode_entry(x)->b1 = MAKE_PART_31(v), dnode_entry(x)->b2 = MAKE_PART_32(v)

static int check_dnode(SUBDIR *d, blk_t blk)
{
	blk_t up_dnode;
	struct dnode_page *dn = read_buffer(blk, dnode_page_sectors, 0);
	unsigned data, o;
	unsigned i;
	if (__unlikely(!dn)) {
		read_err:
		log_printf(1, "%s: ERROR ACCESSING DNODE %"blk_format"X: %s", get_filename(d), blk, buffer_error);
		log_printf(1, "THE DEVICE IS UNRELIABLE, DNODE HAS BEEN SUCCESSFULY READ BEFORE");
		return 1;
	}
#define update_head							\
do {									\
	if (__unlikely(write_buffer(blk, 1, dn))) goto read_err;	\
	if (__unlikely(!(dn = read_buffer(blk, dnode_page_sectors, 0)))) goto read_err;\
} while (0)
	if (__unlikely(SPAD2CPU32_LV(&dn->magic) != magic(blk, DNODE_PAGE_MAGIC))) {
		log_printf(1, "%s: BAD MAGIC WHEN RE-READING DNODE %"blk_format"X: %08X", get_filename(d), blk, (unsigned)SPAD2CPU32_LV(&dn->magic));
		log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
		return 1;
	}
	if (__unlikely((dn->gflags & DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1) >> DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT != super.sectors_per_page_bits - 1)) {
		log_printf(0, "%s: DNODE %"blk_format"X HAS INVALID PAGE SIZE FLAG (%d != %d)", get_filename(d), blk, (dn->gflags & DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1) >> DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT, super.sectors_per_page_bits - 1);
		if (query(qprefix 0, "FIX?")) {
			dn->gflags = (dn->gflags & ~DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1) | ((super.sectors_per_page_bits - 1) << DNODE_GFLAGS_PAGE_SIZE_BITS_MINUS_1_SHIFT);
			update_head;
		}
	}
	up_dnode = !d->dnode_depth ? 0 : d->parent->sub_fnode;
	if (__unlikely(SPAD2CPU64_LV(&dn->up_dnode) != up_dnode)) {
		log_printf(0, "%s: DNODE %"blk_format"X HAS BAD UP-DNODE POINTER (%"blk_format"X != %"blk_format"X)", get_filename(d), blk, (blk_t)SPAD2CPU64_LV(&dn->up_dnode), up_dnode);
		if (query(qprefix 0, "FIX?")) {
			CPU2SPAD64_LV(&dn->up_dnode, up_dnode);
			update_head;
		}
	}
	if (__unlikely(cc_check(&dn->cc, &dn->txc, "DNODE", get_filename, d, blk))) update_head;
	if (cc_valid(dn->cc, dn->txc)) data = DNODE_ENTRY_OFFSET, o = 0;
	else data = DNODE_ENTRY_OFFSET + dnode_data_size, o = 1;
#define reread_dnode							\
do {									\
	if (__unlikely(!(dn = read_buffer(blk, dnode_page_sectors, 0)))) goto read_err;\
} while (0)
#define update_dnode							\
do {									\
	dn->flags[o] |= DNODE_CHECKSUM_VALID;				\
	dn->checksum[o] = __byte_sum((char *)dn + data, dnode_data_size) ^ CHECKSUM_BASE;\
	if (__unlikely(write_buffer(blk, dnode_page_sectors, dn))) goto read_err;\
	reread_dnode;							\
} while (0)
	if (dn->flags[o] & DNODE_CHECKSUM_VALID) {
		if (__unlikely(__byte_sum((char *)dn + data, dnode_data_size) != (CHECKSUM_BASE ^ dn->checksum[o]))) {
			log_printf(0, "%s: BAD CHECKSUM ON DNODE %"blk_format"X: %02X", get_filename(d), blk, __byte_sum((char *)dn + data, dnode_data_size) ^ CHECKSUM_BASE ^ dn->checksum[o]);
			if (query(qprefix 0, "FIX?")) {
				dn->checksum[o] = __byte_sum((char *)dn + data, dnode_data_size) ^ CHECKSUM_BASE;
				update_head;
			}
		}
	}
	for (i = 0; i < 1 << dnode_hash_bits; ) {
		SUBDIR *subdir;
		int r;
		unsigned b, j;
		int hash_bits;
		blk_t ptr;
		print_alloc_progress();
		ptr = dnode_val(rev_bits(i));
		if (!ptr) {
			i++;
			continue;
		}
		subdir = mem_alloc(sizeof(SUBDIR));
		if (__unlikely(!subdir)) {
			log_printf(1, "CAN'T ALLOC MEMORY FOR SUBDIRECTORY DESCRIPTION FOR DIRECTORY %s", get_filename(d));
			return 2;
		}
		INIT_LIST(&subdir->subdirs);
		subdir->this_fnode = 0;
		subdir->this_offset = 0;
		subdir->dir_size = 0;
		subdir->sub_fnode = ptr;
		subdir->file = 0;
		subdir->dir_depth = d->dir_depth;
		subdir->dnode_depth = d->dnode_depth + 1;
		subdir->name[0] = 0;
		subdir->parent = d;
		r = get_fnode_description(ptr, &subdir->blocks, get_filename, d);
		if (__unlikely(r)) {
			mem_free(subdir);
			if (r == 2) return 2;
			reread_dnode;
			if (r == -1) goto dn_skip_this;
			log_printf(0, "%s: DNODE %"blk_format"X HAS INVALID POINTER TO %"blk_format"X AT %X", get_filename(d), blk, ptr, i);
			if (query(qprefix 0, "CLEAR?")) {
				for (j = 0; (i + j < 1 << dnode_hash_bits && dnode_val(rev_bits(i + j)) == ptr) || !j; j++) dnode_set(rev_bits(i + j), 0);
				update_dnode;
				goto cont;
			}
			dn_skip_this:
			for (j = 1; i + j < 1 << dnode_hash_bits && dnode_val(rev_bits(i + j)) == ptr; j++) ;
			goto cont;
		}
		reread_dnode;
		b = 1 << __BSF(i | (1 << dnode_hash_bits));
		for (j = 1; j < b; j++) {
			blk_t newptr = dnode_val(rev_bits(i + j));
			if (newptr != ptr) {
				LIST_HEAD l;
				if (!newptr) break;
				r = get_fnode_description(newptr, &l, get_filename, d);
				if (__likely(!r)) {
					if (__unlikely(r = dealloc_and_free_fnode_description(&l))) {
						mem_free(subdir);
						return r;
					}
					reread_dnode;
					break;
				}
				if (__unlikely(r == 2)) {
					mem_free(subdir);
					return 2;
				}
				reread_dnode;
				log_printf(0, "%s: DNODE %"blk_format"X HAS INVALID POINTER TO %"blk_format"X AT %X BREAKING RUN %"blk_format"X", get_filename(d), blk, newptr, i + j, ptr);
				if (query(qprefix 0, "FIX?")) {
					dnode_set(rev_bits(i + j), ptr);
					j++;
					while (j < b && dnode_val(rev_bits(i + j)) == newptr) dnode_set(rev_bits(i + j), ptr), j++;
					j--;
					update_dnode;
				}
			}
		}
		if (j & (j - 1) || (i + j < 1 << dnode_hash_bits && dnode_val(rev_bits(i + j)) == ptr)) {
			log_printf(0, "%s: DNODE %"blk_format"X HAS UNALIGNED RUN AT %X", get_filename(d), blk, i);
			if (query(qprefix 0, "FIX?")) {
				if (j & (j - 1)) {
					unsigned k;
					for (k = j; k & (k - 1); k++) if (dnode_val(rev_bits(i + k))) goto go_back;
					for (k = j; k & (k - 1); k++) dnode_set(rev_bits(i + k), ptr);
					update_dnode;
					j = k;
					goto fixed;
					go_back:
					for (k = j; k & (k - 1); k--) dnode_set(rev_bits(i + k - 1), 0);
					update_dnode;
					j = k;
					goto fixed;
				} else {
					unsigned k;
					for (k = j; i + k < 1 << dnode_hash_bits && dnode_val(rev_bits(i + k)) == ptr; k++) dnode_set(rev_bits(i + k), 0);
					update_dnode;
					goto fixed;
				}
			}
		}
		fixed:
		hash_bits = dnode_hash_bits - __BSR(j);
		if (__unlikely(d->dnode_depth * dnode_hash_bits + hash_bits > 32)) {
			unsigned k;
			log_printf(0, "%s: DNODE %"blk_format"X HAS TOO DEEP POINTERS TO %"blk_format"X (%u BITS)", get_filename(d), blk, ptr, d->dnode_depth * dnode_hash_bits + hash_bits);
			invl_hash:
			if (query(qprefix 0, "CLEAR?")) {
				for (k = 0; k < j; k++) dnode_set(rev_bits(i + k), 0);
				update_dnode;
			}
			r = dealloc_and_free_fnode_description(&subdir->blocks);
			mem_free(subdir);
			if (__unlikely(r)) return r;
			goto cont;
		}
		if (__unlikely(LIST_STRUCT(subdir->blocks.next, FNODE_DESC, list)->dno) && __unlikely(j != 1)) {
			log_printf(0, "%s: DNODE %"blk_format"X HAS MULTIPLE POINTERS TO DNODE %"blk_format"X", get_filename(d), blk, ptr);
			goto invl_hash;
		}
		if (__likely(d->dnode_depth * dnode_hash_bits + hash_bits != 32))
			subdir->hash_mask = ((__u32)1 << (d->dnode_depth * dnode_hash_bits + hash_bits)) - 1;
		else
			subdir->hash_mask = ~0;
		subdir->hash_val = d->hash_val | rev_bits(i) << (d->dnode_depth * dnode_hash_bits);
		ADD_TO_LIST(&d->subdirs, &subdir->list);
		ADD_TO_LIST(&todolist, &subdir->todolist);
		cont:
		i += j;
	}
	return 0;
#undef update_head
}

void prefetch_dnode(blk_t dnode)
{
	blk_t prev_ptr = 0;
	unsigned data, i;
	struct dnode_page *dn;
	if (__unlikely(!prefetch_supported)) return;
	dn = read_buffer(dnode, dnode_page_sectors, 0);
	if (__unlikely(!dn)) return;
	if (cc_valid(dn->cc, dn->txc)) data = DNODE_ENTRY_OFFSET;
	else data = DNODE_ENTRY_OFFSET + dnode_data_size;
	for (i = 0; i < 1 << dnode_hash_bits; i++) {
		blk_t ptr = dnode_val(rev_bits(i));
		if (!ptr) continue;
		if (ptr == prev_ptr) continue;
		prev_ptr = ptr;
		prefetch_buffer(ptr, 0);
	}
}

#undef dnode_entry
#undef dnode_val
#undef dnode_set

/*
 * Reseal a directory's fnode block after changing its size. The root lives
 * in a fixed_fnode_block, which carries the CRC trailer; every other
 * directory lives in a chained fnode_block, which keeps the XOR fold.
 * Dispatch on the magic, as the kernel reader does (bufstruc.c), or fsck
 * writes an XOR fold onto the root and the next mount fails with EUCLEAN.
 */
static void seal_dir_fnode_block(struct fnode_block *f, blk_t blk)
{
	if (SPAD2CPU32_LV(&f->magic) == magic(blk, FIXED_FNODE_BLOCK_MAGIC)) {
		/* flags and the legacy field first: the trailer covers them */
		f->checksum = 0;
		f->flags |= FIXED_FNODE_BLOCK_CHECKSUM_VALID | FIXED_FNODE_BLOCK_TYPE;
		spadfs_meta_seal(f);
	} else {
		f->flags |= FNODE_BLOCK_CHECKSUM_VALID;
		f->checksum ^= CHECKSUM_BASE ^ __byte_sum(f, FNODE_BLOCK_SIZE);
	}
}

static int check_dir_size(SUBDIR *d)
{
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	__u64 size;

	if (d->dnode_depth) return 0;

	fnode_block = read_buffer(d->this_fnode, 1, 0);
	if (__unlikely(!fnode_block)) {
		read_err:
		log_printf(1, "%s: ERROR ACCESSING FNODE BLOCK %"blk_format"X: %s", get_filename(d), d->this_fnode, buffer_error);
		log_printf(1, "THE DEVICE IS UNRELIABLE, FNODE HAS BEEN SUCCESSFULY READ BEFORE");
		return 1;
	}
	if (__unlikely(SPAD2CPU32_LV(&fnode_block->magic) != magic(d->this_fnode, FNODE_BLOCK_MAGIC)) &&
	    __unlikely(SPAD2CPU32_LV(&fnode_block->magic) != magic(d->this_fnode, FIXED_FNODE_BLOCK_MAGIC))) {
		log_printf(1, "%s: BAD MAGIC WHEN RE-READING FNODE BLOCK %"blk_format"X: %08X", get_filename(d), d->this_fnode, (unsigned)SPAD2CPU32_LV(&fnode_block->magic));
		log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
		return 1;
	}

	fnode = (struct fnode *)((char *)fnode_block + d->this_offset);
	size = SPAD2CPU64(fnode->size[!cc_valid(fnode->cc, fnode->txc)]);

	if (__likely(SPAD2CPU32_LV(&super.flags_compat_ro) & FLAG_COMPAT_RO_DIRECTORY_SIZES) && __unlikely(size != d->dir_size)) {
		if (size == (__u64)-1) goto auto_dir_size;
		log_printf(0, "%s: DIRECTORY SIZE MISCOUNTED, REAL %"__64_format"d, STORED %"__64_format"d", get_filename(d), d->dir_size, size);
		if (query(qprefix 0, "FIX?")) {
			auto_dir_size:
			CPU2SPAD64_LV(&fnode->size[!cc_valid(fnode->cc, fnode->txc)], d->dir_size);
			seal_dir_fnode_block(fnode_block, d->this_fnode);
			if (__unlikely(write_buffer(d->this_fnode, 1, fnode_block))) goto read_err;
		}
	}

	return 0;
}

int adjust_dir_size(blk_t dir_blk, unsigned dir_offset, int delta)
{
	struct fnode_block *fnode_block;
	struct fnode *fnode;
	__u64 size;

	if (dir_blk == root_fnode) root.dir_size += delta;

	fnode_block = read_buffer(dir_blk, 1, 0);
	if (__unlikely(!fnode_block)) {
		log_printf(0, "ERROR READING FNODE BLOCK %"blk_format"X: %s", dir_blk, buffer_error);
		return 1;
	}

	if (__unlikely(SPAD2CPU32_LV(&fnode_block->magic) != magic(dir_blk, FNODE_BLOCK_MAGIC)) &&
	    __unlikely(SPAD2CPU32_LV(&fnode_block->magic) != magic(dir_blk, FIXED_FNODE_BLOCK_MAGIC))) {
		log_printf(1, "BAD MAGIC WHEN RE-READING FNODE BLOCK %"blk_format"X: %08X", dir_blk, (unsigned)SPAD2CPU32_LV(&fnode_block->magic));
		log_printf(1, "THE DEVICE IS UNRELIABLE, MAGIC HAS BEEN CORRECT BEFORE");
		return 1;
	}

	if (__unlikely(!(SPAD2CPU32_LV(&super.flags_compat_ro) & FLAG_COMPAT_RO_DIRECTORY_SIZES))) return 0;

	fnode = (struct fnode *)((char *)fnode_block + dir_offset);
	size = SPAD2CPU64(fnode->size[!cc_valid(fnode->cc, fnode->txc)]);
	size += delta;
	CPU2SPAD64_LV(&fnode->size[!cc_valid(fnode->cc, fnode->txc)], size);

	seal_dir_fnode_block(fnode_block, dir_blk);

	return write_buffer(dir_blk, 1, fnode_block);
}

int claim_recovered_directory(char *path, char *filename, blk_t dnode, void **result)
{
	int r;
	SUBDIR *subdir = mem_alloc(sizeof(SUBDIR) + 1 + strlen(path) + strlen(filename));
	if (__unlikely(!subdir)) {
		log_printf(1, "CAN'T ALLOC MEMORY FOR SUBDIRECTORY DESCRIPTION WHEN RECOVERING DIRECTORY %s/%s", escape_name(path), escape_name(filename));
		return 2;
	}
	INIT_LIST(&subdir->subdirs);
	subdir->this_fnode = 0;
	subdir->this_offset = 0;
	subdir->dir_size = 0;
	subdir->dir_depth = 1;
	subdir->dnode_depth = 0;
	subdir->file = 0;
	subdir->hash_val = 0;
	subdir->hash_mask = 0;
	strcpy(subdir->name, path);
	strcat(subdir->name, "/");
	strcat(subdir->name, filename);
	subdir->sub_fnode = dnode;
	subdir->parent = &root;
	r = get_fnode_description(dnode, &subdir->blocks, get_filename, subdir);
	if (__unlikely(r)) {
		if (r != 2)
			log_printf(0, "%s: INVALID RECOVERED DIRECTORY AT %"blk_format"X (IT WAS PROBABLY ALLOCATED OR OVERWRITTEN BY SOMETHING ELSE)", subdir->name, dnode);
		mem_free(subdir);
		return r;
	}
	ADD_TO_LIST_END(&root.subdirs, &subdir->list);
	ADD_TO_LIST_END(&todolist, &subdir->todolist);
	*result = subdir;
	return 0;
}

void set_recovered_directory_position(void *result, blk_t blk, unsigned pos)
{
	SUBDIR *subdir = result;
	subdir->this_fnode = blk;
	subdir->this_offset = pos;
}

int undo_recovered_directory(void *result)
{
	int r;
	SUBDIR *subdir = result;
	DEL_FROM_LIST(&subdir->list);
	DEL_FROM_LIST(&subdir->todolist);
	r = dealloc_and_free_fnode_description(&subdir->blocks);
	mem_free(subdir);
	if (__unlikely(r)) return r;
	return 0;
}

static void free_node(SUBDIR *d)
{
	free_fnode_description(&d->blocks);
	if (__likely(d != &root)) {
		DEL_FROM_LIST(&d->list);
		mem_free(d);
	}
}

static void free_tree(void)
{
	SUBDIR *d = &root, *p;
	do {
		while (!LIST_EMPTY(&d->subdirs)) d = LIST_STRUCT(d->subdirs.next, SUBDIR, list);
		p = d->parent;
		free_node(d);
	} while ((d = p));
}

static char *filename = NULL;
static size_t filenamelen = 0;

static const char *get_filename(void *d_)
{
	SUBDIR *d = d_;
	int p, q;
	if (__unlikely(d == &root)) return "/";
	p = 0;
	do {
		const char *n;
		ssize_t l;
		size_t want;
		if (__unlikely(d->dnode_depth)) continue;
		n = escape_name(d->name);
		want = p + strlen(n) + 2;
		if (__unlikely(want > filenamelen)) {
			char *newfn = mem_alloc(want);
			if (__unlikely(!newfn)) {
				log_printf(1, "CAN'T ALLOC MEMORY FOR FILENAME");
				return NULL;
			}
			memcpy(newfn, filename, p);
			mem_free(filename);
			filename = newfn;
			filenamelen = want;
		}
		for (l = strlen(n) - 1; l >= 0; l--) {
			filename[p++] = n[l];
		}
		filename[p++] = '/';
	} while ((d = d->parent) != &root);
	if (!p) return "/";
	while (p >= 2 && filename[p - 1] == '/' && filename[p - 2] == '/') p--;
	filename[p] = 0;
	for (q = p >> 1, p = p - q, q--; q >= 0; q--) {
		char c = filename[q];
		filename[q] = filename[p];
		filename[p++] = c;
	}
	return filename;
}

void done_dir(void)
{
	if (__unlikely(filename != NULL)) mem_free(filename);
}
