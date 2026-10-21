#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#define single_escape(c)	(__unlikely((unsigned char)(c) == '"') ||  \
				 __unlikely((unsigned char)(c) == '\'') || \
				 __unlikely((unsigned char)(c) == '\\'))

#define need_escape(c)		(single_escape(c) ||			   \
				 __unlikely((unsigned char)(c) < ' ') ||   \
				 __unlikely((unsigned char)(c) == ':') ||  \
				 __unlikely((unsigned char)(c) == 127) ||  \
				 __unlikely((unsigned char)(c) == 155))

#define N_ESC_STORES	2

static unsigned escstore = 0;

static char *escname[N_ESC_STORES] = { NULL, NULL };
static size_t escnamelen[N_ESC_STORES] = { 0, 0 };

const char *escape_name(const char *name)
{
	size_t needlen;
	const char *p;
	char *q;
	for (p = name; *p; p++) if (need_escape(*p)) goto do_escape;
	return name;
	do_escape:
	escstore = (escstore + 1) % N_ESC_STORES;
	needlen = strlen(name) * 4 + 1;
	if (__unlikely(escnamelen[escstore] < needlen)) {
		mem_free(escname[escstore]);
		escname[escstore] = mem_alloc(escnamelen[escstore] = needlen);
		if (!escname[escstore]) {
			escnamelen[escstore] = 0;
			log_printf(1, "CAN'T ALLOC MEMORY TO ESCAPE FILENAME");
			return "NO_MEMORY";
		}
	}
	for (p = name, q = escname[escstore]; *p; p++) {
		unsigned char c = *p;
		if (!need_escape(c)) *q++ = c;
		else if (single_escape(c)) {
			*q++ = '\\';
			*q++ = c;
		} else {
			*q++ = '\\';
			*q++ = '0' + c / 64;
			*q++ = '0' + c / 8 % 8;
			*q++ = '0' + c % 8;
		}
	}
	*q = 0;
	return escname[escstore];
}

char *ptr_error;

int ptr_valid(blk_t blk, unsigned n_blks)
{
	if (__unlikely(blk + n_blks < blk) ||
	    __unlikely(blk + n_blks > SPAD2CPU64_LV(&super.size))) {
		ptr_error = "OUT OF RANGE";
		return 0;
	}
	if (__likely(st_valid) && __unlikely(S_ISREG(st.st_mode) && __unlikely(blk + n_blks > st.st_size >> 9))) {
		fstat(device_handle, &st);
		if (__unlikely(S_ISREG(st.st_mode) && __unlikely(blk + n_blks > st.st_size >> 9))) {
			ptr_error = "BEYOND FILE END";
			return 0;
		}
	}
	if (__unlikely(blk <= SUPERBLOCK_SECTOR)) {
		if (!(SPAD2CPU32_LV(&super.flags_compat_none) & FLAG_COMPAT_NONE_EXTRA_SPACE)) {
			ptr_error = "POINTS TO RESERVED AREA";
			return 0;
		}
		if (blk + n_blks > (USERBLOCK_SECTOR & ~((1 << super.sectors_per_block_bits) - 1))) {
			ptr_error = "POINTS TO SUPERBLOCK AREA";
			return 0;
		}
		if (blk < BOOTLOADER_SECTORS) {
			ptr_error = "POINTS TO BOOTLOADER AREA";
			return 0;
		}
	}
	if (__unlikely(((unsigned long)blk & ((1 << super.sectors_per_block_bits) - 1)) != 0)) {
		ptr_error = "NOT ALIGNED ON BLOCK BOUNDARY";
		return 0;
	}
	if (__unlikely(((unsigned long)blk & ((1 << super.sectors_per_page_bits) - 1)) + n_blks > 1 << super.sectors_per_page_bits)) {
		ptr_error = "CROSSES PAGE BOUNDARY";
		return 0;
	}
	return 1;
}

void done_util(void)
{
	unsigned i;
	for (i = 0; i < N_ESC_STORES; i++)
		if (__unlikely(escname[i] != NULL)) mem_free(escname[i]);
}

