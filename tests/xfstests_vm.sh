#!/usr/bin/env bash
# Run xfstests against spadfs2 in a throwaway VM. NO ROOT NEEDED on the host:
# only QEMU, membership of the kvm group, and a built xfstests tree.
#
# The guest boots the host's own kernel (/boot/vmlinuz-K, modules from
# /lib/modules/K) from a small busybox initramfs, mounts the host's root
# filesystem read-only over 9p with a tmpfs overlay on top -- so it has every
# host tool, and anything it writes vanishes at poweroff -- and runs as root
# there. WORK is shared read-write (results come back through it). Two sparse
# images are the test and scratch devices (/dev/vda, /dev/vdb).
#
# xfstests has no spadfs2 support; its fallback for an unknown FSTYP runs
# "mkfs -t spadfs2" and "fsck -t spadfs2", so the guest gets mkfs.spadfs2
# and fsck.spadfs2 wrappers around this tree's mkspadfs2 and spadfsck2.
#
# Usage: bash tests/xfstests_vm.sh XFSTESTS_DIR [check arguments...]
#   e.g. bash tests/xfstests_vm.sh ~/src/xfstests -g quick
#        bash tests/xfstests_vm.sh ~/src/xfstests generic/001 generic/002
# XFSTESTS_DIR must be built (make); the guest sees it read-only. Results land
# in WORK/results, the check log in WORK/check.log.
#
#        bash tests/xfstests_vm.sh --script SCRIPT [arguments...]
# runs one of this tree's root tests (e.g. tests/mount_smoke.sh) as root in
# the guest instead, its output in WORK/check.log -- no sudo on the host.
#
# Either way the guest's kernel log is saved to WORK/dmesg.log at the end.
#
# Environment:
#   KVER       guest kernel version (default: uname -r; spadfs2.ko must be
#              built for it)
#   WORK       working directory (default /var/tmp/spadfs-xfstests)
#   MEM, CPUS  guest size (default 4G, 4)
#   TEST_SIZE, SCRATCH_SIZE  device sizes (default 4G, 8G; sparse)
#   MKFS_OPTIONS  extra mkspadfs2 options, e.g. "--block-size 4096"
#   TIMEOUT    seconds before the guest is killed (default 36000)
#   LOGWRITES_SIZE  dm-log-writes device for the tests that need one
#              (default 8G; sparse)
#   EXTRA_PKGS Void packages unpacked into the guest only, for tools the
#              host lacks (default "attr-progs acl-progs quota"; skipped
#              without xbps-query/xbps-fetch). Cached in WORK/pkgs.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "xfstests_vm: $*" >&2; exit 1; }

