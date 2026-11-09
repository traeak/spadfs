#!/usr/bin/env bash
# Crash consistency under a volatile write cache, via dm-log-writes. RUN AS ROOT.
#
# Every other crash test here either crashes the kernel (panic, sysrq 'b') or
# snapshots a live image. Neither loses the drive's write cache, so neither can
# see a commit that reaches media before the metadata it commits -- the bug
# ef221c6 fixed. This test can:
#
#   1. mount spadfs2 on a dm-log-writes target, which records every write
#      and every cache flush in order
#   2. run a workload; after each operation, syncfs and drop an ack.N mark
#      plus a manifest of the tree the operation was acknowledged with
#   3. replay the log (tests/logwrites.py) into every state a power cut could
#      leave: all writes before some flush, plus ANY SUBSET of the writes
#      between it and the next flush -- including "the cct landed, one other
#      write did not", the exact shape of the ordering bug
#   4. judge each state: spadfsck, then a read-only mount through the kernel
#      (the reader a reboot actually uses) compared with the last
#      acknowledged manifest
#
# Also checked directly from the log: every cct write sits in a flush
# interval of its own (plus its CRC region). That one line is the commit
# protocol, read straight off the device.
#
# Self-checks, so a pass cannot be vacuous:
#   - the log must end with the target's own end mark (complete history)
#   - replaying the whole log onto the base must reproduce the device byte
#     for byte (the replayer is faithful)
#   - the cleanly unmounted device must be fsck-clean (the oracle is sane)
#   - at least one interval must contain a cct write (commits were logged)
#
# Positive control: run against a module built from the commit before
# ef221c6 (no pre-cct flush). The ordering check must FAIL:
#   git worktree add /tmp/spadfs-prefix ef221c6~1
#   make -C /lib/modules/$(uname -r)/build M=/tmp/spadfs-prefix \
#        STANDALONE_BUILD=y CONFIG_SPADFS_FS=m SPADFS_NAME=spadfs2
#   sudo SPADFS_KO=/tmp/spadfs-prefix/spadfs2.ko bash tests/crash_logwrites.sh
#
# Usage: sudo bash tests/crash_logwrites.sh [--block-size N]... [--random N]
#                                           [--prefix-cap N] [--no-mount] [--keep]
# SPADFS_KO selects a module other than the one built in this tree.
# RESULTS_DIR (default /var/tmp/spadfs-logwrites.<pid>) keeps the per-state
# report, failure details and up to three failing images, and survives the run.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

[ "$(id -u)" -eq 0 ] || die "needs root (dmsetup, losetup, mount). Run: sudo bash tests/crash_logwrites.sh"

KO=${SPADFS_KO:-$REPO_ROOT/spadfs2.ko}
RESULTS_DIR=${RESULTS_DIR:-/var/tmp/spadfs-logwrites.$$}
LW="$TESTS_DIR/logwrites.py"
BLOCK_SIZES=""
RANDOM_N=6
PREFIX_CAP=16
NO_MOUNT=""
KEEP=0

while [ $# -gt 0 ]; do
	case "$1" in
	--block-size) BLOCK_SIZES="$BLOCK_SIZES $2"; shift 2 ;;
	--random) RANDOM_N=$2; shift 2 ;;
	--prefix-cap) PREFIX_CAP=$2; shift 2 ;;
	--no-mount) NO_MOUNT=--no-mount; shift ;;
	--keep) KEEP=1; shift ;;
	*) die "unknown argument: $1" ;;
	esac
done
BLOCK_SIZES=${BLOCK_SIZES:-"512 4096"}

for t in dmsetup losetup blockdev mount umount python3 cmp; do
	command -v "$t" >/dev/null || die "missing tool: $t"
done
[ -r "$KO" ] || die "no module at $KO (run the kbuild make, or set SPADFS_KO)"

DM="spadfs-lw-$$"
MNT="$WORKDIR/mnt"
LOOPD=""
LOOPL=""

teardown() {
	mountpoint -q "$MNT" && umount "$MNT"
	dmsetup info "$DM" >/dev/null 2>&1 && dmsetup remove "$DM"
	[ -n "$LOOPD" ] && losetup -d "$LOOPD"
	[ -n "$LOOPL" ] && losetup -d "$LOOPL"
	LOOPD=""
	LOOPL=""
}

cleanup() {
	set +e
	teardown 2>/dev/null
	[ "$KEEP" = 1 ] && printf 'kept work directory: %s\n' "$WORKDIR" || rm -rf "$WORKDIR"
}
trap cleanup EXIT

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# workload: one operation per commit, each exercising a different structure
# ---------------------------------------------------------------------------

# deterministic content: gen <bytes> <seed>
gen() {
	python3 -c 'import sys
n, s = int(sys.argv[1]), int(sys.argv[2])
sys.stdout.buffer.write(bytes((i * 31 + s * 17 + (i >> 9)) & 255 for i in range(n)))' "$1" "$2"
}

