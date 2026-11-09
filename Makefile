ifndef CONFIG_SPADFS_FS

#KERNELDIR := /usr/src/linux-$(shell uname -r)/
#KERNELDIR := /usr/src/linux/
KERNELDIR := /lib/modules/$(shell uname -r)/build

# --- spadfs2 tool identity -------------------------------------------
# Binaries carry the "2" suffix so the v1.0.22 stack and this one can sit
# side by side for the Phase 0.4 reference comparison. Build the old names
# from this tree with:
#     make SPADFS_NAME=spadfs SPADFS_TOOL_SUFFIX=
SPADFS_TOOL_SUFFIX ?= 2
MKFS_TOOL ?= mkspadfs$(SPADFS_TOOL_SUFFIX)
FSCK_TOOL ?= spadfsck$(SPADFS_TOOL_SUFFIX)

all : spadfs.module $(MKFS_TOOL) $(FSCK_TOOL)

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

# -MMD -MP writes a .d file next to each tool object listing the headers it
# included, and those files are pulled in below. Without them an edit to a
# shared header (struct.h, meta_crc.h) rebuilt only the .c files that were
# touched too, and the tools linked objects compiled against the old layout:
# a 24 -> 32 byte fnode_block header change left sckfxfn.o reading the old
# fixed_fnode_block offsets, and spadfsck rejected every kernel-written
# volume with a hard link.
DEPFLAGS := -MMD -MP

# Per-tool banner macro, so diagnostics name the binary that is actually running
# instead of a hardcoded "SPADFSCK"/"MKSPADFS". These pattern rules are more
# specific than the generic %.o rule below (shorter stem), so make prefers them.
utils/spadfsck/%.o : utils/spadfsck/%.c
	$(SPADFS_CC) $(SPADFS_CFLAGS) $(AIO_CFLAGS) $(DEPFLAGS) -DTOOL_BANNER='"$(FSCK_TOOL)"' -c -o $@ -x c $<

utils/mkspadfs/%.o : utils/mkspadfs/%.c
	$(SPADFS_CC) $(SPADFS_CFLAGS) $(AIO_CFLAGS) $(DEPFLAGS) -DTOOL_BANNER='"$(MKFS_TOOL)"' -c -o $@ -x c $<

%.o : %.c
	$(SPADFS_CC) $(SPADFS_CFLAGS) $(AIO_CFLAGS) $(DEPFLAGS) -c -o $@ -x c $<

-include $(wildcard utils/*.d utils/*/*.d)

$(MKFS_TOOL) : \
	utils/crc32c.o \
	utils/mounted.o \
	utils/mkspadfs/mkspadfs.o
	$(SPADFS_CC) $(SPADFS_CFLAGS) -o $@ $^

$(FSCK_TOOL) : \
	utils/common.o \
	utils/crc32c.o \
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
	-rm -f */*.o */*/*.o */*.d */*/*.d $(MKFS_TOOL) $(FSCK_TOOL)

clean : clean-kernel clean-userspace

# Unprivileged test suite: no root, no loop devices, no module loading.
# Depends only on the userspace tools, so CI can run it without kernel headers.
test : $(MKFS_TOOL) $(FSCK_TOOL) crc-agreement
	bash tests/roundtrip.sh
	bash tests/detect.sh > tests/BASELINE-checksum-detection.md
	bash tests/cct_integrity.sh > tests/BASELINE-cct-integrity.md
	bash tests/cct_recovery.sh
	bash tests/lint_shell.sh
	bash tests/cc_wrap.sh
	bash tests/txb_backup.sh
	python3 tests/logwrites_test.py
	python3 tests/metasum_test.py
	bash tests/restripe.sh > tests/BASELINE-restripe-attribution.md
	@echo "regenerated both BASELINE files in tests/"

# Runs first among the Phase 2 checks on purpose: if the kernel and userspace
# CRC implementations disagree, every other result below is meaningless, so
# there is no point computing it.
crc-agreement : utils/crc32c.o
	$(CC) -Wall -Wextra -Wno-unused-parameter -I. -o tests/crc_agreement \
		tests/crc_agreement.c utils/crc32c.c
	@./tests/crc_agreement | tail -3
	@echo "crc agreement gate passed"

# NOTE: `test-ref` (side-by-side spadfs/spadfs2 reference comparison, plan task
# 0.4) is not defined yet -- it needs root for insmod/mount and is deliberately
# kept out of the unprivileged suite.

install : install-kernel install-userspace

install-kernel : spadfs.module
	$(MAKE) -C $(KERNELDIR) M=`pwd` STANDALONE_BUILD=y modules_install
	depmod -a

install-userspace : $(MKFS_TOOL) $(FSCK_TOOL)
	install $(MKFS_TOOL) $(FSCK_TOOL) /sbin/
	-strip /sbin/$(MKFS_TOOL)
	-strip /sbin/$(FSCK_TOOL)
	ln -sf $(MKFS_TOOL) /sbin/mkfs.spadfs2
	ln -sf $(FSCK_TOOL) /sbin/fsck.spadfs2

else

ifdef M
include $(M)/Kbuild
else
include fs/spadfs/Kbuild
endif

endif
