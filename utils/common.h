#ifndef __SPAD_API_H
#define __SPAD_API_H

#ifndef __SPAD__

#ifdef __linux__
#ifdef __STRICT_ANSI__
#undef __STRICT_ANSI__
#endif
#include <asm/types.h>
#else
typedef signed char __s8;
typedef unsigned char __u8;
typedef short __s16;
typedef unsigned short __u16;
typedef int __s32;
typedef unsigned __u32;
typedef long long __s64;
typedef unsigned long long __u64;
#endif

#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef __PRINTF_ATTR__
#ifdef __GNUC__
#define __PRINTF_ATTR__(c, ch) __attribute__ ((__format__ (__printf__, c, ch)))
#define __NORET_ATTR__ __attribute__((__noreturn__))
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)
#define __COLD_ATTR__	__attribute__((__noinline__,__cold__))
#else
#define __COLD_ATTR__	__attribute__((__noinline__))
#endif
#if __GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1)
#define __finline__	__inline__ __attribute__((__always_inline__))
#else
#define __finline__	__inline__
#endif
#else
#define __PRINTF_ATTR__(c, ch)
#define __NORET_ATTR__
#define __COLD_ATTR__
#define __finline__	inline
#endif
#endif

#if !defined(__likely)
#if !defined(__GNUC__) || (__GNUC__ <= 2 && __GNUC_MINOR__ < 96)
#define __likely(x)	((int)(x))
#define __unlikely(x)	((int)(x))
#else
#define __likely(x)	(__builtin_expect((x),1))
#define __unlikely(x)	(__builtin_expect((x),0))
#endif
#endif
#define likely		__likely
#define unlikely	__unlikely

#ifndef __MAX_STR_LEN
#define __MAX_STR_LEN	256
#endif

#ifndef EBADSYN
#define EBADSYN		EINVAL
#endif
#ifndef EEOF
#define EEOF		ERANGE
#endif

#define _snprintf	snprintf
#define __sync_malloc	malloc
#define _strcasecmp	strcasecmp

#ifndef MAXULONG
#define MAXULONG	((unsigned long)-1)
#endif

#if defined(__alpha__) || defined(__ia64__)
#define __64_format	"l"
#elif defined(__linux__) && !defined(__INTEL_COMPILER)
#define __64_format	"L"
#else
#define __64_format	"ll"
#endif

#define __make64(__lo, __hi)	(((__u64)(__hi) << 32) | (__u32)(__lo))

static __finline__ int __BT(void *__ptr, int __bit)
{
	return (*((__u32 *)__ptr + (__bit >> 5)) >> (__bit & 31)) & 1;
}

static __finline__ int __BTS(void *__ptr, int __bit)
{
	int __r = __BT(__ptr, __bit);
	*((__u32 *)__ptr + (__bit >> 5)) |= 1 << (__bit & 31);
	return __r;
}

static __finline__ int __BTR(void *__ptr, int __bit)
{
	int __r = __BT(__ptr, __bit);
	*((__u32 *)__ptr + (__bit >> 5)) &= ~(1 << (__bit & 31));
	return __r;
}

static __finline__ int __BSR(unsigned long __x)
{
	int __i;
	for (__i = sizeof(unsigned long) * 8 - 1; __i > 0; __i--) if (unlikely((__x & (1UL << __i)) != 0)) return __i;
	return 0;
}

static __finline__ int __BSF(unsigned long __x)
{
	unsigned __i;
	for (__i = 0; __i < sizeof(unsigned long) * 8 - 1; __i++) if (unlikely((__x & (1UL << __i)) != 0)) return __i;
	return sizeof(unsigned long) * 8 - 1;
}

int _memcasecmp(const void *__s1, const void *__s2, size_t __count);
size_t strlcpy(char *__dest, const char *__src, size_t __count);
size_t strlcat(char *__dest, const char *__src, size_t __count);

__u64 GET_MEMORY_SIZE(void);
char *ERROR_MSG(void);
int STDIN(void);
int STDOUT(void);
int STDERR(void);
void __PRINTF_ATTR__(1,2) __NORET_ATTR__ panic(const char *__fmt, ...);
int __PRINTF_ATTR__(1,2) _printf(const char *__fmt, ...);
int _vprintf(const char *__fmt, va_list __args);
int __PRINTF_ATTR__(1,2) _eprintf(const char *__fmt, ...);
int _veprintf(const char *__fmt, va_list __args);
int __PRINTF_ATTR__(2,3) _fprintf(int __h, const char *__fmt, ...);
int _vfprintf(int __h, const char *__fmt, va_list __args);

static __finline__ void barrier(void) {
#ifdef __GNUC__
	__asm__ __volatile__ ("":::"memory");
#else
	rand();
#endif
}
#define __barrier barrier
static __finline__ void __PREFETCHT0(void *p) {}
static __finline__ void __PREFETCHT1(void *p) {}
static __finline__ void __PREFETCHT2(void *p) {}
static __finline__ void __PREFETCHNTA(void *p) {}

#include "list.h"
#include "endian.h"
#include "../meta_crc.h"
#include "../struct.h"

#else

#include <SYS/TYPES.H>
#include <SPAD/LIST.H>
#include <SPAD/SYNC.H>
#include <SPAD/VM.H>
#include <ERRNO.H>

#define likely __likely
#define unlikely __unlikely

#define GET_MEMORY_SIZE()	KERNEL$GET_MEMORY_SIZE(VM_TYPE_WIRED_MAPPED)
#define ERROR_MSG		KERNEL$ERROR_MSG
#define STDIN			KERNEL$STDIN
#define STDOUT			KERNEL$STDOUT
#define STDERR			KERNEL$STDERR
#define panic			KERNEL$SUICIDE
#define inline			__finline__

#include "ENDIAN.H"
#include "../STRUCT.H"

#endif

#endif
