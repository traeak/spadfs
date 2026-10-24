#ifndef _SPADFS_LINUX_COMPAT_H
#define _SPADFS_LINUX_COMPAT_H

/*
 * Included by spadfs.h after <linux/version.h>, so this is already reachable in
 * the real build. Pulled in explicitly so the header is self-contained when
 * analyzed on its own. linux/version.h is a pure UAPI header with no dependency
 * on anything in this tree, so unlike a common.h/struct.h cycle this is safe.
 */
#include <linux/version.h>

/*
 * Kernel API compatibility layer.
 *
 * FLOOR: Linux 6.1 LTS.
 *
 * Everything below either (a) bridges APIs that changed between 6.1 and the
 * kernels we build against, or (b) is a plain fallback with no version gate.
 *
 * The rule used when this was trimmed: a `#if LINUX_VERSION_CODE < X` shim is
 * dead only when X <= 6.1, because no supported kernel is older. A
 * `>= X` gate is dead only when X < 6.1 -- and if X is above the floor it must
 * be KEPT, since we still build against newer kernels where it fires.
 *
 * Distro-kernel support (RHEL 5/6/7, stable-branch backports) was dropped at
 * the same time. That removed the compound conditions that made this file hard
 * to read, e.g.
 *   `#if < 3.15.0 && !TEST_STABLE_BRANCH(3,12,41) && !TEST_RHEL_VERSION(6,8)
 *        && !TEST_RHEL_VERSION(7,1)`
 * and it is a bigger win than moving the floor from 6.1 to 6.7 would have been.
 *
 * To support an older kernel you must restore a floor here and re-add the shims
 * this file no longer provides.
 */

#ifdef CONFIG_FORTIFY_SOURCE
/*
 * Not version-gated: under CONFIG_FORTIFY_SOURCE a plain memcpy() can trip
 * false-positive field-spanning warnings, so copy byte by byte.
 */
static inline void *spadfs_memcpy(void *dest, const void *src, size_t len)
{
	__u8 *d = dest;
	const __u8 *s = src;
	while (len--)
		*d++ = *s++;
	return dest;
}
static inline void *spadfs_memmove(void *dest, const void *src, size_t len)
{
	__u8 *d = dest;
	const __u8 *s = src;
	if (dest < src)
		return spadfs_memcpy(dest, src, len);
	while (len--)
		d[len] = s[len];
	return dest;
}
#else
#define spadfs_memcpy	memcpy
#define spadfs_memmove	memmove
#endif

/*
 * Distro-kernel support (RHEL 5/6/7, stable-branch backports) is dropped at
 * this floor. These stubs keep the 15 remaining call sites in super.c, file.c,
 * inode.c, name.c and spadfs.h compiling, and they fold to a constant so every
 * `&& !TEST_RHEL_VERSION(...)` / `|| TEST_RHEL_VERSION(...)` branch resolves at
 * compile time. Removing the dead conditions from those call sites is a
 * follow-on cleanup; leaving the stubs costs nothing at runtime.
 */
#define TEST_RHEL_VERSION(v1, v2)	0
#define TEST_STABLE_BRANCH(v1, v2, v3)	0

/*
 * SpadFS-private aliases. The kernel does not provide these under these names,
 * so they must survive the trim. Both are the modern-kernel variant of what used
 * to be version-selected here.
 */
typedef struct kmem_cache spadfs_cache_t;
#define spadfs_free_cache(c)	do { if (c) { kmem_cache_destroy(c); (c) = NULL; } } while (0)

/*
 * uid/gid accessors. Post-uid-namespaces form; the pre-3.5 direct-field and
 * pre-2.6.27 current->uid variants are gone with the floor.
 */
#define get_current_uid()	from_kuid(&init_user_ns, current_uid())
#define get_current_gid()	from_kgid(&init_user_ns, current_gid())
#define get_current_fsuid()	from_kuid(&init_user_ns, current_fsuid())
#define get_current_fsgid()	from_kgid(&init_user_ns, current_fsgid())

/*
 * Gates above the floor -- these still fire on the newer kernels we build
 * against, so they are kept.
 */

/* SLAB_MEM_SPREAD was removed in 6.9. Below the floor it never applied. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,9,0)
#define SLAB_MEM_SPREAD	0
#endif

/* mnt_idmap was named user_namespace before 6.3. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,3,0)
#define mnt_idmap	user_namespace
#endif

/* inode ctime accessors arrived in 6.6. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
#define inode_get_ctime(inode)	((inode)->i_ctime)
#define inode_set_ctime(inode, sec, nsec)	\
do {						\
	(inode)->i_ctime.tv_sec = (sec);		\
	(inode)->i_ctime.tv_nsec = (nsec);	\
} while (0)
#endif

/* inode atime/mtime accessors arrived in 6.7. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,7,0)
#define inode_get_atime(inode)	((inode)->i_atime)
#define inode_set_atime(inode, sec, nsec)	\
do {						\
	(inode)->i_atime.tv_sec = (sec);		\
	(inode)->i_atime.tv_nsec = (nsec);	\
} while (0)
#define inode_get_mtime(inode)	((inode)->i_mtime)
#define inode_set_mtime(inode, sec, nsec)	\
do {						\
	(inode)->i_mtime.tv_sec = (sec);		\
	(inode)->i_mtime.tv_nsec = (nsec);	\
} while (0)
#endif

/*
 * Guards that are always true at this floor -- unwrapped, body kept.
 */

/* time_t was removed from the kernel in 5.6; anything we build on has that. */
#define time_t u32

/* New workqueue interface (alloc_workqueue et al). */
#define NEW_WORKQUEUE

/* set_nlink() is native; the pre-3.2 fallback is gone. */
#define HAVE_SET_NLINK

/* huge_valid_dev() is native. */
#define huge_valid_dev(dev)	1

/*
 * Plain fallbacks, not version-gated. Modern kernels define READ_ONCE/WRITE_ONCE
 * and BIO_MAX_PAGES, so these normally expand to nothing; they are kept only so
 * the header stays usable if those ever move.
 */

#ifndef __cold
#define __cold
#endif

#ifndef READ_ONCE
#define READ_ONCE(x)	(*(volatile typeof(x) *)&(x))
#endif

#ifndef WRITE_ONCE
#define WRITE_ONCE(x, y)	(*(volatile typeof(x) *)&(x)) = (y)
#endif

#ifndef BIO_MAX_VECS
#define BIO_MAX_VECS	BIO_MAX_PAGES
#endif

#endif
