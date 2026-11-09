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
#   flag     checksums cannot be switched off: clear a dnode's and the root
#            directory fnode block's CHECKSUM_VALID bit and damage the
#            payload -- the kernel must still reject both; and a volume with
#            a bit set in flags_compat_fsck (the removed NO_CHECKSUMS) must
#            not mount
#
# The seal workload also runs symlinks, hard links, renames, xattrs,
# truncation and a fragmented file, so the "spadfsck agrees" check -- fsck
# verifies every block whatever its flag says -- covers those write paths.
#
# Positive controls (build each module in a worktree, then
# `sudo SPADFS_KO=<dir>/spadfs2.ko bash tests/meta_csum_mount.sh`):
#   82f4752~1  must fail "seal": its dnodes and modified apages come out
#              unsealed
#   the commit before checksums became always-on must fail "flag": it
#   trusts the cleared bit and mounts the flagged volume
#   git worktree add /tmp/spadfs-old <commit>
#   make -C /lib/modules/$(uname -r)/build M=/tmp/spadfs-old \
#        STANDALONE_BUILD=y CONFIG_SPADFS_FS=m SPADFS_NAME=spadfs2
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

# kernel log since the last kmsg_mark (lib.sh; dies if the marker scrolled out)
kmsg_mark() { klog_mark; }
kmsg_since() { klog_since; }

metasum() { python3 "$TESTS_DIR/metasum.py" "$@"; }

# Patch an image in place: clear a CHECKSUM_VALID bit and damage the payload,
# keeping every other check satisfied so only the cleared bit stands between
# the damage and a reader that trusts it.
#   dnode     clear the active version's flag (outside the folded range) and
#             flip a byte of its entries
#   rootdir   clear the root directory's first fnode block flag, rebalance
#             the XOR fold, and flip a payload byte
#   compat    set bit 0 of flags_compat_fsck and reseal the superblock
patch_flag() {
	python3 - "$1" "$2" "$TESTS_DIR" <<'PY'
import sys
img, kind, tests = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, tests)
import metasum, sbinfo
from corrupt import restripe_trailer

def flip(f, off, mask):
    f.seek(off); b = f.read(1)[0]; f.seek(off); f.write(bytes([b ^ mask]))

info = sbinfo.read_super(img)
with open(img, "r+b") as f:
    if kind == "dnode":
        vol = metasum.Volume(img)
        ds = vol.dnode_data_size()
        for sec, version in vol.dnodes():
            hdr = sec * 512 + 4 + version
            if vol.img[hdr] & metasum.DNODE_CHECKSUM_VALID:
                flip(f, hdr, metasum.DNODE_CHECKSUM_VALID)
                flip(f, sec * 512 + metasum.DNODE_ENTRY_OFFSET + version * ds + 7, 0x10)
                break
        else:
            sys.exit(1)
    elif kind == "rootdir":
        off = (info["root"] + (1 << info["sectors_per_block_bits"])) * 512
        flip(f, off + 4, 0x01)
        flip(f, off + 5, 0x01)
        flip(f, off + 40, 0x20)
    elif kind == "compat":
        off = sbinfo.SUPERBLOCK_SECTOR * 512
        f.seek(off); u = bytearray(f.read(512))
        u[96] |= 0x01
        restripe_trailer(u)
        f.seek(off); f.write(u)