[ $# -ge 1 ] || die "usage: $0 XFSTESTS_DIR [check arguments...] | --script SCRIPT [arguments...]"
SCRIPT=""
XFSTESTS=""
if [ "$1" = "--script" ]; then
	[ $# -ge 2 ] || die "--script needs a script"
	SCRIPT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
	[ -r "$SCRIPT" ] || die "cannot read $2"
	shift 2
else
	XFSTESTS="$(cd "$1" && pwd)" || die "no such directory: $1"
	shift
	[ $# -ge 1 ] || set -- -g quick
fi
KVER=${KVER:-$(uname -r)}
WORK=${WORK:-/var/tmp/spadfs-xfstests}
MEM=${MEM:-4G}
CPUS=${CPUS:-4}
TEST_SIZE=${TEST_SIZE:-4G}
SCRATCH_SIZE=${SCRATCH_SIZE:-8G}
LOGWRITES_SIZE=${LOGWRITES_SIZE:-8G}
TIMEOUT=${TIMEOUT:-36000}
EXTRA_PKGS=${EXTRA_PKGS-attr-progs acl-progs quota}
KERNEL=/boot/vmlinuz-$KVER
MODDIR=/lib/modules/$KVER

[ -n "$SCRIPT" ] || { [ -x "$XFSTESTS/check" ] && [ -x "$XFSTESTS/ltp/fsstress" ]; } \
	|| die "$XFSTESTS is not a built xfstests tree (run make there)"
[ -r "$KERNEL" ] || die "cannot read $KERNEL"
[ -w /dev/kvm ] || die "/dev/kvm is not writable (join the kvm group)"
for f in spadfs2.ko mkspadfs2 spadfsck2; do
	[ -e "$REPO/$f" ] || die "$REPO/$f is missing; build the tree first"
done
vermagic=$(modinfo -F vermagic "$REPO/spadfs2.ko")
[ "${vermagic%% *}" = "$KVER" ] || die "spadfs2.ko is built for ${vermagic%% *}, not $KVER"
for t in qemu-system-x86_64 busybox cpio zstd; do
	command -v "$t" >/dev/null || die "$t not found"
done

mkdir -p "$WORK"

# --- tools the host lacks (getfattr, chacl, setquota, ...): unpacked into
# WORK/extra-root, which the guest copies over its overlay. The host is
# untouched. ---
rm -rf "$WORK/extra-root"
mkdir -p "$WORK/extra-root" "$WORK/pkgs"
if [ -n "$EXTRA_PKGS" ] && command -v xbps-query >/dev/null && command -v xbps-fetch >/dev/null; then
	repo=$(xbps-query -L 2>/dev/null | awk '$2 ~ /^https?:/ { print $2; exit }')
	arch=$(xbps-uhelper arch 2>/dev/null || uname -m)
	for p in $EXTRA_PKGS; do
		v=$(xbps-query -R -p pkgver "$p" 2>/dev/null) || { echo "xfstests_vm: no package $p, skipped" >&2; continue; }
		f="$WORK/pkgs/$v.$arch.xbps"
		[ -s "$f" ] || xbps-fetch -o "$f" "$repo/$v.$arch.xbps" >/dev/null 2>&1 \
			|| { echo "xfstests_vm: could not fetch $v, skipped" >&2; rm -f "$f"; continue; }
		tar --zstd -xf "$f" -C "$WORK/extra-root" ./usr 2>/dev/null \
			|| echo "xfstests_vm: could not unpack $v" >&2
	done
fi

# --- xfstests knows nothing of spadfs2: these helpers skip unknown
# filesystems. Patched into the guest's copy of common/rc only. ---
cat >"$WORK/patch_rc.py" <<'P'
import sys
p = sys.argv[1]
s = open(p).read()
def insert(anchor, text):
    # put text before anchor, which must occur exactly once
    global s
    if s.count(anchor) != 1:
        sys.exit("patch_rc: anchor not found once: " + anchor.strip()[:60])
    s = s.replace(anchor, text + anchor)
# mkspadfs DEVICE SIZE (bytes); block size by option
insert('\t*)\n\t\t_notrun "Filesystem $FSTYP not supported in _scratch_mkfs_sized"',
       '\tspadfs2)\n\t\t${MKFS_PROG} -t $FSTYP -- $MKFS_OPTIONS --block-size $blocksize "$@" $SCRATCH_DEV $fssize\n\t\t;;\n')
insert('\t*)\n\t\t_notrun "Filesystem $FSTYP not supported in _scratch_mkfs_blocksized"',
       '\tspadfs2)\n\t\t${MKFS_PROG} -t $FSTYP -- $MKFS_OPTIONS --block-size $blocksize $SCRATCH_DEV\n\t\t;;\n')
# fnode times are unsigned 32-bit seconds (s_time_min/s_time_max)
insert('\text2)\n\t\techo "$s32min $s32max"',
       '\tspadfs2)\n\t\techo "0 $u32max"\n\t\t;;\n')
open(p, "w").write(s)
P

rm -rf "$WORK/initramfs"
mkdir -p "$WORK/initramfs"/{bin,usr/lib,proc,sys,dev,host,rw,new,modules}

# --- initramfs: busybox, its libraries, and the modules to reach 9p and
# overlayfs (decompressed: busybox insmod cannot read .zst) ---
IR="$WORK/initramfs"
cp /usr/bin/busybox "$IR/bin/"
# links made here: not every busybox build has --install
for a in $(busybox --list); do
	[ -e "$IR/bin/$a" ] || ln -s busybox "$IR/bin/$a"
done
# every library in usr/lib, under each directory the loader may search
ln -s usr/lib "$IR/lib"
ln -s usr/lib "$IR/lib64"
ln -s lib "$IR/usr/lib64"
ldd /usr/bin/busybox | grep -o '/[^ ]*' | while read -r lib; do
	cp -L "$lib" "$IR/usr/lib/$(basename "$lib")"
done
mods=$(for m in virtio_pci virtio_blk 9pnet_virtio 9p overlay; do
	modprobe -S "$KVER" --show-depends "$m" | awk '$1 == "insmod" { print $2 }'
done | awk '!seen[$0]++')
: >"$IR/modules/order"
for m in $mods; do
	b=$(basename "$m"); b=${b%.zst}; b=${b%.xz}; b=${b%.gz}
	case "$m" in
	*.zst) zstd -qdc "$m" >"$IR/modules/$b" ;;
	*.xz) xz -dc "$m" >"$IR/modules/$b" ;;
	*.gz) gzip -dc "$m" >"$IR/modules/$b" ;;
	*) cp "$m" "$IR/modules/$b" ;;
	esac
	echo "$b" >>"$IR/modules/order"
