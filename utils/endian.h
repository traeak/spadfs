#ifndef _SPADFS_UTILS_ENDIAN_H
#define _SPADFS_UTILS_ENDIAN_H

extern int endian_big;

#ifndef __SPAD__

static __finline__ __u16 __16CPU2LE(__u16 val)
{
	__u16 x;
	((__u8 *)&x)[0] = val;
	((__u8 *)&x)[1] = val >> 8;
	return x;
}

static __finline__ __u16 __16LE2CPU(__u16 val)
{
	return	((__u16)((__u8 *)&val)[0]) |
		((__u16)((__u8 *)&val)[1] << 8);
}

static __finline__ __u32 __32CPU2LE(__u32 val)
{
	__u32 x;
	((__u8 *)&x)[0] = val;
	((__u8 *)&x)[1] = val >> 8;
	((__u8 *)&x)[2] = val >> 16;
	((__u8 *)&x)[3] = val >> 24;
	return x;
}

static __finline__ __u32 __32LE2CPU(__u32 val)
{
	return	((__u32)((__u8 *)&val)[0]) |
		((__u32)((__u8 *)&val)[1] << 8) |
		((__u32)((__u8 *)&val)[2] << 16) |
		((__u32)((__u8 *)&val)[3] << 24);
}

static __finline__ __u64 __64CPU2LE(__u64 val)
{
	__u64 x;
	((__u8 *)&x)[0] = val;
	((__u8 *)&x)[1] = val >> 8;
	((__u8 *)&x)[2] = val >> 16;
	((__u8 *)&x)[3] = val >> 24;
	((__u8 *)&x)[4] = val >> 32;
	((__u8 *)&x)[5] = val >> 40;
	((__u8 *)&x)[6] = val >> 48;
	((__u8 *)&x)[7] = val >> 56;
	return x;
}

static __finline__ __u64 __64LE2CPU(__u64 val)
{
	return	((__u64)((__u8 *)&val)[0]) |
		((__u64)((__u8 *)&val)[1] << 8) |
		((__u64)((__u8 *)&val)[2] << 16) |
		((__u64)((__u8 *)&val)[3] << 24) |
		((__u64)((__u8 *)&val)[4] << 32) |
		((__u64)((__u8 *)&val)[5] << 40) |
		((__u64)((__u8 *)&val)[6] << 48) |
		((__u64)((__u8 *)&val)[7] << 56);
}

static __finline__ __u16 __16CPU2BE(__u16 val)
{
	__u16 x;
	((__u8 *)&x)[0] = val >> 8;
	((__u8 *)&x)[1] = val;
	return x;
}

static __finline__ __u16 __16BE2CPU(__u16 val)
{
	return	((__u16)((__u8 *)&val)[0] << 8) |
		((__u16)((__u8 *)&val)[1]);
}

static __finline__ __u32 __32CPU2BE(__u32 val)
{
	__u32 x;
	((__u8 *)&x)[0] = val >> 24;
	((__u8 *)&x)[1] = val >> 16;
	((__u8 *)&x)[2] = val >> 8;
	((__u8 *)&x)[3] = val;
	return x;
}

static __finline__ __u32 __32BE2CPU(__u32 val)
{
	return	((__u32)((__u8 *)&val)[0] << 24) |
		((__u32)((__u8 *)&val)[1] << 16) |
		((__u32)((__u8 *)&val)[2] << 8) |
		((__u32)((__u8 *)&val)[3]);
}

static __finline__ __u64 __64CPU2BE(__u64 val)
{
	__u64 x;
	((__u8 *)&x)[0] = val >> 56;
	((__u8 *)&x)[1] = val >> 48;
	((__u8 *)&x)[2] = val >> 40;
	((__u8 *)&x)[3] = val >> 32;
	((__u8 *)&x)[4] = val >> 24;
	((__u8 *)&x)[5] = val >> 16;
	((__u8 *)&x)[6] = val >> 8;
	((__u8 *)&x)[7] = val;
	return x;
}

static __finline__ __u64 __64BE2CPU(__u64 val)
{
	return	((__u64)((__u8 *)&val)[0] << 56) |
		((__u64)((__u8 *)&val)[1] << 48) |
		((__u64)((__u8 *)&val)[2] << 40) |
		((__u64)((__u8 *)&val)[3] << 32) |
		((__u64)((__u8 *)&val)[4] << 24) |
		((__u64)((__u8 *)&val)[5] << 16) |
		((__u64)((__u8 *)&val)[6] << 8) |
		((__u64)((__u8 *)&val)[7]);
}

