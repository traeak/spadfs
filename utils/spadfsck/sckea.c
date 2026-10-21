#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#define FLAGS_NEED_RDEV		1
#define FLAGS_HAVE_RDEV		2
#define FLAGS_DONT_HAVE_RDEV	4

static char *ea_check_unx(struct fnode *fnode, struct ea_unx *unx, int type, int *flags, __u32 *prealloc)
{
	if (__unlikely((SPAD2CPU32_LV(&unx->magic) & EA_UNX_MAGIC_MASK) != (EA_UNX_MAGIC & EA_UNX_MAGIC_MASK))) return "UNX ATTRIBUTE HAS INVALID LENGTH";

	if (__unlikely(!(type & EA_TYPE_FILE))) return "UNX ATTRIBUTE NOT ALLOWED ON HARDLINK";

	if (!LINUX_S_ISLNK(SPAD2CPU16_LV(&unx->mode)) &&
	    !LINUX_S_ISREG(SPAD2CPU16_LV(&unx->mode)) &&
	    !LINUX_S_ISDIR(SPAD2CPU16_LV(&unx->mode)) &&
	    !LINUX_S_ISCHR(SPAD2CPU16_LV(&unx->mode)) &&
	    !LINUX_S_ISBLK(SPAD2CPU16_LV(&unx->mode)) &&
	    !LINUX_S_ISFIFO(SPAD2CPU16_LV(&unx->mode)) &&
	    !LINUX_S_ISSOCK(SPAD2CPU16_LV(&unx->mode)))
		return "UNX ATTRIBUTE ERROR: INVALID MODE";
	if (__unlikely(fnode->flags & FNODE_FLAGS_DIR)) {
		if (__unlikely(!LINUX_S_ISDIR(SPAD2CPU16_LV(&unx->mode)))) return "UNX ATTRIBUTE ERROR: DIRECTORY HAS NON-DIRECTORY MODE";
	} else {
		if (__unlikely(LINUX_S_ISDIR(SPAD2CPU16_LV(&unx->mode)))) return "UNX ATTRIBUTE ERROR: NON-DIRECTORY HAS DIRECTORY MODE";
	}
	if (__likely(LINUX_S_ISREG(SPAD2CPU16_LV(&unx->mode)))) {
		if (__likely(!(type & EA_ZERO_PREALLOC))) {
			__u64 size = SPAD2CPU64(fnode->size[!cc_valid(fnode->cc, fnode->txc)]);
			unsigned pa = SPAD2CPU32(unx->prealloc[!cc_valid(fnode->cc, fnode->txc)]);
			if (__unlikely(pa > size)) return "UNX ATTRIBUTE ERROR: PREALLOC IS LARGER THAN FILE SIZE";
			*prealloc = pa;
		} else {
			unx->prealloc[0] = unx->prealloc[1] = CPU2SPAD32(0);
		}
	} else {
		if (__unlikely((SPAD2CPU32_LV(&unx->prealloc[0]) | SPAD2CPU32_LV(&unx->prealloc[1])) != 0)) return "UNX ATTRIBUTE ERROR: NON-FILE FNODE HAS PREALLOC";
	}
	if (__unlikely(LINUX_S_ISCHR(SPAD2CPU16_LV(&unx->mode))) || __unlikely(LINUX_S_ISBLK(SPAD2CPU16_LV(&unx->mode)))) {
		if (*flags & FLAGS_DONT_HAVE_RDEV) return "UNX ATTRIBUTE ERROR: DEVICE DOESN'T HAVE RDEV ATTRIBUTE";
		*flags |= FLAGS_NEED_RDEV;
	}
	if (SPAD2CPU32_LV(&unx->magic) == EA_UNX_MAGIC) {
		if (ino_claim(SPAD2CPU64_LV(&unx->ino)))
			return "UNX ATTRIBUTE ERROR: INVALID INODE NUMBER";
	}
	return NULL;
}

static char *ea_check_symlink(struct fnode *fnode, struct fnode_ea *ea, int type)
{
	__u64 ino;
	unsigned len = (SPAD2CPU32_LV(&ea->magic) >> FNODE_EA_SIZE_SHIFT) & FNODE_EA_SIZE_MASK_1;
	unsigned i;

	if (__unlikely(!len)) return "SYMLINK ATTRIBUTE HAS ZERO LENGTH";

	if (__unlikely(!(type & EA_TYPE_FILE))) return "SYMLINK ATTRIBUTE NOT ALLOWED ON HARDLINK";

	for (i = 0; i < len; i++) if (__unlikely(!((char *)(ea + 1))[i])) return "SYMLINK ATTRIBUTE CONTAINS NULL CHARACTER";
	ino = ((__u64)SPAD2CPU32_LV(&fnode->run10)) |
	      ((__u64)SPAD2CPU16_LV(&fnode->run11) << 32) |
	      ((__u64)SPAD2CPU16_LV(&fnode->run1n) << 48);
	if (ino) {
		if (ino_claim(ino))
			return "SYMLINK: INVALID INODE NUMBER";
	}
	return NULL;

}

static char *ea_check_rdev(struct fnode_ea *ea, int type, int *flags)
{
	if (__unlikely(ea->magic != CPU2SPAD32(EA_RDEV_MAGIC))) return "RDEV ATTRIBUTE HAS INVALID LENGTH";

	if (__unlikely(!(type & EA_TYPE_FILE))) return "RDEV ATTRIBUTE NOT ALLOWED ON HARDLINK";

	*flags |= FLAGS_HAVE_RDEV;
	return NULL;
}