done

cat >"$IR/init" <<'EOF'
#!/bin/busybox sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
while read -r m; do insmod /modules/$m; done </modules/order
mount -t 9p -o trans=virtio,version=9p2000.L,ro,msize=1048576 hostroot /host
mount -t tmpfs -o size=75% tmpfs /rw
mkdir -p /rw/upper /rw/work
mount -t overlay overlay \
	-o lowerdir=/host,upperdir=/rw/upper,workdir=/rw/work /new \
	|| { echo "overlay over 9p failed"; dmesg | tail; echo o >/proc/sysrq-trigger; }
for d in proc sys dev; do mount --move /$d /new/$d; done
mkdir -p /new/xvm
mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576 work /new/xvm
exec chroot /new /bin/sh /xvm/guest.sh
EOF
chmod +x "$IR/init"
(cd "$IR" && find . | cpio -o -H newc --quiet | gzip -1) >"$WORK/initramfs.img"

# --- the guest's script: runs as root in the chrooted overlay ---
cat >"$WORK/guest.sh" <<EOF
#!/bin/sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# /tmp stays the overlay's (writable already): a tmpfs there would hide
# an xfstests tree or this tree under the host's /tmp
mount -t tmpfs tmpfs /run
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /dev/shm
# what init and udev provide on a real system: /dev/fd (bash process
# substitution) and the /dev/mapper names of device-mapper targets
ln -sf /proc/self/fd /dev/fd
ln -sf /proc/self/fd/0 /dev/stdin
ln -sf /proc/self/fd/1 /dev/stdout
ln -sf /proc/self/fd/2 /dev/stderr
udevd --daemon && udevadm trigger && udevadm settle
hostname spadfs-xfstests
# loopback: locktest (generic/131) runs its client and server over TCP
ip link set lo up 2>/dev/null || ifconfig lo up
# xfstests calls "hostname -s", which not every hostname(1) accepts
cat >/usr/local/bin/hostname <<'W'
#!/bin/sh
case "\$1" in -s) uname -n | cut -d. -f1 ;; *) uname -n ;; esac
W
chmod +x /usr/local/bin/hostname
exec >/xvm/guest.log 2>&1
set -x
# no poweroff in a chroot without an init system: sysrq powers off
off() { sync; echo o >/proc/sysrq-trigger; sleep 30; }
insmod "$REPO/spadfs2.ko" || { echo "insmod spadfs2.ko failed"; off; }
for m in loop dm_mod dm_flakey dm_log_writes dm_snapshot dm_thin_pool scsi_debug; do
	modprobe \$m
