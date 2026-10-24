#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

int ro = 0;
int y = 0;
int preen = 0;
int verbose = 0;
int force = 0;
int mark_fsck = 0;
int extend = 0;
int rebuild_apages = 0;
int reset_cc = 0;
int zero_file_tails = 0;
int undelete = 0;
int undelete_scan_all = 0;
int dont_store_cross_links = 0;
const char *swapfile = NULL;
unsigned long memory_limit = 0;
const char *logfile = NULL;
int sync_writes = 0;
__s64 reserve = -1;
#if defined(__SPAD__) || defined(DEFAULT_DIRECT)
int nodirect = 0;
#else
int nodirect = 1;
#endif
int nocache = 0;
int noprefetch = 0;
int noaio = 0;
#ifdef TESTCODE
int recover_all_files = 0;
int recover_all_directories = 0;
int fragment_recovered_files = 0;
int move_recovered_files = 0;
int dont_sort_recovered_files = 0;
int make_apage_bitmaps = 0;
int always_swap = 0;
int debug_malloc = 0;
#endif
const char *dev = NULL;
int device_handle = -1;
#ifdef UNRELIABLE_O_DIRECT
int device_handle_unreliable = -1;
#endif
struct stat st;
char st_valid = 0;

char modified = 0;
char reboot_needed = 0;
char answered_yes = 0;
char answered_no = 0;
char was_out_of_memory = 0;
char was_read_error = 0;
char was_write_error = 0;
char was_stdin_error = 0;

#ifdef __SPAD__
#ifndef TESTCODE
#define ARRAY_SIZE	17
#else
#define ARRAY_SIZE	28
#endif
static const struct __param_table params[ARRAY_SIZE] = {
	"", __PARAM_STRING, 1, __MAX_STR_LEN,
	"RO", __PARAM_BOOL, ~0, 1,
	"Y", __PARAM_BOOL, ~0, 1,
	"F", __PARAM_BOOL, ~0, 1,
	"MARK", __PARAM_BOOL, ~0, 1,
	"V", __PARAM_BOOL, ~0, 1,
	"EXTEND", __PARAM_BOOL, ~0, 1,
	"REBUILD_APAGES", __PARAM_BOOL, ~0, 1,
	"RESET_CRASH_COUNTS", __PARAM_BOOL, ~0, 1,
	"DONT_STORE_CROSS_LINKS", __PARAM_BOOL, ~0, 1,
	"SWAPFILE", __PARAM_STRING, 1, __MAX_STR_LEN,
	"MEMORY", __PARAM_UNSIGNED_LONG, BUFFER_SIZE, MAXULONG - BUFFER_SIZE,
	"LOG", __PARAM_STRING, 1, __MAX_STR_LEN,
	"SYNC_WRITES", __PARAM_BOOL, ~0, 1,
	"SET_RESERVE", __PARAM_INT64, 0, 1,
	"NODIRECT", __PARAM_BOOL, ~0, 1,
	"NOCACHE", __PARAM_BOOL, ~0, 1,
	"NOPREFETCH", __PARAM_BOOL, ~0, 1,
	"UNDELETE", __PARAM_BOOL, ~0, 1,
	"UNDELETE_SCAN_ALL", __PARAM_BOOL, ~0, 1,
#ifdef TESTCODE
	"RECOVER_ALL_FILES", __PARAM_BOOL, ~0, 1,
	"RECOVER_ALL_DIRECTORIES", __PARAM_BOOL, ~0, 1,
	"FRAGMENT_RECOVERED_FILES", __PARAM_BOOL, ~0, 1,
	"MOVE_RECOVERED_FILES", __PARAM_BOOL, ~0, 1,
	"DONT_SORT_RECOVERED_FILES", __PARAM_BOOL, ~0, 1,
	"MAKE_APAGE_BITMAPS", __PARAM_BOOL, ~0, 1,
	"ALWAYS_SWAP", __PARAM_BOOL, ~0, 1,
	"DEBUG_MALLOC", __PARAM_BOOL, ~0, 1,
#endif
	NULL, 0, 0, 0,
};
static void *const vars[ARRAY_SIZE] = {
	&dev,
	&ro,
	&y,
	&force,
	&mark_fsck,
	&verbose,
	&extend,
	&rebuild_apages,
	&reset_cc,
	&dont_store_cross_links,
	&swapfile,
	&memory_limit,
	&logfile,
	&sync_writes,
	&reserve,
	&nodirect,
	&nocache,
	&noprefetch,
	&undelete,
	&undelete_scan_all,
#ifdef TESTCODE
	&recover_all_files,
	&recover_all_directories,
	&fragment_recovered_files,
	&move_recovered_files,
	&dont_sort_recovered_files,
	&make_apage_bitmaps,
	&always_swap,
	&debug_malloc,
#endif
	NULL,
};
#endif