PY
}
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
	# other fnode, anode and fixed-fnode write paths
	mkdir -p "$MNT/e/f/g"
	ln -s ../d/name-1 "$MNT/e/sym"
	ln -s "$(printf 'p%.0s' $(seq 1 150))" "$MNT/e/longsym"
	ln "$MNT/d/name-2" "$MNT/e/hard"
	mv "$MNT/d/name-3" "$MNT/e/f/moved"
	setfattr -n user.k -v v1 "$MNT/e/hard" 2>/dev/null || true
	for i in $(seq 0 63); do
		dd if=/dev/urandom bs=4k count=1 status=none >>"$MNT/frag-a"
		dd if=/dev/urandom bs=4k count=1 status=none >>"$MNT/frag-b"
	done
	truncate -s 10000 "$MNT/frag-b"
	sync -f "$MNT"
	# commit 2: change the same dnodes and apages again (copy-and-reseal)
	for i in $(seq 0 999); do rm "$MNT/d/name-$i"; done
	for i in $(seq 3000 3999); do printf 'y%d\n' "$i" > "$MNT/d/name-$i"; done
	rm "$MNT/blob"
	rm "$MNT/e/sym"
	rmdir "$MNT/e/f/g"
	setfattr -x user.k "$MNT/e/hard" 2>/dev/null || true
	truncate -s 0 "$MNT/frag-a"
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
		log=$(kmsg_since) || die "kernel log marker lost; rerun"
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
		log=$(kmsg_since) || die "kernel log marker lost; rerun"
		grep -q "bad checksum on apage" <<<"$log" \
			&& ok "bs$bs: the kernel logs the apage checksum error" \
			|| bad "bs$bs: no apage checksum error in the kernel log" "$(tail -3 <<<"$log")"
	else
		bad "bs$bs: no sealed apage to damage"
	fi

	banner "block size $bs: cleared flag"
	cp "$IMG" "$WORKDIR/fl.img"
	if patch_flag "$WORKDIR/fl.img" dnode; then
		attach "$WORKDIR/fl.img"
		kmsg_mark
		if mount -t spadfs2 -o ro "$LOOP" "$MNT"; then
			ls "$MNT/d" >/dev/null 2>&1 \
				&& bad "bs$bs: a damaged dnode with its flag cleared was listed" \
				|| ok "bs$bs: a damaged dnode with its flag cleared is still rejected"
			umount "$MNT"
		else
			bad "bs$bs: read-only mount failed (the damage is in a dnode)"
		fi
		detach
		log=$(kmsg_since) || die "kernel log marker lost; rerun"
		grep -q "bad checksum on dnode" <<<"$log" \
			&& ok "bs$bs: the kernel logs it as a dnode checksum error" \
			|| bad "bs$bs: no dnode checksum error for the cleared-flag dnode" "$(tail -3 <<<"$log")"
	else
		bad "bs$bs: no sealed dnode to clear"
	fi

	cp "$IMG" "$WORKDIR/fr.img"
	patch_flag "$WORKDIR/fr.img" rootdir
	attach "$WORKDIR/fr.img"
	kmsg_mark
	if mount -t spadfs2 -o ro "$LOOP" "$MNT" 2>/dev/null; then
		ls "$MNT" >/dev/null 2>&1 \
			&& bad "bs$bs: a damaged root directory block with its flag cleared was listed" \
			|| ok "bs$bs: a damaged root directory block with its flag cleared is still rejected"
		umount "$MNT"
	else
		ok "bs$bs: a damaged root directory block with its flag cleared stops the mount"
	fi
	detach
	log=$(kmsg_since) || die "kernel log marker lost; rerun"
	grep -q "bad checksum on fnode block" <<<"$log" \
		&& ok "bs$bs: the kernel logs it as an fnode block checksum error" \
		|| bad "bs$bs: no fnode block checksum error for the cleared-flag block" "$(tail -3 <<<"$log")"

	cp "$IMG" "$WORKDIR/fc.img"
	patch_flag "$WORKDIR/fc.img" compat
	attach "$WORKDIR/fc.img"
	kmsg_mark
	if mount -t spadfs2 -o ro "$LOOP" "$MNT" 2>/dev/null; then
		bad "bs$bs: a volume with flags_compat_fsck set mounted"
		umount "$MNT"
	else
		ok "bs$bs: a volume with flags_compat_fsck set does not mount"
	fi
	detach
	log=$(kmsg_since) || die "kernel log marker lost; rerun"
	grep -q "incompatible feature flags" <<<"$log" \
		&& ok "bs$bs: the kernel names the incompatible flag" \
		|| bad "bs$bs: no incompatible-flags message" "$(tail -3 <<<"$log")"
done

summary
