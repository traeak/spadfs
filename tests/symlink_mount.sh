#!/usr/bin/env bash
# Long symlinks. RUN AS ROOT.
#
# A symlink target up to 164 characters fits in the fnode's EA pool (a short
# symlink). A longer one is a long symlink: laid out like a regular file --
# UNX attribute of mode S_IFLNK plus a zero-length SYM attribute as the
# marker -- with the target as its data (struct.h, EA_SYMLINK_MAGIC). For
# each block size:
#
#   create   targets of 1, 164 (the last short length), 165 (the first long
#            one), 1000 and 4095 characters (the longest: PATH_MAX - 1) all
#            read back exactly; 4096 characters is refused
#   persist  after unmount, spadfsck is clean (it checks the marker and the
#            symlink mode pair, and the long symlinks' data as file data),
#            and after a remount every target still reads back exactly
#   types    stat reports symbolic links of the target's length; following
#            one reaches the target
#   delete   removing the long symlinks frees their data: spadfsck is clean
#            with no lost space, and the free-space count returns
#
# Usage: sudo bash tests/symlink_mount.sh
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail
# Output is captured first and grepped from a here-string (see cc_wrap.sh).

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (losetup, mount). Run: sudo bash tests/symlink_mount.sh"

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

# a target of exactly $1 characters: path components of 'p's, '/'-separated
target() {
	python3 -c 'import sys
n = int(sys.argv[1])
s = "/".join("p" * 200 for _ in range(n // 201 + 1))
print(s[:n].rstrip("/") + "x" * (n - len(s[:n].rstrip("/"))), end="")' "$1"
}

LENGTHS="1 164 165 1000 4095"

banner "module"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -3)"
ok "loaded $KO"

for bs in 512 4096; do
	IMG="$WORKDIR/bs$bs.img"
	mkdir -p "$MNT"

	banner "block size $bs: create"
	make_image "$IMG" 64M --block-size "$bs" >/dev/null || { bad "bs$bs: mkfs"; continue; }
	attach "$IMG"
	mount -t spadfs2 "$LOOP" "$MNT" || { bad "bs$bs: mount" "$(dmesg | tail -3)"; detach; continue; }
	mkdir "$MNT/t"
	echo hello >"$MNT/t/file"
	for n in $LENGTHS; do
		t=$(target "$n")
		[ "${#t}" = "$n" ] || die "target() made ${#t} characters, not $n"
		if ln -s "$t" "$MNT/l$n" 2>/dev/null && [ "$(readlink "$MNT/l$n")" = "$t" ]; then
			ok "bs$bs: a $n-character target reads back exactly"
		else
			got=$(readlink "$MNT/l$n" 2>/dev/null)
			bad "bs$bs: $n-character symlink" "readlink gave ${#got} characters, want $n; size $(stat -c %s "$MNT/l$n" 2>/dev/null)"
		fi
	done
	t=$(target 4096)
	if ln -s "$t" "$MNT/l4096" 2>/dev/null; then
		bad "bs$bs: a 4096-character target was accepted"
	else
		ok "bs$bs: a 4096-character target is refused"
	fi
	# a long target that resolves: 100 "./" then t/file
	ln -s "$(printf './%.0s' $(seq 1 100))t/file" "$MNT/follow"
	[ "$(cat "$MNT/follow" 2>/dev/null)" = hello ] \
		&& ok "bs$bs: following a long symlink reaches its target" \
		|| bad "bs$bs: following a long symlink failed"
	[ "$(stat -c %F:%s "$MNT/l1000")" = "symbolic link:1000" ] \
		&& ok "bs$bs: stat reports a symbolic link of the target's length" \
		|| bad "bs$bs: stat of the long symlink" "$(stat -c %F:%s "$MNT/l1000")"
	sync -f "$MNT"
	umount "$MNT" || { bad "bs$bs: umount"; detach; continue; }
	detach

	banner "block size $bs: persist"
	"$FSCK" -n -f "$IMG" >"$WORKDIR/fsck.txt" 2>&1 \
		&& ok "bs$bs: spadfsck is clean with short and long symlinks" \
		|| bad "bs$bs: spadfsck rejects the volume" "$(tr '\r' '\n' <"$WORKDIR/fsck.txt" | grep -E 'SYMLINK|ATTRIBUTE|LOST|ERROR' | head -3)"
	attach "$IMG"
	mount -t spadfs2 "$LOOP" "$MNT" || { bad "bs$bs: remount" "$(dmesg | tail -3)"; detach; continue; }
	all=1
	for n in $LENGTHS; do
		[ "$(readlink "$MNT/l$n")" = "$(target "$n")" ] || { all=0; bad "bs$bs: $n-character target changed after remount"; }
	done
	[ "$all" = 1 ] && ok "bs$bs: every target reads back exactly after a remount"
	free_before=$(stat -f -c %f "$MNT")

	banner "block size $bs: delete"
	rm "$MNT/l165" "$MNT/l1000" "$MNT/l4095"
	sync -f "$MNT"
	free_after=$(stat -f -c %f "$MNT")
	[ "$free_after" -gt "$free_before" ] \
		&& ok "bs$bs: deleting the long symlinks frees their blocks ($free_before -> $free_after)" \
		|| bad "bs$bs: no space freed by deleting long symlinks ($free_before -> $free_after)"
	umount "$MNT"
	detach
	"$FSCK" -n -f "$IMG" >"$WORKDIR/fsck.txt" 2>&1 \
		&& ok "bs$bs: spadfsck is clean after the deletes (no lost space)" \
		|| bad "bs$bs: spadfsck after the deletes" "$(tr '\r' '\n' <"$WORKDIR/fsck.txt" | grep -E 'LOST|ERROR|SYMLINK' | head -3)"
done

summary