static char *ea_check_xattr(struct fnode_ea *ea, int type)
{
	__u8 *xat_history[FNODE_MAX_EA_SIZE];
	unsigned xat_history_used = 0;
	__u8 *xat;
	unsigned xat_size = (SPAD2CPU32_LV(&ea->magic) >> FNODE_EA_SIZE_SHIFT) & FNODE_EA_SIZE_MASK_1;

	if (__unlikely(!(type & EA_TYPE_FILE))) return "XATTR ATTRIBUTE NOT ALLOWED ON HARDLINK";

	xat = (__u8 *)(ea + 1);
	while (1) {
		unsigned i;

		xat = GET_XAT(xat, xat_size, GET_XAT_ALL, 0, NULL, 0);
		if (xat == GET_XAT_ERROR) return "XATTR: ATTRIBUTE ERROR";
		if (!xat) break;

		for (i = 0; i < xat_history_used; i++) {
			if (xat_history[i][0] == xat[0] &&
			    xat_history[i][1] == xat[1] &&
			    !memcmp(&xat_history[i][3], &xat[3], xat[1])) {
				return "XATTR: DUPLICATE ATTRIBUTE";
			}
		}
		xat_history[xat_history_used++] = xat;

		xat_size -= 3 + xat[1] + xat[2];
		xat += 3 + xat[1] + xat[2];
	}
	return NULL;
}

static char *check_specific(struct fnode *fnode, struct fnode_ea *ea, int type, int *flags, __u32 *prealloc)
{
	switch (SPAD2CPU32_LV(&ea->magic) & FNODE_EA_MAGIC_MASK) {
		case EA_UNX_MAGIC & FNODE_EA_MAGIC_MASK: {
			return ea_check_unx(fnode, (struct ea_unx *)ea, type, flags, prealloc);
		}
		case EA_SYMLINK_MAGIC & FNODE_EA_MAGIC_MASK: {
			return ea_check_symlink(fnode, ea, type);
		}
		case EA_RDEV_MAGIC & FNODE_EA_MAGIC_MASK: {
			return ea_check_rdev(ea, type, flags);
		}
		case EA_XATTR_MAGIC & EA_XATTR_MAGIC_MASK: {
			return ea_check_xattr(ea, type);
		}
	}
	return NULL;
}

static char *check_ea_err(struct fnode *fnode, struct fnode_ea *ea_start, unsigned ea_size, int type, unsigned *err_offset, unsigned *err_size, __u32 *prealloc)
{
	unsigned off;
	int flags = 0;
	__u32 local_prealloc;
	if (prealloc)
		*prealloc = 0;
	again:
	local_prealloc = 0;
	off = 0;
	while (off < ea_size) {
		char *c;
		struct fnode_ea *ea = (struct fnode_ea *)((char *)ea_start + off);
		unsigned rec_size = (((SPAD2CPU32_LV(&ea->magic) >> FNODE_EA_SIZE_SHIFT) & FNODE_EA_SIZE_MASK_1) + FNODE_EA_SIZE_ADD) & FNODE_EA_SIZE_MASK_2;
		*err_offset = off;
		if (__unlikely(off + rec_size > ea_size)) {
			*err_size = ea_size - off;
			return "RECORD BEYOND END";
		}
		*err_size = rec_size;
		{
			unsigned off1 = 0;
			while (off1 < off) {
				struct fnode_ea *ea1 = (struct fnode_ea *)((char *)ea_start + off1);
				if (__unlikely((SPAD2CPU32_LV(&ea1->magic) & FNODE_EA_MAGIC_MASK) == (SPAD2CPU32_LV(&ea->magic) & FNODE_EA_MAGIC_MASK))) {
					return "DUPLICATE ATTRIBUTE";
				}
				off1 += (((SPAD2CPU32_LV(&ea1->magic) >> FNODE_EA_SIZE_SHIFT) & FNODE_EA_SIZE_MASK_1) + FNODE_EA_SIZE_ADD) & FNODE_EA_SIZE_MASK_2;
			}
			if (__unlikely(off != off1))
				panic("check_ea_err: desynced when scanning for duplicate ea (%u != %u)", off, off1);
		}
		c = check_specific(fnode, ea, type, &flags, &local_prealloc);
		if (__unlikely(c != NULL)) return c;
		off += rec_size;
	}
	if (__unlikely((flags & (FLAGS_NEED_RDEV | FLAGS_HAVE_RDEV)) == FLAGS_NEED_RDEV)) {
		flags |= FLAGS_DONT_HAVE_RDEV;
		goto again;
	}
	if (prealloc)
		*prealloc = local_prealloc;
	return NULL;
}

char *check_ea(struct fnode *fnode, struct fnode_ea *ea_start, unsigned ea_size, int type, __u32 *prealloc)
{
	unsigned sink;
	return check_ea_err(fnode, ea_start, ea_size, type, &sink, &sink, prealloc);
}

void fix_ea(struct fnode *fnode, struct fnode_ea *ea, unsigned *ea_size, int type)
{
	unsigned err_offset, err_size;
	while (__unlikely(check_ea_err(fnode, ea, *ea_size, type, &err_offset, &err_size, NULL) != NULL)) {
		memmove((char *)ea + err_offset, (char *)ea + err_offset + err_size, *ea_size - err_offset - err_size);
		*ea_size -= err_size;
	}
}