done
cat >/usr/bin/mkfs.spadfs2 <<'W'
#!/bin/sh
# xfstests: mkfs -t spadfs2 -- [options] DEV
[ "\$1" = "--" ] && shift
# mkspadfs prints its progress on stderr; tests keep stderr in their output
# (only mkfs errors belong there), so move it to stdout like mkfs.ext4's
exec "$REPO/mkspadfs2" --no-trim \$MKFS_EXTRA "\$@" 2>&1
W
cat >/usr/bin/fsck.spadfs2 <<'W'
#!/bin/sh
exec "$REPO/spadfsck2" "\$@"
W
chmod +x /usr/bin/mkfs.spadfs2 /usr/bin/fsck.spadfs2
export MKFS_EXTRA="${MKFS_OPTIONS:-}"
# the users and groups some tests require (_require_user, _require_group)
# (shadow entries too: su runs PAM's account check, which needs one. The
# host's /etc/shadow is unreadable to the user QEMU runs as, so the guest
# gets its own, holding only root and these users.)
rm -f /etc/shadow /etc/gshadow
echo 'root:!:20000::::::' >/etc/shadow
chmod 600 /etc/shadow
grep -q '^fsgqa:' /etc/passwd || {
	for u in fsgqa:1500:1500 123456-fsgqa:1501:1500 fsgqa2:1502:1502; do
		IFS=: read -r n uid gid <<U
\$u
U
		echo "\$n:x:\$uid:\$gid::/tmp:/bin/sh" >>/etc/passwd
		echo "\$n:!:20000::::::" >>/etc/shadow
	done
	echo 'fsgqa:x:1500:' >>/etc/group
	echo 'fsgqa2:x:1502:' >>/etc/group
}
# tools unpacked on the host (EXTRA_PKGS)
cp -a /xvm/extra-root/. / 2>/dev/null
if [ -n "$SCRIPT" ]; then
	set +x
	cd "$REPO"
	bash "$SCRIPT" $* >/xvm/check.log 2>&1
	echo "script exited \$?" >>/xvm/check.log
	dmesg >/xvm/dmesg.log
	off
fi
mkdir -p /mnt/test /mnt/scratch
cat >"$XFSTESTS/local.config" <<C
export FSTYP=spadfs2
export TEST_DEV=/dev/vda
export TEST_DIR=/mnt/test
export SCRATCH_DEV=/dev/vdb
export SCRATCH_MNT=/mnt/scratch
export RESULT_BASE=/xvm/results
export LOGWRITES_DEV=/dev/vdc
C
python3 /xvm/patch_rc.py "$XFSTESTS/common/rc" || { echo "patching common/rc failed"; off; }
mkfs.spadfs2 -- /dev/vda
cd "$XFSTESTS"
set +x
./check -E "$REPO/tests/xfstests.exclude" $* >/xvm/check.log 2>&1
echo "check exited \$?" >>/xvm/check.log
# the test device lives across tests: report how full it ended and whether
# it is consistent (a space leak shows as falling free space here)
{
	umount /mnt/test 2>/dev/null
	echo "== test device at the end"
	mount -t spadfs2 /dev/vda /mnt/test && df -k /mnt/test && umount /mnt/test
	"$REPO/spadfsck2" -n -f /dev/vda; echo "spadfsck exited \$?"
} >>/xvm/check.log 2>&1
dmesg >/xvm/dmesg.log
off
EOF

truncate -s "$TEST_SIZE" "$WORK/test.img"
truncate -s "$SCRATCH_SIZE" "$WORK/scratch.img"
rm -f "$WORK/logwrites.img"
truncate -s "$LOGWRITES_SIZE" "$WORK/logwrites.img"
rm -f "$WORK/check.log" "$WORK/guest.log" "$WORK/dmesg.log"

if [ -n "$SCRIPT" ]; then
	echo "xfstests_vm: kernel $KVER, script $SCRIPT $*"
else
	echo "xfstests_vm: kernel $KVER, xfstests $XFSTESTS, check $*"
fi
echo "xfstests_vm: console $WORK/console.log, progress $WORK/check.log"
timeout "$TIMEOUT" qemu-system-x86_64 \
	-enable-kvm -cpu host -m "$MEM" -smp "$CPUS" \
	-nographic -no-reboot \
	-kernel "$KERNEL" -initrd "$WORK/initramfs.img" \
	-append "console=ttyS0 panic=-1 oops=panic loglevel=4" \
	-virtfs local,path=/,mount_tag=hostroot,security_model=none,readonly=on,multidevs=remap \
	-virtfs local,path="$WORK",mount_tag=work,security_model=mapped-xattr \
	-drive file="$WORK/test.img",if=virtio,format=raw,cache=unsafe \
	-drive file="$WORK/scratch.img",if=virtio,format=raw,cache=unsafe \
	-drive file="$WORK/logwrites.img",if=virtio,format=raw,cache=unsafe \
	>"$WORK/console.log" 2>&1 || echo "xfstests_vm: qemu exited $?"

[ -e "$WORK/check.log" ] || { tail -30 "$WORK/console.log"; die "the guest never ran the tests"; }
tail -5 "$WORK/check.log"
# the script's own verdict, for callers
! grep -qE '^(script|check) exited [1-9]' "$WORK/check.log"