static int return_value(int r)
{
	int retval = 0;
	if (modified) retval |= 1;
	if (reboot_needed) retval |= 2;
	if (answered_yes && !modified) retval |= 4;
	if (answered_no | was_read_error | was_write_error) retval |= 4;
	if (r > 0) retval |= 4;
	if (was_read_error | was_write_error | was_out_of_memory) retval |= 8;
	/*__debug_printf("err: %d %d %d %d\n", was_read_error, was_write_error, was_out_of_memory, was_stdin_error);*/
	if (r < 0) retval |= 16;
	if (was_stdin_error) retval |= 32;
	return retval;
}

int main(int argc, const char * const argv[])
{
	int r;
	char print_done = 0;
#ifdef __SPAD__
	const char * const *arg = argv;
	int state = 0;
	if (__unlikely(__parse_params(&arg, &state, params, vars, NULL, NULL, NULL)) || __unlikely(!dev)) {
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": SYNTAX ERROR");
		r = -EBADSYN;
		goto ret_r;
	}
#else
	int noopt = 0, i;
	/*
	fprintf(stderr, "spadfsck:");
	for (i = 1; i < argc; i++) fprintf(stderr, " \"%s\"", argv[i]);
	fprintf(stderr, "\n");
	*/
	if (0) {
		badsyn:
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": SYNTAX ERROR");
		r = -EBADSYN;
		goto ret_r;
	}
	for (i = 1; i < argc; i++) {
		/* there used to be getopt, but it sux. On each system in a different way. */
		if (noopt) goto process_dev;
		if (argv[i][0] == '-' && argv[i][1] != '-' && argv[i][1]) {
			long j;
			for (j = 1; argv[i][j]; j++) {
				char c = argv[i][j];
				if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
				switch (c) {
					case 'a':
					case 'p':
						preen = 1;
						/* fall through */
					case 'y':
						y = 1;
						break;
					case 'n':
						ro = 1;
						break;
					case 'r':
						break;
					case 'f':
						force = 1;
						break;
					case 'v':
						verbose = 1;
						break;
					default:
						goto badsyn;
				}
			}
		} else if (!_strcasecmp(argv[i], "--")) noopt = 1;
		else if (!_strcasecmp(argv[i], "--mark")) mark_fsck = 1;
		else if (!_strcasecmp(argv[i], "--extend")) extend = 1;
		else if (!_strcasecmp(argv[i], "--rebuild-apages")) rebuild_apages = 1;
		else if (!_strcasecmp(argv[i], "--reset-crash-counts")) reset_cc = 1;
		else if (!_strcasecmp(argv[i], "--zero-file-tails")) zero_file_tails = 1;
		else if (!_strcasecmp(argv[i], "--dont-store-cross-links")) dont_store_cross_links = 1;
		else if (!_strcasecmp(argv[i], "--swapfile") && i + 1 < argc) swapfile = argv[++i];
		else if (!_strcasecmp(argv[i], "--memory") && i + 1 < argc) {
			char *ep;
			memory_limit = strtoul(argv[++i], &ep, 10);
			if (*ep || !argv[i][0]) goto badsyn;
			if (memory_limit < 1 << 20 && memory_limit << 20 >> 20 == memory_limit) memory_limit <<= 20;
			if (memory_limit < BUFFER_SIZE || memory_limit >= MAXULONG - BUFFER_SIZE) goto badsyn;
		}
		else if (!_strcasecmp(argv[i], "--log") && i + 1 < argc) logfile = argv[++i];
		else if (!_strcasecmp(argv[i], "--sync-writes")) sync_writes = 1;
		else if (!_strcasecmp(argv[i], "--set-reserve") && i + 1 < argc) {
			char *ep;
#if !defined(__linux__) || defined(__GLIBC__)
			reserve = strtoll(argv[++i], &ep, 10);
#else
			reserve = strtod(argv[++i], &ep);
#endif
			if (*ep || !argv[i][0]) goto badsyn;
			if (reserve < 0) goto badsyn;
		}
		else if (!_strcasecmp(argv[i], "--direct")) nodirect = 0;
		else if (!_strcasecmp(argv[i], "--nodirect")) nodirect = 1;
		else if (!_strcasecmp(argv[i], "--cache")) nocache = 0;
		else if (!_strcasecmp(argv[i], "--nocache")) nocache = 1;
		else if (!_strcasecmp(argv[i], "--prefetch")) noprefetch = 0;
		else if (!_strcasecmp(argv[i], "--noprefetch")) noprefetch = 1;
		else if (!_strcasecmp(argv[i], "--aio")) noaio = 0;
		else if (!_strcasecmp(argv[i], "--noaio")) noaio = 1;
		else if (!_strcasecmp(argv[i], "--undelete")) undelete = 1;
		else if (!_strcasecmp(argv[i], "--undelete-scan-all")) undelete_scan_all = 1;
#ifdef TESTCODE
		else if (!_strcasecmp(argv[i], "--recover-all-files")) recover_all_files = 1;
		else if (!_strcasecmp(argv[i], "--recover-all-directories")) recover_all_directories = 1;
		else if (!_strcasecmp(argv[i], "--fragment-recovered-files")) fragment_recovered_files = 1;
		else if (!_strcasecmp(argv[i], "--move-recovered-files")) move_recovered_files = 1;
		else if (!_strcasecmp(argv[i], "--dont-sort-recovered-files")) dont_sort_recovered_files = 1;
		else if (!_strcasecmp(argv[i], "--make-apage-bitmaps")) make_apage_bitmaps = 1;
		else if (!_strcasecmp(argv[i], "--always-swap")) always_swap = 1;
		else if (!_strcasecmp(argv[i], "--debug-malloc")) debug_malloc = 1;
#endif
		else if (argv[i][0] != '-') {
			process_dev:
			if (dev) goto badsyn;
			dev = argv[i];
		} else goto badsyn;
	}
	if (!dev) goto badsyn;