op_1()  { printf 'payload one\n' > f1; }
op_2()  { mkdir d1 && printf 'aaa\n' > d1/a; }
op_3()  { printf 'more\n' >> f1; }
# interleaved appends, so the two files fragment and grow past their inline
# extents (anode path)
op_4()  { local i; for i in $(seq 0 23); do gen 32768 "$i" >> big && gen 32768 $((i + 100)) >> big2 || return 1; done; }
# enough names in one directory to split fnode blocks (dnode path)
op_5()  { local i; mkdir d2 && for i in $(seq 0 299); do printf 'n%d\n' "$i" > "d2/f$i" || return 1; done; }
op_6()  { mv f1 d1/f1moved; }
op_7()  { ln d1/a hl; }                   # hard link: fixed_fnode_block
op_8()  { local i; for i in $(seq 0 149); do rm "d2/f$i" || return 1; done; }
op_9()  { ln -s d1/f1moved sl; }          # symlink EA
op_10() { truncate -s 1048576 big; }
op_11() { gen 4096 77 | dd of=big2 bs=4096 seek=2 conv=notrunc status=none; }
op_12() { rm -r d1; }                     # hl survives with nlink 1
op_13() { mv d2 d3; }
N_OPS=13

# syncfs, then record what was acknowledged, then mark the log. Order
# matters: the mark must follow the commit's flushes in the log.
ack() {
	sync -f "$MNT" &&
	python3 "$LW" manifest "$MNT" "$DIR/manifests/manifest.$1.json" &&
	dmsetup message "$DM" 0 mark "ack.$1"
}

run_block_size() {
	local bs=$1 n r
	DIR="$WORKDIR/bs$bs"
	local res="$RESULTS_DIR/bs$bs"
	mkdir -p "$DIR/manifests" "$MNT" "$res"

	banner "block size $bs: record"
	make_image "$DIR/dev.img" 32M --block-size "$bs" >/dev/null || { bad "bs$bs: mkfs"; return; }
	cp "$DIR/dev.img" "$DIR/base.img"
	truncate -s 512M "$DIR/log.img"

	LOOPD=$(losetup --find --show "$DIR/dev.img") || { bad "bs$bs: losetup"; return; }
	LOOPL=$(losetup --find --show "$DIR/log.img") || { bad "bs$bs: losetup"; teardown; return; }
	if ! dmsetup create "$DM" --table "0 $(blockdev --getsz "$LOOPD") log-writes $LOOPD $LOOPL"; then
		bad "bs$bs: dmsetup create log-writes"
		teardown
		return
	fi
	# noatime: the manifest walk must not dirty anything. sync_time: no
	# background commits, so every commit is one the workload asked for.
	if ! mount -t spadfs2 -o noatime,sync_time=3600 "/dev/mapper/$DM" "$MNT"; then
		bad "bs$bs: mount on the log-writes target" "$(dmesg | tail -3)"
		teardown
		return
	fi
	if ! ack 0; then
		bad "bs$bs: initial ack"
		teardown
		return
	fi
	for n in $(seq 1 "$N_OPS"); do
		( cd "$MNT" && "op_$n" ) || bad "bs$bs: workload op $n"
		ack "$n" || { bad "bs$bs: ack $n"; break; }
	done
	umount "$MNT" || { bad "bs$bs: umount" "$(dmesg | tail -3)"; teardown; return; }
	teardown
	ok "bs$bs: workload recorded ($N_OPS operations)"

	banner "block size $bs: self-checks"
	python3 "$LW" parse "$DIR/log.img" | sed 's/^/    /'
	if python3 "$LW" replay --log "$DIR/log.img" --base "$DIR/base.img" --out "$DIR/replayed.img" &&
	   cmp -s "$DIR/replayed.img" "$DIR/dev.img"; then
		ok "bs$bs: replaying the whole log reproduces the device exactly"
	else
		bad "bs$bs: replay does not reproduce the device -- the replayer cannot be trusted"
		return
	fi
	rm -f "$DIR/replayed.img"
	if "$FSCK" -n -f "$DIR/dev.img" >"$res/fsck-final.txt" 2>&1; then
		ok "bs$bs: the cleanly unmounted device is fsck-clean"
	else
		bad "bs$bs: the cleanly unmounted device is not clean; crash states would be noise" \
		    "see $res/fsck-final.txt"
		return
	fi

	banner "block size $bs: replay every crash state"
	python3 "$LW" check --log "$DIR/log.img" --base "$DIR/base.img" \
		--fsck "$FSCK" --manifests "$DIR/manifests" --require-end \
		--random "$RANDOM_N" --prefix-cap "$PREFIX_CAP" $NO_MOUNT \
		--results "$res" --workdir "$DIR" | sed 's/^/    /'
	r=${PIPESTATUS[0]}
	if [ "$r" -eq 0 ]; then
		ok "bs$bs: commit ordering and every crash state"
	else
		bad "bs$bs: crash states (rc=$r)" "details: $res"
		# keep what is needed to rerun the analysis without root
		cp "$DIR/log.img" "$DIR/base.img" "$res/" 2>/dev/null
		cp -r "$DIR/manifests" "$res/" 2>/dev/null
	fi
}

banner "module"
modprobe dm-log-writes || die "cannot load dm-log-writes"
if [ -e /sys/module/spadfs2 ]; then
	rmmod spadfs2 || die "spadfs2 is loaded and cannot be unloaded (mounted?)"
fi
insmod "$KO" || die "insmod $KO failed: $(dmesg | tail -3)"
ok "loaded $KO"

for bs in $BLOCK_SIZES; do
	run_block_size "$bs"
done

printf '\nresults: %s\n' "$RESULTS_DIR"
summary
