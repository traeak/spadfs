#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

static int overflow = 0;
static __u64 max_ino = 0;

int ino_claim(__u64 ino)
{
	if (ino >= max_ino) {
		max_ino = ino + 1;
		if (__unlikely(!max_ino))
			overflow = 1;
	}
	return 0;
}

__u64 ino_get_max(void)
{
	if (overflow)
		return (__u64)SPADFS_INO_INITIAL_REGION * 2;
	return max_ino;
}
