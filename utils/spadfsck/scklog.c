#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

static int logh = -1;
static int was_log_error = 0;
static int status_line = 0;

static void log_only_vprintf(char *msg, va_list va);
static void __PRINTF_ATTR__(1,2) log_only_printf(char *msg, ...);

static void pre_clr_status(void)
{
	if (status_line) _eprintf("\r");
}

static void clr_status(void)
{
	if (status_line) {
		_eprintf("\e[0K");
		status_line = 0;
	}
}

int init_log(void)
{
	if (!logfile) return 0;
	logh = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (__unlikely(logh == -1)) return 1;
	log_only_printf("\nSPADFSCK STARTED AT %"__64_format"d ON %s\n", (__u64)time(NULL), dev);
	if (was_log_error) {
		close(logh);
		logh = -1;
		errno = was_log_error;
		return 1;
	}
	return 0;
}

static void log_only_vprintf(char *msg, va_list va)
{
	if (logh != -1) {
		int snc;
		errno = 0;
		if (__unlikely(_vfprintf(logh, msg, va) == -1)) {
			snc = 0;
			log_er:
			was_log_error = errno;
			if (!was_log_error) was_log_error = EEOF;
			close(logh);
			logh = -1;
			pre_clr_status();
			clr_status();
			_printf("ERROR %s LOG %s: %s\n", !snc ? "WRITING TO" : "SYNCING", logfile, strerror(was_log_error));
			return;
		}
		if (__unlikely(sync_writes)) {
			if (__unlikely(fdatasync(logh))) {
				snc = 1;
				goto log_er;
			}
		}
	}
}

static void log_only_printf(char *msg, ...)
{
	va_list va;
	va_start(va, msg);
	log_only_vprintf(msg, va);
	va_end(va);
}

void log_printf(int important, char *msg, ...)
{
	va_list va;
	pre_clr_status();
	if (important) _eprintf("\e[1m");
	va_start(va, msg);
	_vprintf(msg, va);
	va_end(va);
	if (important) _eprintf("\e[0m");
	clr_status();
	_printf("\n");
	va_start(va, msg);
	log_only_vprintf(msg, va);
	va_end(va);
	log_only_printf("\n");
}

void status_printf(int r, char *msg, ...)
{
	va_list va;
	if (r) {
		static time_t lt = 0;
		time_t t = time(NULL);
		if (t == lt && status_line) return;
		lt = t;
	}
	pre_clr_status();
	va_start(va, msg);
	if (!r) _vprintf(msg, va);
	else _veprintf(msg, va);
	va_end(va);
	clr_status();
	if (!r) _printf("\n");
	else status_line = 1;
}

void print_progress(blk_t total, blk_t partial, int exact)
{
	unsigned percent;
	if (__unlikely(!total)) return;
	percent = ((__u64)partial * 100 / total);
	if (!exact) status_printf(1, "%u%%", percent);
	else status_printf(1, "%u%%  (%"blk_format"u of %"blk_format"u)", percent, partial, total);
}

void log_suppress(void)
{
	log_printf(0, "SUPPRESSING SIMILAR OUTPUT");
}

static DECL_XLIST(msglines);

typedef struct {
	LIST_ENTRY list;
	char *file;
	long line;
} MSGLINE;

int query(char *file, long line, int flags, char *msg, ...)
{
	MSGLINE *m;
	char c[2];
	char reply[10];
	va_list va;
	if (__unlikely(ro)) goto ret_0;
	if (y) {
		va_start(va, msg);
		log_only_vprintf(msg, va);
		va_end(va);
		if (__unlikely(flags & Q_IMPORTANT)) {
			log_only_printf(" NO (DANGEROUS ACTIONS CANNOT BE AUTOANSWERED)\n");
			goto ret_0;
		}
		yes_all:
		log_only_printf(" YES (ALL)\n");
		goto ret_1;
	}
	if (!(flags & Q_IMPORTANT)) XLIST_FOR_EACH(m, &msglines, MSGLINE, list) if (!strcmp(m->file, file) && m->line == line) {
		va_start(va, msg);
		log_only_vprintf(msg, va);
		va_end(va);
		yes_same:
		log_only_printf(" YES (SAME)\n");
		goto ret_1;
	}
	retry:
	pre_clr_status();
	if (__unlikely(flags & Q_IMPORTANT)) _eprintf("\e[1m");
	va_start(va, msg);
	_vprintf(msg, va);
	va_end(va);
	va_start(va, msg);
	log_only_printf(msg, va);
	va_end(va);
	if (__likely(!(flags & Q_IMPORTANT))) _eprintf(" (Y/N/SAME/ALL) ");
	else _eprintf("\e[0m (YES/NO) ");
	clr_status();
	if (y && flags & Q_IMPORTANT) {
		log_printf(0, " NO (RUN SPADFSCK AGAIN WITHOUT /Y)");
		goto ret_0;
	}
	reply[0] = 0;
	next_char:
	errno = 0;
	if (__unlikely(read(STDIN(), c, 1) != 1)) {
		int e;
		was_stdin_error = 1;
		e = errno ? errno : EEOF;
		_eprintf("\n");
		log_only_printf(" NO (STDIN ERROR)\n");
		log_printf(1, "STDIN ERROR %s", strerror(e));
		goto ret_0;
	}
	if (c[0] != '\n') {
		c[1] = 0;
		strlcat(reply, c, sizeof reply);
		goto next_char;
	}
	if (!_strcasecmp(reply, "YES")) {
		yes:
		log_only_printf(" YES\n");
		goto ret_1;
	}
	if (!_strcasecmp(reply, "NO")) {
		no:
		log_only_printf(" NO\n");
		goto ret_0;
	}
	if (!_strcasecmp(reply, "N")) goto no;
	if (flags & Q_IMPORTANT) goto retry;
	if (!_strcasecmp(reply, "Y")) goto yes;
	if (!_strcasecmp(reply, "ALL")) {
		y = 1;
		goto yes_all;
	}
	if (!_strcasecmp(reply, "SAME")) {
		m = mem_alloc(sizeof(MSGLINE));
		if (m) {
			m->file = file;
			m->line = line;
			ADD_TO_XLIST(&msglines, &m->list);
		}
		goto yes_same;
	}
	goto retry;

	ret_1:
	if (__likely(!(flags & Q_NOFIX)))
		answered_yes = 1;
	return 1;

	ret_0:
	if (__likely(!(flags & Q_NOFIX)))
		answered_no = 1;
	return 0;
}

int done_log(void)
{
	while (!XLIST_EMPTY(&msglines)) {
		MSGLINE *m = LIST_STRUCT(msglines.next, MSGLINE, list);
		DEL_FROM_LIST(&m->list);
		mem_free(m);
	}
	if (logh != -1) {
		log_only_printf("SPADFSCK ENDED AT %"__64_format"d ON %s\n\n", (__u64)time(NULL), dev);
	}
	if (__unlikely(was_log_error)) {
		log_printf(1, "THERE WAS ERROR WRITING TO LOG %s: %s", logfile, strerror(was_log_error));
	}
	if (logh != -1) {
		int r;
		r = fsync(logh);
		if (__unlikely(r)) {
			if (!was_log_error) was_log_error = errno;
			log_printf(1, "ERROR SYNCING LOG FILE %s: %s", logfile, strerror(errno));
		}
		r = close(logh);
		logh = -1;
		if (__unlikely(r)) {
			if (!was_log_error) was_log_error = errno;
			log_printf(1, "ERROR CLOSING LOG FILE %s: %s", logfile, strerror(errno));
		}
	}
	if (was_log_error) return 2;
	return 0;
}
