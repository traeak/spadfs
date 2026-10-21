#include <unistd.h>

#include "common.h"

LIST_ENTRY LIST_END;

char *ERROR_MSG(void)
{
	static char err[__MAX_STR_LEN] = "";
	return err;
}

int STDIN() { return 0; }
int STDOUT() { return 1; }
int STDERR() { return 2; }

static char printf_buffer[4096];

int _vfprintf(int h, __const__ char *fmt, va_list args)
{
	int r;
	char *pb;
	vsnprintf(printf_buffer, sizeof printf_buffer, fmt, args);
	pb = printf_buffer;
	while (*pb) {
		r = write(h, pb, strlen(pb));
		if (__unlikely(r <= 0)) return -1;
		pb += r;
	}
	return strlen(printf_buffer);
}

int _fprintf(int h, __const__ char *fmt, ...)
{
	int r;
	va_list va;
	va_start(va, fmt);
	r = _vfprintf(h, fmt, va);
	va_end(va);
	return r;
}

int _veprintf(__const__ char *fmt, va_list args)
{
	return _vfprintf(STDERR(), fmt, args);
}

int _eprintf(__const__ char *fmt, ...)
{
	int r;
	va_list va;
	va_start(va, fmt);
	r = _veprintf(fmt, va);
	va_end(va);
	return r;
}

int _vprintf(__const__ char *fmt, va_list args)
{
	return _vfprintf(STDOUT(), fmt, args);
}

int _printf(__const__ char *fmt, ...)
{
	int r;
	va_list va;
	va_start(va, fmt);
	r = _vprintf(fmt, va);
	va_end(va);
	return r;
}

void panic(__const__ char *fmt, ...)
{
	va_list va;
	_eprintf("SUICIDE: ");
	va_start(va, fmt);
	_veprintf(fmt, va);
	va_end(va);
	_eprintf("\n");
	abort();
}

int _memcasecmp(__const__ void *s1, __const__ void *s2, size_t count)
{
	__const__ char *p1 = s1, *p2 = s2;
	while (count--) {
		unsigned char c1 = *p1, c2 = *p2;
		if (c1 >= 'a' && c1 <= 'z') c1 -= 0x20;
		if (c2 >= 'a' && c2 <= 'z') c2 -= 0x20;
		if (__unlikely(c1 != c2)) return c2 - c1;
		p1++, p2++;
	}
	return 0;
}

size_t strlcpy(char *dest, __const__ char *src, size_t count)
{
	size_t l = strlen(src);
	if (__likely(l < count)) {
		strcpy(dest, src);
	} else if (count) {
		memcpy(dest, src, count - 1);
		dest[count - 1] = 0;
	}
	return l;
}

size_t strlcat(char *dest, __const__ char *src, size_t count)
{
	size_t l = strlen(dest);
	size_t ll = strlen(src);
	if (l + ll < count) {
		strcat(dest, src);
	} else if (count) {
		memcpy(dest + l, src, count - l - 1);
		dest[count - 1] = 0;
	}
	return l + ll;
}

__u64 GET_MEMORY_SIZE(void)
{
#ifdef __linux__
#if defined(__hppa__) || defined(__x86_64__)
#define linux_max_va_size	4194304
#else
#define linux_max_va_size	2097152
#endif
	__u64 mem;
	FILE *f;
	char buffer[64];
	f = fopen("/proc/meminfo", "r");
	if (!f)
		goto no_fopen;
	mem = 0;
	while (fgets(buffer, sizeof(buffer), f)) {
		if (!strchr(buffer, '\n')) {
			int c;
			do c = getc(f); while (c != EOF && c != '\n');
			if (c == EOF)
				break;
			continue;
		}
		if (!strncmp(buffer, "MemFree:", 8) ||
		    !strncmp(buffer, "Buffers:", 8) ||
		    !strncmp(buffer, "Cached:", 7)) {
			unsigned long long val;
			size_t off = strcspn(buffer, "0123456789");
			val = strtoull(buffer + off, NULL, 10);
			mem += val;
		}
	}
	fclose(f);
	if (!mem)
		goto no_fopen;
	if (sizeof(void *) <= 4 && mem > linux_max_va_size)
		mem = linux_max_va_size;
	return mem << 10;
no_fopen:
#endif
	return 128 << 20;
}


