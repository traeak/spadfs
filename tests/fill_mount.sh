#!/usr/bin/env bash
# Filling a volume. RUN AS ROOT.
#
# Creating 4 KiB files until the volume is full keeps a small-file
# preallocation outstanding at the moment allocation fails. spadfs_alloc_blocks
# then frees that preallocation and retries -- and used to retry with the
# request its failed attempt had halved, so a directory page got 8 sectors
# where it asked for 16 and used all 16: the rest overlapped a spare apage and
# later file data (dnodes pointing at file contents, lost sectors, "free ...
# does overlap with block" in the kernel log). About 1 fill in 25 hit it; the
# bug is in v1.0.22 too, where random names expose it.
#
# For each block size, ROUNDS times: make a 48 MiB volume, create 4 KiB files
# until ENOSPC, unmount, and require spadfsck to be clean. At the end the
# kernel log must hold no spadfs error.
#
# Usage: sudo bash tests/fill_mount.sh        ROUNDS=40 by default
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (losetup, mount). Run: sudo bash tests/fill_mount.sh"

KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
ROUNDS=${ROUNDS:-40}
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

banner "module"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -3)"
ok "loaded $KO"
klog_mark

IMG="$WORKDIR/fill.img"
mkdir -p "$MNT"
for bs in 512 4096; do
	banner "block size $bs: $ROUNDS fills"
	bad_rounds=0
	first=""
	for round in $(seq 1 "$ROUNDS"); do
		rm -f "$IMG"
		make_image "$IMG" 48M --block-size "$bs" >/dev/null || die "mkfs failed"
		attach "$IMG"
		mount -t spadfs2 "$LOOP" "$MNT" || die "mount failed: $(klog_since | tail -3)"
		mkdir "$MNT/fill"
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
EOF
		umount "$MNT" || die "umount failed"
		detach
		if ! "$FSCK" -n -f "$IMG" >"$WORKDIR/fsck.txt" 2>&1; then
			bad_rounds=$((bad_rounds + 1))
			[ -n "$first" ] || first="round $round: $(tr '\r' '\n' <"$WORKDIR/fsck.txt" | grep -E 'LOST|FLAG|BAD|INVALID|DOES' | head -2 | tr '\n' ' ')"
		fi
	done
	[ "$bad_rounds" -eq 0 ] \
		&& ok "bs$bs: spadfsck is clean after all $ROUNDS fills" \
		|| bad "bs$bs: $bad_rounds of $ROUNDS filled volumes are damaged" "$first"
done

all=$(klog_since) || die "kernel log marker lost; rerun"
log=$(grep -E 'spadfs: .*(overlap|bad magic|error)' <<<"$all")
[ -z "$log" ] \
	&& ok "no spadfs errors in the kernel log" \
	|| bad "spadfs logged errors" "$(head -3 <<<"$log")"

summary
