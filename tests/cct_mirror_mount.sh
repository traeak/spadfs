#!/usr/bin/env bash
# The kernel keeps the cct backup mirror current (Task 3.3).
#
# For each block size:
#   current  -- mount, write, sync, umount: the mirror must be a byte-for-byte
#               copy of the primary, stamped with the volume's crash count and
#               the txc on disk, and fsck must find nothing to do
#   crash    -- snapshot the image while mounted, right after an fsync (what a
#               power cut at that moment leaves), then damage the snapshot's
#               primary cct: fsck -y must restore the mirror unattended, and
#               the fsynced file must survive a mount of the result. This is
#               the case the mirror exists for.
#   damaged  -- damage the primary before mounting: the kernel must leave the
#               mirror's data alone (copying a damaged primary would destroy
#               the good copy), clear its stamp (the primary moves on without
#               it), and say so; fsck -y must then refuse to restore unasked
#
# Needs root (losetup, mount). Run: sudo bash tests/cct_mirror_mount.sh
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (mount/losetup). Run: sudo bash tests/cct_mirror_mount.sh"

MNT="$WORKDIR/mnt"
LOOP=""

cleanup() {
	set +e
	mountpoint -q "$MNT" && umount "$MNT"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Always reload: this kernel records no srcversion to compare against.
KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
[ -f "$KO" ] || die "no module at $KO"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -2)"
ok "loaded $KO"

stamp() { python3 -I "$TESTS_DIR/cctstamp.py" show "$1" | tr '\n' ' '; }
field() { python3 -I "$TESTS_DIR/cctstamp.py" show "$1" | sed -n "s/^$2=//p"; }

# rot_cct IMG SECTOR -- flip one byte in a primary cct sector, CRC untouched.
rot_cct() {
	python3 -I - "$TESTS_DIR" "$1" "$2" <<'PY'
import sys
tdir, img, sec = sys.argv[1], sys.argv[2], int(sys.argv[3])
sys.path.insert(0, tdir)
from sbinfo import read_super, SECTOR
off = (read_super(img)["cct"] + sec) * SECTOR + 17
with open(img, "r+b") as f:
	f.seek(off)
	b = f.read(1)
	f.seek(off)
	f.write(bytes([b[0] ^ 0x5a]))
PY
}

mirror_sum() {
	python3 -I - "$TESTS_DIR" "$1" <<'PY'
import hashlib, sys
tdir, img = sys.argv[1], sys.argv[2]
sys.path.insert(0, tdir)
from sbinfo import read_super, SECTOR
d = read_super(img)
with open(img, "rb") as f:
	f.seek(d["cct_backup"] * SECTOR)
	print(hashlib.sha256(f.read(512 * SECTOR)).hexdigest())
PY
}

do_mount() {
	LOOP=$(losetup --find --show "$1") || return 1
	mkdir -p "$MNT"
	mount -t spadfs2 "$LOOP" "$MNT" || { losetup -d "$LOOP"; LOOP=""; return 1; }
}
do_umount() {
	umount "$MNT"
	losetup -d "$LOOP"
	LOOP=""
}

fresh() { # fresh IMG BS -- formatted and primed (tx.cc 1, real txc values)
	make_image "$1" 64M --checksums --block-size "$2" || die "make_image failed"
	python3 -I "$TESTS_DIR/prime.py" "$1" --cc 1 --base-txc 100 >/dev/null 2>&1 \
		|| die "prime failed"
}

