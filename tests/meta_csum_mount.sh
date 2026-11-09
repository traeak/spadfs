#!/usr/bin/env bash
# The kernel seals and verifies apage and dnode checksums. RUN AS ROOT.
#
# Until this change the kernel cleared the checksum flag of any apage half or
# dnode version it modified and never checked one, so the only covered
# structures were those mkfs or fsck had last written. For each block size:
#
#   seal     write enough to split a directory into dnodes and to change the
#            free-space map, across two commits (so dnodes take both the
#            seal-on-create and the copy-and-reseal path), unmount, then scan
#            the image (tests/metasum.py): every active apage half and every
#            dnode must be sealed and match, and spadfsck must agree (clean)
#   dnode    damage one byte of a sealed dnode's active entries on disk;
#            listing that directory must fail, the kernel must log a dnode
#            checksum error, and spadfsck must report it too
#   apage    damage one byte of a sealed apage's active half on disk; the
#            mount (which reads every apage) must log an apage checksum
#            error and fail, and spadfsck must report it too
#
# Positive control: a module from before this change (HEAD~1) must fail
# "seal" -- its dnodes and modified apages come out unsealed:
#   git worktree add /tmp/spadfs-old HEAD~1
#   make -C /lib/modules/$(uname -r)/build M=/tmp/spadfs-old \
#        STANDALONE_BUILD=y CONFIG_SPADFS_FS=m SPADFS_NAME=spadfs2
#   sudo SPADFS_KO=/tmp/spadfs-old/spadfs2.ko bash tests/meta_csum_mount.sh
#
# Usage: sudo bash tests/meta_csum_mount.sh
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail
# Under pipefail, `cmd | grep -q x` returns cmd's status: spadfsck exits 4
# when it finds damage, so a matching grep still reads as failure. Capture
# output first, then grep it.

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (losetup, mount). Run: sudo bash tests/meta_csum_mount.sh"

KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
MNT="$WORKDIR/mnt"
LOOP=""
MARK=0

