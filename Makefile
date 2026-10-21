ifndef CONFIG_SPADFS_FS

#KERNELDIR := /usr/src/linux-$(shell uname -r)/
#KERNELDIR := /usr/src/linux/
KERNELDIR := /lib/modules/$(shell uname -r)/build

all : spadfs.module mkspadfs spadfsck

spadfs.module :
	$(MAKE) -C $(KERNELDIR) M=`pwd` STANDALONE_BUILD=y CONFIG_SPADFS_FS=m

SPADFS_CC := gcc
SPADFS_CFLAGS := -Wall -W -Wno-unused-parameter -Wno-sign-compare -Wno-array-bounds -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -O2 -fno-strict-aliasing -g -fomit-frame-pointer -shared-libgcc $(CFLAGS)
#SPADFS_CC := icc
#SPADFS_CFLAGS := -no-ansi-alias -ipo -static-intel $(CFLAGS)

ifneq (,$(findstring linux,$(AIO)))
AIO_CFLAGS += -DLINUX_AIO
AIO_LDFLAGS += -laio
endif
ifneq (,$(findstring posix,$(AIO)))
AIO_CFLAGS += -DPOSIX_AIO
AIO_LDFLAGS += -lrt
endif
ifneq (,$(findstring direct,$(AIO)))
AIO_CFLAGS += -DDEFAULT_DIRECT
endif

%.o : %.c
	$(SPADFS_CC) $(SPADFS_CFLAGS) $(AIO_CFLAGS) -c -o $@ -x c $<

mkspadfs : \
	utils/mounted.o \
	utils/mkspadfs/mkspadfs.o
	$(SPADFS_CC) $(SPADFS_CFLAGS) -o $@ $^

spadfsck : \
	utils/common.o \
	utils/mounted.o \
	utils/spadfsck/sck.o \
	utils/spadfsck/sckalloc.o \
	utils/spadfsck/sckapage.o \
	utils/spadfsck/sckbuf.o \
	utils/spadfsck/sckcct.o \
	utils/spadfsck/sckcrt.o \
	utils/spadfsck/sckdir.o \
	utils/spadfsck/sckea.o \
	utils/spadfsck/sckfblk.o \
	utils/spadfsck/sckfile.o \
	utils/spadfsck/sckfn.o \
	utils/spadfsck/sckfxfn.o \
	utils/spadfsck/sckhdlnk.o \
	utils/spadfsck/sckino.o \
	utils/spadfsck/scklog.o \
	utils/spadfsck/sckmass.o \
	utils/spadfsck/sckrcv.o \
	utils/spadfsck/scksuper.o \
	utils/spadfsck/sckutil.o \
	utils/spadfsck/sckxl.o
	$(SPADFS_CC) $(SPADFS_CFLAGS) -o $@ $^ $(AIO_LDFLAGS)

clean-kernel:
	-$(MAKE) -C $(KERNELDIR) M=$$PWD clean
	-rm -f *.ko *.o Module.markers Module.symvers modules.builtin modules.order .cache.mk .*.d .*.tmp .*.cmd *.mod.c
	-rm -rf .tmp_versions/

clean-userspace:
	-rm -f */*.o */*/*.o mkspadfs spadfsck

clean : clean-kernel clean-userspace

install : install-kernel install-userspace

install-kernel : spadfs.module
	$(MAKE) -C $(KERNELDIR) M=`pwd` STANDALONE_BUILD=y modules_install
	depmod -a

install-userspace : mkspadfs spadfsck
	install mkspadfs spadfsck /sbin/
	-strip /sbin/mkspadfs
	-strip /sbin/spadfsck
	ln -sf mkspadfs /sbin/mkfs.spadfs
	ln -sf spadfsck /sbin/fsck.spadfs

else

ifdef M
include $(M)/Kbuild
else
include fs/spadfs/Kbuild
endif

endif
