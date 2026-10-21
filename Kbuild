ifdef STANDALONE_BUILD
CONFIG_SPADFS_FS = m
endif

ifndef CONFIG_SPADFS_FS
CONFIG_SPADFS_FS = m
endif

ifndef SPADFS_NAME
SPADFS_NAME = spadfs
endif

CFLAGS_super.o := -DSPADFS_NAME=$(SPADFS_NAME)

obj-$(CONFIG_SPADFS_FS) += $(SPADFS_NAME).o
$(SPADFS_NAME)-y := alloc.o allocmem.o buffer.o bufstruc.o dir.o file.o inode.o ioctl.o link.o name.o namei.o super.o xattr.o
