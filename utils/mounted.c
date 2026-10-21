#ifndef __SPAD__
#include "mounted.h"
#else
#include "MOUNTED.H"
#endif

#ifndef __SPAD__

#include <string.h>
#include <sys/stat.h>
#include <mntent.h>

static int st_valid;
static struct stat st;

static int patheq(const char *path1, const char *path2)
{
	return !strcmp(path1, path2);
}

static int stateq(const struct stat *st1, const struct stat *st2)
{
#ifndef __SPAD__
	if (st1->st_dev == st2->st_dev && st1->st_ino == st2->st_ino)
		return 1;
	if (S_ISBLK(st1->st_mode) && S_ISBLK(st2->st_mode) && st1->st_rdev == st2->st_rdev)
		return 1;
	if (S_ISCHR(st1->st_mode) && S_ISCHR(st2->st_mode) && st1->st_rdev == st2->st_rdev)
		return 1;
#endif
	return 0;
}

static int is_mounted_in(FILE *mnt, const char *dev)
{
	int ret = 0;
	struct mntent *mntent;

	while ((mntent = getmntent(mnt))) {
		int val;
		struct stat stm;

		if (mntent->mnt_fsname[0] != '/')
			continue;

		if (hasmntopt(mntent, MNTOPT_RO))
			val = 1;
		else
			val = 2;

		if (patheq(mntent->mnt_fsname, dev)) {
			ret = val;
			break;
		}

		if (st_valid && !stat(mntent->mnt_fsname, &stm)) {
			if (stateq(&st, &stm)) {
				ret = val;
				break;
			}
		}

		if (st_valid && S_ISBLK(st.st_mode) && !strcmp(mntent->mnt_fsname, "/dev/root") && !stat(mntent->mnt_dir, &stm)) {
			if (st.st_rdev == stm.st_dev) {
				ret = val;
				break;
			}
		}
	}

	endmntent(mnt);
	return ret;
}

int is_mounted(const char *dev, int h)
{
	FILE *mnt;
	st_valid = 0;
	if (h < 0) {
		if (!stat(dev, &st)) st_valid = 1;
	} else {
		if (!fstat(h, &st)) st_valid = 1;
	}
	if ((mnt = setmntent("/proc/mounts", "r"))) return is_mounted_in(mnt, dev);
	if ((mnt = setmntent(_PATH_MOUNTED, "r"))) return is_mounted_in(mnt, dev);
	return 0;
}

#else

int is_mounted(const char *dev, int h)
{
	return 0;
}

#endif
