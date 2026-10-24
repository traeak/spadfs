ifdef STANDALONE_BUILD
CONFIG_SPADFS_FS = m
endif

ifndef CONFIG_SPADFS_FS
CONFIG_SPADFS_FS = m
endif

# spadfs2 is the default identity on the modernization branch. Override with
# `make SPADFS_NAME=spadfs` to build the v1.0.22-compatible module from the
# same tree (used by the Phase 0.4 reference rig for side-by-side comparison).
#
# The two modules coexist: distinct fs type aliases (fs-spadfs2 vs fs-spadfs)
# and disjoint slab cache names, and neither can mount the other's volumes
# because the superblock signature differs.
ifndef SPADFS_NAME
SPADFS_NAME = spadfs2
endif

CFLAGS_super.o := -DSPADFS_NAME=$(SPADFS_NAME)

obj-$(CONFIG_SPADFS_FS) += $(SPADFS_NAME).o
$(SPADFS_NAME)-y := alloc.o allocmem.o buffer.o bufstruc.o dir.o file.o inode.o ioctl.o link.o name.o namei.o super.o xattr.o