cleanup() {
	set +e
	mountpoint -q "$MNT" && umount "$MNT"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

attach() { LOOP=$(losetup --find --show "$1"); }
detach() { [ -n "$LOOP" ] && losetup -d "$LOOP"; LOOP=""; }

# kernel log since the last kmsg_mark
kmsg_mark() { MARK=$((MARK + 1)); echo "spadfs-meta-csum-mark-$$-$MARK" > /dev/kmsg; }
kmsg_since() { dmesg | sed -n "/spadfs-meta-csum-mark-$$-$MARK\$/,\$p"; }

metasum() { python3 "$TESTS_DIR/metasum.py" "$@"; }
field() { metasum "$1" | sed -n "s/^$2=//p"; }

# flip one bit at a byte offset of an image file
flip() {
	python3 - "$1" "$2" <<'PY'
import sys
path, off = sys.argv[1], int(sys.argv[2])
with open(path, "r+b") as f:
    f.seek(off)
    b = f.read(1)[0]
    f.seek(off)
    f.write(bytes([b ^ 0x10]))
PY
}

# byte offset of the first sealed (good) structure of a kind, inside its
# checksummed range: dnode -> active entry array, apage -> active half
target_offset() {
	python3 -c '
import sys
sys.path.insert(0, sys.argv[3])
import metasum
img, kind = sys.argv[1], sys.argv[2]
vol = metasum.Volume(img)
if kind == "apage":
    for i, sec, half, which in vol.apages():
        if half is not None and half[8] & metasum.APAGE_CHECKSUM_VALID:
            print(sec * 512 + which * (vol.page // 2) + 100)
            sys.exit(0)
else:
    ds = vol.dnode_data_size()
    for sec, version in vol.dnodes():
        if vol.img[sec * 512 + 4 + version] & metasum.DNODE_CHECKSUM_VALID:
            print(sec * 512 + metasum.DNODE_ENTRY_OFFSET + version * ds + 7)
            sys.exit(0)
sys.exit(1)
' "$1" "$2" "$TESTS_DIR"
}

banner "module"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -3)"
ok "loaded $KO"

for bs in 512 4096; do
	IMG="$WORKDIR/bs$bs.img"
	mkdir -p "$MNT"

	banner "block size $bs: seal"
	make_image "$IMG" 64M --block-size "$bs" >/dev/null || { bad "bs$bs: mkfs"; continue; }
	attach "$IMG" || { bad "bs$bs: losetup"; continue; }
	if ! mount -t spadfs2 -o noatime "$LOOP" "$MNT"; then
		bad "bs$bs: mount" "$(dmesg | tail -3)"
		detach
		continue
	fi
	# commit 1: a directory big enough to need dnodes, plus file data
	mkdir "$MNT/d"
	for i in $(seq 0 2999); do printf 'x%d\n' "$i" > "$MNT/d/name-$i"; done
	dd if=/dev/urandom of="$MNT/blob" bs=1M count=4 status=none
	sync -f "$MNT"
	# commit 2: change the same dnodes and apages again (copy-and-reseal)
	for i in $(seq 0 999); do rm "$MNT/d/name-$i"; done
	for i in $(seq 3000 3999); do printf 'y%d\n' "$i" > "$MNT/d/name-$i"; done
	rm "$MNT/blob"
	sync -f "$MNT"
	umount "$MNT" || { bad "bs$bs: umount"; detach; continue; }
	detach

	metasum "$IMG" | sed 's/^/    /'
	[ "$(field "$IMG" dnode_total)" -gt 0 ] \
		&& ok "bs$bs: the workload produced dnodes" \
		|| bad "bs$bs: no dnodes found; the directory never split, so nothing below tests dnodes"
	[ "$(field "$IMG" apage_bad)" = 0 ] && [ "$(field "$IMG" apage_unsealed)" = 0 ] \
		&& ok "bs$bs: every active apage half is sealed and matches" \
		|| bad "bs$bs: apage halves unsealed or mismatched" "$(metasum "$IMG" | grep apage_)"
	[ "$(field "$IMG" dnode_bad)" = 0 ] && [ "$(field "$IMG" dnode_unsealed)" = 0 ] \
		&& ok "bs$bs: every dnode is sealed and matches" \
		|| bad "bs$bs: dnodes unsealed or mismatched" "$(metasum "$IMG" | grep dnode_)"
	"$FSCK" -n -f "$IMG" >"$WORKDIR/fsck.txt" 2>&1 \
		&& ok "bs$bs: spadfsck agrees with every kernel-written checksum" \
		|| bad "bs$bs: spadfsck rejects the kernel-written volume" "$(grep -v '^checking' "$WORKDIR/fsck.txt" | head -5)"

	banner "block size $bs: damaged dnode"
	cp "$IMG" "$WORKDIR/dn.img"
	if off=$(target_offset "$WORKDIR/dn.img" dnode); then
		flip "$WORKDIR/dn.img" "$off"
		out=$("$FSCK" -n -f "$WORKDIR/dn.img" 2>/dev/null)
		grep -q "BAD CHECKSUM ON DNODE" <<<"$out" \
			&& ok "bs$bs: spadfsck reports the damaged dnode" \
			|| bad "bs$bs: spadfsck does not report the damaged dnode" "$(grep -v '^checking' <<<"$out" | head -3)"
		attach "$WORKDIR/dn.img"
		kmsg_mark
		if mount -t spadfs2 -o ro "$LOOP" "$MNT"; then
			if ls "$MNT/d" >/dev/null 2>&1; then
				bad "bs$bs: listing a directory with a damaged dnode succeeded"
			else
				ok "bs$bs: listing the directory fails"
			fi
			umount "$MNT"
		else
			bad "bs$bs: read-only mount failed (the damage is in a dnode, not the mount path)"
		fi
		detach
		log=$(kmsg_since)
		grep -q "bad checksum on dnode" <<<"$log" \
			&& ok "bs$bs: the kernel logs the dnode checksum error" \
			|| bad "bs$bs: no dnode checksum error in the kernel log" "$(tail -3 <<<"$log")"
	else
		bad "bs$bs: no sealed dnode to damage"
	fi

	banner "block size $bs: damaged apage"
	cp "$IMG" "$WORKDIR/ap.img"
	if off=$(target_offset "$WORKDIR/ap.img" apage); then
		flip "$WORKDIR/ap.img" "$off"
		out=$("$FSCK" -n -f "$WORKDIR/ap.img" 2>/dev/null)
		grep -q "BAD CHECKSUM ON APAGE" <<<"$out" \
			&& ok "bs$bs: spadfsck reports the damaged apage" \
			|| bad "bs$bs: spadfsck does not report the damaged apage" "$(grep -v '^checking' <<<"$out" | head -3)"
		attach "$WORKDIR/ap.img"
		kmsg_mark
		if mount -t spadfs2 "$LOOP" "$MNT" 2>/dev/null; then
			bad "bs$bs: mount succeeded over a damaged apage (every apage is read at mount)"
			umount "$MNT"
		else
			ok "bs$bs: mount refuses the volume"
		fi
		detach
		log=$(kmsg_since)
		grep -q "bad checksum on apage" <<<"$log" \
			&& ok "bs$bs: the kernel logs the apage checksum error" \
			|| bad "bs$bs: no apage checksum error in the kernel log" "$(tail -3 <<<"$log")"
	else
		bad "bs$bs: no sealed apage to damage"
	fi
done

summary
