#!/usr/bin/env bash
# A mount + commit must not re-bless a damaged crash count table sector.
#
# The kernel used to build its in-RAM cct CRC table from the cct it read at
# mount, then rewrite whole CRC blocks from that table on every commit. With
# 4K blocks one block holds all five CRC units, so the first commit gave every
# cct sector -- rotted or not -- a fresh matching CRC, and fsck could no longer
# see the damage. The table is now seeded from the CRCs on disk.
#
# For each block size:
#   clean -- format, prime, mount, write, sync, umount: fsck must stay clean
#            (guards against the kernel writing CRCs fsck disagrees with)
#   rot   -- damage the ROT_SECTORS cct sectors without touching their CRCs,
#            then the same mount cycle: fsck must STILL report each one, and
#            the kernel must have warned at mount
#
# Why two sectors: a commit rewrites whole CRC blocks. With 4K blocks that is
# all five units, so the old code re-blessed every sector. With 512-byte
# blocks it is only unit 0 (sectors 0-126), so sector 300 survived even the
# old code; sector 50 shares unit 0 with the cc sector and did not.
#
# None of them may be the sector holding the current cc (sector 0 here):
# the kernel rewrites that one from RAM on every commit by design.
#
# Needs root (losetup, mount). Run: sudo bash tests/cct_crc_mount.sh

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (mount/losetup). Run: sudo bash tests/cct_crc_mount.sh"

ROT_SECTORS=${ROT_SECTORS:-"50 300"}
MNT="$WORKDIR/mnt"
LOOP=""

cleanup() {
	set +e
	mountpoint -q "$MNT" && umount "$MNT"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Test the module in this tree, not whatever an earlier run left loaded.
# Always reload: this kernel has no srcversion to compare against.
# SPADFS_KO points at another build (e.g. an unfixed one, to prove the
# test fails against it).
KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
[ -f "$KO" ] || die "no module at $KO"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -2)"
ok "loaded $KO"

# rot_cct IMG SECTOR -- flip one byte in a cct sector, leave its CRC alone.
rot_cct() {
	python3 -I - "$TESTS_DIR/sbinfo.py" "$1" "$2" <<'PY'
import subprocess, sys
sbinfo, img, sec = sys.argv[1], sys.argv[2], int(sys.argv[3])
info = dict(l.split("=", 1) for l in subprocess.run(
	[sys.executable, "-I", sbinfo, img], capture_output=True, text=True,
	check=True).stdout.splitlines() if "=" in l)
off = (int(info["cct"], 0) + sec) * 512 + 17
with open(img, "r+b") as f:
	f.seek(off)
	b = f.read(1)
	f.seek(off)
	f.write(bytes([b[0] ^ 0x5a]))
PY
}

# mount_cycle IMG -- mount, make the kernel commit, unmount.
mount_cycle() {
	LOOP=$(losetup --find --show "$1") || return 1
	mkdir -p "$MNT"
	mount -t spadfs2 "$LOOP" "$MNT" || { losetup -d "$LOOP"; LOOP=""; return 1; }
	echo "payload" >"$MNT/f.txt" && sync -f "$MNT/f.txt"
	sync
	umount "$MNT"
	losetup -d "$LOOP"
	LOOP=""
}

# cct_report IMG -- fsck's cct CRC lines. Always returns 0: fsck exits
# non-zero exactly when it finds the damage we are looking for, and under
# pipefail that would fail the check. Callers test the TEXT, never the rc.
cct_report() { "$FSCK" -f -n "$1" 2>&1 | grep -E 'CCT (SECTOR|CRC UNIT)' || true; }

for bs in 512 4096; do
	log ""
	log "== block size $bs =="

	img="$WORKDIR/clean-$bs.img"
	make_image "$img" 64M --checksums --block-size "$bs" || die "make_image failed"
	python3 -I "$TESTS_DIR/prime.py" "$img" --cc 1 --base-txc 100 >/dev/null 2>&1 \
		|| die "prime failed"
	if mount_cycle "$img"; then
		rep=$(cct_report "$img")
		[ -z "$rep" ] && ok "[$bs] clean volume: cct CRCs agree after mount + commit" \
			|| bad "[$bs] clean volume reports cct CRC errors after mount" "$rep"
	else
		bad "[$bs] clean volume mount cycle" "$(dmesg | tail -3)"
	fi

	img="$WORKDIR/rot-$bs.img"
	make_image "$img" 64M --checksums --block-size "$bs" || die "make_image failed"
	python3 -I "$TESTS_DIR/prime.py" "$img" --cc 1 --base-txc 100 >/dev/null 2>&1 \
		|| die "prime failed"
	nrot=0
	for sec in $ROT_SECTORS; do
		rot_cct "$img" "$sec" || die "rot failed"
		nrot=$((nrot + 1))
	done
	rep=$(cct_report "$img")
	for sec in $ROT_SECTORS; do
		grep -qF "CCT SECTOR $sec DOES NOT MATCH ITS CHECKSUM" <<<"$rep" \
			&& ok "[$bs] precondition: fsck sees rotted sector $sec before mount" \
			|| bad "[$bs] precondition: sector $sec not detected before mount" "$rep"
	done

	dmesg_before=$(dmesg | wc -l)
	if mount_cycle "$img"; then
		rep=$(cct_report "$img")
		for sec in $ROT_SECTORS; do
			grep -qF "CCT SECTOR $sec DOES NOT MATCH ITS CHECKSUM" <<<"$rep" \
				&& ok "[$bs] rotted sector $sec still reported after mount + commit" \
				|| bad "[$bs] mount + commit re-blessed rotted sector $sec" "$rep"
		done
		# capture first: grep -q can close the pipe early, and pipefail
		# would turn tail's SIGPIPE into a false FAIL
		new_dmesg=$(dmesg | tail -n +"$((dmesg_before + 1))")
		grep -qF "crash count table: $nrot of 512 cct sector" <<<"$new_dmesg" \
			&& ok "[$bs] kernel warned about the mismatch at mount" \
			|| bad "[$bs] no mount-time warning" "$(grep spadfs <<<"$new_dmesg" | head -3)"
	else
		bad "[$bs] rotted volume mount cycle" "$(dmesg | tail -3)"
	fi
done

summary
