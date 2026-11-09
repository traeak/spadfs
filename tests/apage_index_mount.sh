#!/usr/bin/env bash
# The driver keeps the apage index CRC current and checks it. RUN AS ROOT.
#
# The CRC of each apage index copy lives in the txblock (struct txblock,
# apage_index_crc). The driver rewrites an index copy only when an apage
# splits, and an apage only splits when its range is larger than one bitmap
# apage can cover (CONV_APAGE_SECTORS: ~64 MiB at 512-byte blocks, ~511 MiB
# at 4096); below that a full list apage converts to a bitmap instead. So
# the other mount tests, on 64 MiB volumes, never reach the index write.
#
#   split    a 256 MiB volume with 512-byte blocks; 2400 small files, every
#            other one deleted, leave ~1200 free fragments, more than one
#            list apage holds (~1020). After unmount the index must have
#            been rewritten (more than one apage in use), its CRC must match
#            the live copy, spadfsck must agree, and the volume must mount
#            again (the driver checks the CRC at mount).
#   damage   flip a byte of an unused tail entry of the live copy -- no
#            structural check reads it, only the CRC covers it. The mount
#            must refuse the volume with a bad apage index checksum, and
#            spadfsck -y must repair it so the volume mounts again.
#
# Usage: sudo bash tests/apage_index_mount.sh
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail
# Output is captured first and grepped from a here-string (see cc_wrap.sh).

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (losetup, mount). Run: sudo bash tests/apage_index_mount.sh"

KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
MNT="$WORKDIR/mnt"
IMG="$WORKDIR/idx.img"
LOOP=""

cleanup() {
	set +e
	mountpoint -q "$MNT" && umount "$MNT"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
attach() { LOOP=$(losetup --find --show "$IMG") || die "losetup failed"; }
detach() { [ -n "$LOOP" ] && losetup -d "$LOOP"; LOOP=""; }
idx() { python3 "$TESTS_DIR/apageidx.py" "$IMG"; }
crc_matches() { local s=${1#*stored=}; s=${s%% *}; [ "$s" = "${1##*computed=}" ]; }

banner "module"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -3)"
ok "loaded $KO"

banner "split: fragment the free space until an apage splits"
make_image "$IMG" 256M --block-size 512 >/dev/null || die "make_image failed"
before=$(idx)
echo "    before: $before"
mkdir -p "$MNT"
attach
mount -t spadfs2 "$LOOP" "$MNT" || die "mount failed: $(dmesg | tail -3)"
mkdir "$MNT/f"
for i in $(seq 0 2399); do head -c 1024 /dev/urandom >"$MNT/f/$i"; done
sync -f "$MNT"
for i in $(seq 0 2 2399); do rm "$MNT/f/$i"; done
sync -f "$MNT"
umount "$MNT" || die "umount failed"
detach
after=$(idx)
echo "    after:  $after"
active=$(sed 's/.*active=\([0-9]*\).*/\1/' <<<"$after")
[ "$active" -gt 1 ] \
	&& ok "an apage split: $active apages in use, so the driver rewrote the index" \
	|| bad "no apage split ($active in use); the index write was not exercised"
crc_matches "$after" \
	&& ok "the txblock's CRC matches the live index copy" \
	|| bad "index CRC does not match after the driver's writes" "$after"
"$FSCK" -n -f "$IMG" >/dev/null 2>&1 \
	&& ok "spadfsck agrees with the driver-written index" \
	|| bad "spadfsck rejects the driver-written volume" "$("$FSCK" -n -f "$IMG" 2>&1 | tr '\r' '\n' | grep -E 'APAGE|CHECKSUM' | head -3)"
attach
klog_mark
if mount -t spadfs2 "$LOOP" "$MNT"; then
	ok "the volume mounts again (the driver checked the index CRC)"
	[ "$(ls "$MNT/f" | wc -l)" = 1200 ] && ok "the 1200 surviving files are there" \
		|| bad "file count after remount is $(ls "$MNT/f" | wc -l), want 1200"
	umount "$MNT"
else
	bad "remount failed" "$(klog_since | tail -3)"
fi
detach

banner "damage: an unused tail entry of the live copy"
python3 - "$IMG" "$TESTS_DIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from sbinfo import read_super
from corrupt import n_apages
p = sys.argv[1]
info = read_super(p)
with open(p, "rb") as f:
    f.seek(info["txblock"] * 512)
    tx = f.read(512)
    a_cc = int.from_bytes(tx[10:12], "little")
    a_txc = int.from_bytes(tx[12:16], "little")
    f.seek(info["cct"] * 512 + a_cc * 4)
    cct = int.from_bytes(f.read(4), "little")
copy = 0 if ((cct - a_txc) & 0xFFFFFFFF) < 0x80000000 else 1
# the last entry is unused (end_sector 0); flip a bit of its apage pointer
off = info["apage_index%d" % copy] * 512 + (n_apages(info) - 1) * 16 + 2
with open(p, "r+b") as f:
    f.seek(off)
    b = f.read(1)[0]
    f.seek(off)
    f.write(bytes([b ^ 0x01]))
PY
attach
klog_mark
if mount -t spadfs2 "$LOOP" "$MNT" 2>/dev/null; then
	bad "mounted with a damaged apage index"
	umount "$MNT"
else
	ok "the mount refuses the damaged index"
fi
log=$(klog_since) || die "kernel log marker lost; rerun"
grep -q "bad checksum on apage index" <<<"$log" \
	&& ok "the kernel logs a bad apage index checksum" \
	|| bad "no apage index checksum error in the kernel log" "$(tail -3 <<<"$log")"
detach
"$FSCK" -y -f "$IMG" >/dev/null 2>&1
"$FSCK" -n -f "$IMG" >/dev/null 2>&1 \
	&& ok "spadfsck -y repairs it; forced re-check is clean" \
	|| bad "not clean after spadfsck -y"
attach
if mount -t spadfs2 "$LOOP" "$MNT"; then
	ok "the volume mounts after the repair"
	umount "$MNT"
else
	bad "no mount after the repair" "$(dmesg | tail -3)"
fi
detach

summary
