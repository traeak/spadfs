#!/usr/bin/env bash
# A heavily fragmented file. RUN AS ROOT.
#
# A file's extents live in a tree of anodes (struct.h): a root anode with 20
# direct extents and pointers whose index sets the depth, interior anodes,
# and leaf anodes of ANODE_N_EXTENTS extents each. Ordinary files never go
# past the root, so the tree's deeper levels go untested unless a file is
# forced into thousands of pieces. A leaf that filled one extent too far
# (find_direct, a stale literal 31) once made such files unreadable.
#
# For each block size: fill the volume with 4 KiB files, delete every other
# one, then write one big file that can only use the 4 KiB holes. Then:
#
#   layout   the file has more extents than the root, one leaf and one
#            interior level can hold (so the tree is at least 3 deep)
#   data     it reads back exactly, before and after a remount
#   fsck     spadfsck is clean with the deep tree, and again after the file
#            is deleted (every anode freed: no lost space)
#   log      the kernel logged no spadfs error
#
# Usage: sudo bash tests/fragment_mount.sh
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (losetup, mount). Run: sudo bash tests/fragment_mount.sh"

KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
MNT="$WORKDIR/mnt"
LOOP=""

cleanup() {
	set +e
	mountpoint -q "$MNT" && umount "$MNT"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
attach() { LOOP=$(losetup --find --show "$1") || die "losetup failed"; }
detach() { [ -n "$LOOP" ] && losetup -d "$LOOP"; LOOP=""; }

# more than 20 (root) + 30 (one leaf) + 29 * 30 (one interior level)
MIN_EXTENTS=1000

banner "module"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -3)"
ok "loaded $KO"

for bs in 512 4096; do
	IMG="$WORKDIR/bs$bs.img"
	mkdir -p "$MNT"
	klog_mark

	banner "block size $bs: fragment"
	make_image "$IMG" 48M --block-size "$bs" >/dev/null || { bad "bs$bs: mkfs"; continue; }
	attach "$IMG"
	mount -t spadfs2 "$LOOP" "$MNT" || { bad "bs$bs: mount" "$(klog_since | tail -3)"; detach; continue; }
	mkdir "$MNT/fill"
	# fill with 4 KiB files until the volume is full, then free every other
	python3 - "$MNT/fill" <<'EOF'
import os, sys
d = sys.argv[1]
buf = b"f" * 4096
i = 0
while True:
    try:
        with open(os.path.join(d, "%06d" % i), "wb") as f:
            f.write(buf)
    except OSError:
        break
    i += 1
for j in range(0, i, 2):
    os.unlink(os.path.join(d, "%06d" % j))
print(i)
EOF
	sync -f "$MNT"
	free_kb=$(( $(stat -f -c '%a * %S' "$MNT") / 1024 ))
	# most of the freed holes, leaving room for anodes
	size_kb=$(( free_kb * 3 / 4 ))
	head -c "$((size_kb * 1024))" /dev/urandom >"$WORKDIR/big.src"
	cp "$WORKDIR/big.src" "$MNT/big" && sync -f "$MNT" \
		|| bad "bs$bs: writing the ${size_kb} KiB file failed" "$(klog_since | grep -i spadfs | tail -3)"
	n=$(filefrag "$MNT/big" 2>/dev/null | awk '{print $2}')
	if [ "${n:-0}" -ge "$MIN_EXTENTS" ] 2>/dev/null; then
		ok "bs$bs: the file has $n extents (at least $MIN_EXTENTS: a 3-level tree)"
	else
		bad "bs$bs: only ${n:-?} extents; the test did not fragment the file enough"
	fi
	cmp -s "$WORKDIR/big.src" "$MNT/big" \
		&& ok "bs$bs: the fragmented file reads back exactly" \
		|| bad "bs$bs: the fragmented file reads back wrong" "$(klog_since | grep -i spadfs | tail -3)"
	umount "$MNT" || { bad "bs$bs: umount"; detach; continue; }
	detach

	banner "block size $bs: persist"
	"$FSCK" -n -f "$IMG" >"$WORKDIR/fsck.txt" 2>&1 \
		&& ok "bs$bs: spadfsck is clean with the deep extent tree" \
		|| bad "bs$bs: spadfsck rejects the volume" "$(tr '\r' '\n' <"$WORKDIR/fsck.txt" | grep -E 'ANODE|EXTENT|LOST|ERROR' | head -3)"
	attach "$IMG"
	mount -t spadfs2 "$LOOP" "$MNT" || { bad "bs$bs: remount" "$(klog_since | tail -3)"; detach; continue; }
	echo 3 >/proc/sys/vm/drop_caches
	cmp -s "$WORKDIR/big.src" "$MNT/big" \
		&& ok "bs$bs: it still reads back exactly after a remount" \
		|| bad "bs$bs: it reads back wrong after a remount" "$(klog_since | grep -i spadfs | tail -3)"

	banner "block size $bs: delete"
	rm "$MNT/big"
	umount "$MNT"
	detach
	"$FSCK" -n -f "$IMG" >"$WORKDIR/fsck.txt" 2>&1 \
		&& ok "bs$bs: spadfsck is clean after the delete (every anode freed)" \
		|| bad "bs$bs: spadfsck after the delete" "$(tr '\r' '\n' <"$WORKDIR/fsck.txt" | grep -E 'ANODE|LOST|ERROR' | head -3)"
	all=$(klog_since) || die "kernel log marker lost; rerun"
	log=$(grep -iE 'spadfs.*(error|anode|valid extents)' <<<"$all")
	[ -z "$log" ] \
		&& ok "bs$bs: no spadfs errors in the kernel log" \
		|| bad "bs$bs: spadfs logged errors" "$(head -3 <<<"$log")"
	rm -f "$IMG" "$WORKDIR/big.src"
done

summary