#define __16LE2CPU_CONST(__x)		__16LE2CPU(__x)
#define __16CPU2LE_CONST(__x)		__16CPU2LE(__x)
#define __32LE2CPU_CONST(__x)		__32LE2CPU(__x)
#define __32CPU2LE_CONST(__x)		__32CPU2LE(__x)
#define __64LE2CPU_CONST(__x)		__64LE2CPU(__x)
#define __64CPU2LE_CONST(__x)		__64CPU2LE(__x)

#define __16BE2CPU_CONST(__x)		__16BE2CPU(__x)
#define __16CPU2BE_CONST(__x)		__16CPU2BE(__x)
#define __32BE2CPU_CONST(__x)		__32BE2CPU(__x)
#define __32CPU2BE_CONST(__x)		__32CPU2BE(__x)
#define __64BE2CPU_CONST(__x)		__64BE2CPU(__x)
#define __64CPU2BE_CONST(__x)		__64CPU2BE(__x)

#define __16LE2CPU_LV(__x)		__16LE2CPU(*(__x))
#define __16CPU2LE_LV(__x, __y)		(*(__x) = __16CPU2LE(__y))
#define __32LE2CPU_LV(__x)		__32LE2CPU(*(__x))
#define __32CPU2LE_LV(__x, __y)		(*(__x) = __32CPU2LE(__y))
#define __64LE2CPU_LV(__x)		__64LE2CPU(*(__x))
#define __64CPU2LE_LV(__x, __y)		(*(__x) = __64CPU2LE(__y))

#define __16BE2CPU_LV(__x)		__16BE2CPU(*(__x))
#define __16CPU2BE_LV(__x, __y)		(*(__x) = __16CPU2BE(__y))
#define __32BE2CPU_LV(__x)		__32BE2CPU(*(__x))
#define __32CPU2BE_LV(__x, __y)		(*(__x) = __32CPU2BE(__y))
#define __64BE2CPU_LV(__x)		__64BE2CPU(*(__x))
#define __64CPU2BE_LV(__x, __y)		(*(__x) = __64CPU2BE(__y))

#endif

#define SPAD2CPU16(x)		(!endian_big ? __16LE2CPU(x) : __16BE2CPU(x))
#define CPU2SPAD16(x)		(!endian_big ? __16CPU2LE(x) : __16CPU2BE(x))
#define SPAD2CPU32(x)		(!endian_big ? __32LE2CPU(x) : __32BE2CPU(x))
#define CPU2SPAD32(x)		(!endian_big ? __32CPU2LE(x) : __32CPU2BE(x))
#define SPAD2CPU64(x)		(!endian_big ? __64LE2CPU(x) : __64BE2CPU(x))
#define CPU2SPAD64(x)		(!endian_big ? __64CPU2LE(x) : __64CPU2BE(x))
#define SPAD2CPU16_LV(x)	(!endian_big ? __16LE2CPU_LV(x) : __16BE2CPU_LV(x))
#define CPU2SPAD16_LV(r, x)	do { if (!endian_big) __16CPU2LE_LV(r, x); else __16CPU2BE_LV(r, x); } while (0)
#define SPAD2CPU32_LV(x)	(!endian_big ? __32LE2CPU_LV(x) : __32BE2CPU_LV(x))
#define CPU2SPAD32_LV(r, x)	do { if (!endian_big) __32CPU2LE_LV(r, x); else __32CPU2BE_LV(r, x); } while (0)
#define SPAD2CPU64_LV(x)	(!endian_big ? __64LE2CPU_LV(x) : __64BE2CPU_LV(x))
#define CPU2SPAD64_LV(r, x)	do { if (!endian_big) __64CPU2LE_LV(r, x); else __64CPU2BE_LV(r, x); } while (0)
#define SPAD2CPU16_CONST(x)	(!endian_big ? __16LE2CPU_CONST(x) : __16BE2CPU_CONST(x))
#define CPU2SPAD16_CONST(x)	(!endian_big ? __16CPU2LE_CONST(x) : __16CPU2BE_CONST(x))
#define SPAD2CPU32_CONST(x)	(!endian_big ? __32LE2CPU_CONST(x) : __32BE2CPU_CONST(x))
#define CPU2SPAD32_CONST(x)	(!endian_big ? __32CPU2LE_CONST(x) : __32CPU2BE_CONST(x))
#define SPAD2CPU64_CONST(x)	(!endian_big ? __64LE2CPU_CONST(x) : __64BE2CPU_CONST(x))
#define CPU2SPAD64_CONST(x)	(!endian_big ? __64CPU2LE_CONST(x) : __64CPU2BE_CONST(x))

#endif
