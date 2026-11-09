#!/usr/bin/env bash
# The kernel half of the crash-count ceiling (tests/cc_wrap.sh is the fsck
# half). Mounting increments tx.cc, so a volume primed to cc N is checked
# against the threshold as N+1.
#
#   quiet    primed 0xfefe -> mounts as 0xfeff: read-write, no warning
#   warn     primed 0xff10 -> mounts as 0xff11: read-write, warns with the
#            exact headroom (0xffff - 0xff11 = 238), and a clean unmount
#            steps tx.cc back to 0xff10 -- clean cycles cost no headroom
#   ceiling  primed 0xffff: the kernel refuses read-write and says how to
#            recover; spadfsck -y resets it; the reset volume then mounts
#            read-write without a warning and keeps the file written
#
# Needs root (losetup, mount). Run: sudo bash tests/cc_wrap_mount.sh
# SPADFS_KO selects a module other than the one built in this tree.

set -uo pipefail
# Under pipefail, `cmd | grep -q x` takes cmd's exit status: spadfsck exits
# non-zero when it reports anything, and a producer can die of SIGPIPE once
# grep -q has matched. That turns a match into "no match" -- a false FAIL on
# a presence check and, worse, a false PASS on an absence check. So output is
# captured first and grepped from a here-string.

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (mount/losetup). Run: sudo bash tests/cc_wrap_mount.sh"

MNT="$WORKDIR/mnt"
LOOP=""

cleanup() {
	set +e
	mountpoint -q "$MNT" && umount "$MNT"
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
[ -f "$KO" ] || die "no module at $KO"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -2)"
ok "loaded $KO"

tx_cc() {
	python3 -I - "$1" "$TESTS_DIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from sbinfo import read_super
p = sys.argv[1]
with open(p, "rb") as f:
    f.seek(read_super(p)["txblock"] * 512)
    print(int.from_bytes(f.read(8)[6:8], "little"))
PY
}

primed() { # primed IMG CC
	make_image "$1" 64M || die "make_image failed"
	python3 -I "$TESTS_DIR/prime.py" "$1" --cc "$2" --base-txc 100 >/dev/null 2>&1 ||
		die "prime to cc $2 failed"
}

# Kernel messages since the last mark. A line count, not dmesg -c: the test
# must not erase the host's log.
KMARK=0
kmark() { KMARK=$(dmesg | wc -l); }
klog() { dmesg | tail -n +"$((KMARK + 1))"; }

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
mounted_rw() {
	local opts
	opts=$(findmnt -n -o OPTIONS "$MNT") || return 1
	grep -qx rw <<<"${opts//,/$'\n'}"
}

# ------------------------------------------------------------- quiet --
img="$WORKDIR/quiet.img"
primed "$img" $((0xfefe))
kmark
if do_mount "$img"; then
	mounted_rw && ok "quiet: read-write" || bad "quiet: read-write" "mounted read-only"
	log=$(klog)
	if grep -q 'crash count is' <<<"$log"; then
		bad "quiet: no warning below the threshold" "$(grep 'crash count' <<<"$log")"
	else
		ok "quiet: no warning below the threshold"
	fi
	do_umount
else
	bad "quiet: mount failed" "$(klog | tail -3)"
fi

# -------------------------------------------------------------- warn --
img="$WORKDIR/warn.img"
primed "$img" $((0xff10))
kmark
if do_mount "$img"; then
	mounted_rw && ok "warn: still read-write" || bad "warn: still read-write" "mounted read-only"
	want='crash count is ff11 of ffff, headroom for 238 more unclean shutdown(s)'
	log=$(klog)
	if grep -qF "$want" <<<"$log"; then
		ok "warn: warns with the exact headroom"
	else
		bad "warn: warns with the exact headroom" "want '$want'; got: $(klog | tail -3)"
	fi
	echo payload >"$MNT/f.txt" && sync -f "$MNT/f.txt"
	do_umount
	got=$(tx_cc "$img")
	[ "$got" = $((0xff10)) ] && ok "warn: clean unmount steps tx.cc back (no headroom used)" ||
		bad "warn: clean unmount steps tx.cc back (no headroom used)" "tx.cc=$(printf %04x "$got")"
else
	bad "warn: mount failed" "$(klog | tail -3)"
fi

# ----------------------------------------------------------- ceiling --
img="$WORKDIR/ceiling.img"
primed "$img" $((0xffff))
kmark
if do_mount "$img"; then
	if mounted_rw && touch "$MNT/should-fail" 2>/dev/null; then
		bad "ceiling: refuses writes" "a file was created at cc ffff"
	else
		ok "ceiling: refuses writes"
	fi
	log=$(klog)
	if grep -qF 'crash count wrap-around, remounting read-only; run spadfsck --reset-crash-counts' <<<"$log"; then
		ok "ceiling: says how to recover"
	else
		bad "ceiling: says how to recover" "$(klog | tail -3)"
	fi
	do_umount
else
	# Refusing the mount outright is also a safe outcome; record which.
	ok "ceiling: mount refused ($(klog | tail -1))"
fi

"$FSCK" -y "$img" >"$WORKDIR/reset.log" 2>&1
got=$(tx_cc "$img")
[ "$got" = 0 ] && ok "ceiling: spadfsck -y resets tx.cc to 0" ||
	bad "ceiling: spadfsck -y resets tx.cc to 0" "tx.cc=$got; $(tail -3 "$WORKDIR/reset.log")"

kmark
if do_mount "$img"; then
	mounted_rw && ok "reset: read-write again" || bad "reset: read-write again" "mounted read-only"
	log=$(klog)
	if grep -q 'crash count' <<<"$log"; then
		bad "reset: no crash-count message" "$(grep 'crash count' <<<"$log")"
	else
		ok "reset: no crash-count message"
	fi
	echo after-reset >"$MNT/g.txt" && sync -f "$MNT/g.txt"
	do_umount
	do_mount "$img" || die "remount after reset failed"
	[ "$(cat "$MNT/g.txt" 2>/dev/null)" = after-reset ] && ok "reset: a file written after the reset survives a remount" ||
		bad "reset: a file written after the reset survives a remount" "content lost"
	do_umount
else
	bad "reset: mount failed" "$(klog | tail -3)"
fi

if check_clean "$img" -n; then
	ok "final: fsck -n -f clean"
else
	bad "final: fsck -n -f clean" "$("$FSCK" -n -f "$img" 2>&1 | tail -3)"
fi

summary