#endif

	if (!spadfs_struct_check_correctness()) {
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": BAD STRUCTURE ALIGNMENT, THE PROGRAM WAS COMPILED WITH WRONG ABI");
		r = -EINVAL;
		goto ret_r;
	}

	if (undelete_scan_all) undelete = 1;
	if (ro && (__unlikely(y | mark_fsck | extend | rebuild_apages | reset_cc | undelete) || __unlikely(reserve != -1))) {
#ifdef __SPAD__
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": YOU CAN'T USE BOTH /RO AND /Y, /MARK, /SET_RESERVE, /EXTEND, /REBUILD_APAGES, /RESET_CRASH_COUNTS, /UNDELETE");
#else
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": YOU CAN'T USE BOTH -n AND -y, --mark, --set-reserve, --extend, --rebuild-apages, --reset-crash-counts, --undelete");
#endif
		r = -EBADSYN;
		goto ret_r;
	}
	if (!memory_limit) {
		__u64 ml = GET_MEMORY_SIZE() / 2;
		if (ml > MAXULONG - BUFFER_SIZE) ml = MAXULONG - BUFFER_SIZE;
		memory_limit = ml;
		if (memory_limit < BUFFER_SIZE * 2) memory_limit = BUFFER_SIZE * 2;
	}
	device_handle = open(dev, (!ro ? O_RDWR : O_RDONLY)
#ifndef UNRELIABLE_O_DIRECT
		| (nodirect ? 0 : O_DIRECT)
#endif
		);
	if (__unlikely(device_handle < 0)) {
		r = -errno;
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": CAN'T OPEN %s: %s", dev, strerror(-r));
		goto realret;
	}