for bs in 512 4096; do
	log ""
	log "== block size $bs =="

	# ---------------------------------------------------------- current --
	img="$WORKDIR/cur-$bs.img"
	fresh "$img" "$bs"
	if do_mount "$img"; then
		echo "payload" >"$MNT/f.txt" && sync -f "$MNT/f.txt"
		do_umount
		s=$(stamp "$img")
		[ "$(field "$img" mirror)" = "same" ] \
			&& ok "[$bs] current: mirror is byte-identical to the primary" \
			|| bad "[$bs] current: mirror differs from the primary" "$s"
		[ "$(field "$img" magic)" = "ok" ] && [ "$(field "$img" sealed)" = "yes" ] \
			&& [ "$(field "$img" stamp_cc)" = "$(field "$img" tx_cc)" ] \
			&& [ "$(field "$img" stamp_txc)" = "$(field "$img" primary_txc)" ] \
			&& ok "[$bs] current: stamp names the volume's cc and on-disk txc" \
			|| bad "[$bs] current: stamp does not describe the primary" "$s"
		out=$("$FSCK" -f -n "$img" 2>&1); rc=$?
		[ "$rc" -eq 0 ] && ! grep -qE 'OUT OF DATE|BACKUP CORRUPT|DOES NOT MATCH' <<<"$out" \
			&& ok "[$bs] current: fsck finds nothing to do (rc=0)" \
			|| bad "[$bs] current: fsck rc=$rc" "$(grep -E 'CCT|BACKUP' <<<"$out" | head -3)"
	else
		bad "[$bs] current: mount failed" "$(dmesg | tail -3)"
	fi

	# ------------------------------------------------------------ crash --
	img="$WORKDIR/crash-$bs.img"
	snap="$WORKDIR/snap-$bs.img"
	fresh "$img" "$bs"
	if do_mount "$img"; then
		echo "acknowledged $bs" >"$MNT/acked.txt" && sync -f "$MNT/acked.txt"
		# the state a power cut right now would leave: mounted, dirty
		cp --sparse=always "$img" "$snap"
		do_umount
		s=$(stamp "$snap")
		[ "$(field "$snap" mirror)" = "same" ] \
			&& [ "$(field "$snap" stamp_cc)" -eq $(( $(field "$snap" tx_cc) - 1 )) ] \
			&& ok "[$bs] crash: mid-mount mirror is current, stamped with the working cc" \
			|| bad "[$bs] crash: mid-mount mirror state" "$s"
		rot_cct "$snap" 0
		out=$("$FSCK" -f -y "$snap" 2>&1)
		grep -qF 'THE BACKUP IS FROM THE CURRENT CRASH EPOCH' <<<"$out" \
			&& grep -qF 'CRASH COUNT TABLE RESTORED FROM BACKUP' <<<"$out" \
			&& ok "[$bs] crash: fsck -y restores the current mirror unattended" \
			|| bad "[$bs] crash: no unattended restore" "$(grep -E 'CCT|BACKUP|CRASH COUNT' <<<"$out" | head -4)"
		out=$("$FSCK" -f -n "$snap" 2>&1); rc=$?
		[ "$rc" -eq 0 ] && ok "[$bs] crash: restored volume checks clean (rc=0)" \
			|| bad "[$bs] crash: restored volume rc=$rc" "$(grep -v '%' <<<"$out" | tail -4)"
		if do_mount "$snap"; then
			[ "$(cat "$MNT/acked.txt" 2>/dev/null)" = "acknowledged $bs" ] \
				&& ok "[$bs] crash: the fsynced file survived the restore" \
				|| bad "[$bs] crash: fsynced file lost or wrong after restore" "$(ls -la "$MNT")"
			do_umount
		else
			bad "[$bs] crash: restored volume does not mount" "$(dmesg | tail -3)"
		fi
	else
		bad "[$bs] crash: mount failed" "$(dmesg | tail -3)"
	fi

	# ---------------------------------------------------------- damaged --
	img="$WORKDIR/dmg-$bs.img"
	fresh "$img" "$bs"
	rot_cct "$img" 300
	before=$(mirror_sum "$img")
	dmesg_before=$(dmesg | wc -l)
	if do_mount "$img"; then
		echo "payload" >"$MNT/f.txt" && sync -f "$MNT/f.txt"
		do_umount
		new_dmesg=$(dmesg | tail -n +"$((dmesg_before + 1))")
		[ "$(mirror_sum "$img")" = "$before" ] \
			&& ok "[$bs] damaged: the mirror's data was not overwritten" \
			|| bad "[$bs] damaged: the mirror was overwritten from a damaged primary"
		[ "$(field "$img" magic)" != "ok" ] && [ "$(field "$img" sealed)" = "yes" ] \
			&& ok "[$bs] damaged: the stamp was withdrawn, the unit still sealed" \
			|| bad "[$bs] damaged: stamp still vouches for a mirror left behind" "$(stamp "$img")"
		grep -qF 'leaving its backup untouched' <<<"$new_dmesg" \
			&& ok "[$bs] damaged: the kernel said why" \
			|| bad "[$bs] damaged: no kernel message" "$(grep spadfs <<<"$new_dmesg" | head -3)"
		out=$("$FSCK" -f -y "$img" 2>&1)
		grep -qF 'NO VALID EPOCH STAMP' <<<"$out" \
			&& ! grep -qF 'CRASH COUNT TABLE RESTORED FROM BACKUP' <<<"$out" \
			&& ok "[$bs] damaged: fsck -y will not restore the unstamped mirror" \
			|| bad "[$bs] damaged: fsck -y restore decision" "$(grep -E 'BACKUP|STAMP' <<<"$out" | head -4)"
	else
		bad "[$bs] damaged: mount failed" "$(dmesg | tail -3)"
	fi
done

summary