#ifdef UNRELIABLE_O_DIRECT
	if (!nodirect) {
		device_handle_unreliable = open(dev, (!ro ? O_RDWR : O_RDONLY) | O_DIRECT);
	}
#endif
	memset(&st, 0, sizeof st);
	if (__likely(!fstat(device_handle, &st))) {
		st_valid = 1;
	} else if (errno != EOVERFLOW) {
		r = -errno;
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": CAN'T FSTAT %s: %s", dev, strerror(-r));
		goto realret;
	}
	if (__unlikely(init_log())) {
		r = -errno;
		_snprintf(ERROR_MSG(), __MAX_STR_LEN, TOOL_BANNER ": CAN'T OPEN LOG FILE %s: %s", logfile, strerror(-r));
		goto realret;
	}
	if (is_mounted(dev, device_handle) >= 2) {
		if (!ro) {
			if (!mark_fsck)
				mark_fsck = 2;
		} else {
			log_printf(0, "FILESYSTEM IS MOUNTED, ERRORS ARE POSSIBLE");
			sync();
		}
	}
	check_again:
	init_buffers();
	init_alloc();
	init_super();
	init_apages();
	init_cross_links();
	init_recovery();
	if (__unlikely(r = check_super())) goto ret;
	if (mark_fsck) {
		goto ret0;
	}
	if (!force && !SPAD2CPU32_LV(&tx.txflags) && !extend && !rebuild_apages && !reset_cc && !zero_file_tails && !undelete) {
		status_printf(0, "filesystem is clean, no checking needed.");
		goto ret0;
	}
	if (__unlikely(r = check_cct())) goto ret;
	if (__unlikely(r = check_apages())) goto ret;
	if (__unlikely(r = check_dir_tree())) goto ret;
	if (__unlikely(r = fix_cct())) goto ret;
	if (__unlikely(r = fix_txblock())) goto ret;
	if (__unlikely(r = fix_apages())) goto ret;
	if (__unlikely(r = fix_errorflag())) goto ret;

	if (__likely(!alloc_over_blocks))
		print_done = 1;

	ret0:
	r = 0;
	ret:
	done_recovery();
	done_cross_links();
	done_apages();
	done_super();
	done_alloc();
	done_buffers();
	if (!r && __unlikely(alloc_over_blocks)) {
		log_printf(0, "ALLOCATED OVER EXISTING DATA - CHECKING AGAIN");
		goto check_again;
	}
	if (__unlikely(modified)) {
		int rr;
		status_printf(0, "synchronizing cache...");
		rr = flush_cache();
		if (!r) r = rr;
	}
	if (__unlikely(close(device_handle))) {
		log_printf(1, "ERROR CLOSING HANDLE: %s", strerror(errno));
		if (!r) r = 1;
	}
	device_handle = -1;
#ifdef UNRELIABLE_O_DIRECT
	if (device_handle_unreliable >= 0 && __unlikely(close(device_handle_unreliable))) {
		log_printf(1, "ERROR CLOSING DIRECT I/O HANDLE: %s", strerror(errno));
		if (!r) r = 1;
	}
	device_handle_unreliable = -1;
#endif
	if (__unlikely(done_log())) {
		if (r < 2) r = 2;
	}
	realret:
	done_dir();
	done_util();
	mem_done();
	ret_r:
	if (__unlikely(device_handle >= 0)) close(device_handle), device_handle = -1;
#ifdef UNRELIABLE_O_DIRECT
	if (__unlikely(device_handle_unreliable >= 0)) close(device_handle_unreliable), device_handle_unreliable = -1;
#endif
	if (print_done && __likely(!r))
		status_printf(0, "done.");
	if (__unlikely(r < 0)) {
#ifndef __SPAD__
		if (__likely(*ERROR_MSG())) fprintf(stderr, "%s\n", ERROR_MSG());
#else
		return r;
#endif
	}
	return return_value(r);
}
