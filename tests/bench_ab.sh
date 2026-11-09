#!/usr/bin/env bash
# Side-by-side benchmark of filesystems on one device. RUN AS ROOT.
# THIS ERASES DEV EVERY ROUND -- scratch partition only.
#
# Each side is three arguments:
#   KO      the module to load, or - for a filesystem built into the kernel
#           or loadable with modprobe (ext4, xfs)
#   TYPE    the filesystem type for mount, optionally with mount options
#           after a comma: ext4,noinit_itable
#   MKFS    the mkfs command, as one argument; DEV is appended. A bare
#           mkspadfs/mkspadfs2 path gets --no-trim added.
# Two sides is an A/B comparison; more are allowed. Rounds rotate which side
# goes first, and DEV is reformatted before every side of every round, so
# all sides start from a fresh volume and drift (thermal, background I/O,
# the disk's cache) lands on all of them.
#
# Per round and side, in this order:
#   commit    ITERS x (write a small file, syncfs): ms per commit
#   create    NFILES small files in one directory, then syncfs: seconds
#   stat      after a remount (cold cache), stat every one of them: seconds
#   unlink    delete them all, then syncfs: seconds
#   fsync1    1 writer creating NSYNC 4 KiB files, fsync after each: ops/s
#   fsync8    8 parallel writers doing the same in their own directories:
#             total ops/s -- where commit batching and lock design show
#   write     SEQ_MB of sequential data with fsync: MB/s
#   read      after a remount (cold cache), read it back: MB/s
# write/read are a control: they should be close to the raw device for
# every filesystem; a large difference points at placement or the device.
#
# Usage:
#   sudo bash tests/bench_ab.sh [-d DEV] [-m MNT] [-r ROUNDS] [-n ITERS]
#        [-f NFILES] [-y NSYNC] [-s SEQ_MB]
#        KO TYPE MKFS  KO TYPE MKFS  [KO TYPE MKFS ...]
#
# DEV must currently be mounted at MNT as one of the sides' types (or spadfs
# or spadfs2) -- the proof that it is the scratch volume. Defaults:
# /dev/sda1, /mnt/spadcrash, 3 rounds, 50 commits, 20000 files, 50 fsyncs
# per writer, 256 MB. It is left formatted and mounted as the first side.
#
# Examples:
#   # v1.0.22 against this tree (tests/build_reference.sh first)
#   sudo bash tests/bench_ab.sh /tmp/spadfs-ref/spadfs.ko spadfs /tmp/spadfs-ref/mkspadfs \
#        ./spadfs2.ko spadfs2 ./mkspadfs2
#   # spadfs2 against ext4 and XFS
#   sudo bash tests/bench_ab.sh ./spadfs2.ko spadfs2 ./mkspadfs2 \
#        - ext4,noinit_itable 'mkfs.ext4 -F -q' \
#        - xfs 'mkfs.xfs -f -q'
# noinit_itable stops ext4 initialising inode tables in a background thread
# after mount, which would otherwise run during the measurements.

set -uo pipefail

DEV=/dev/sda1
MNT=/mnt/spadcrash
ROUNDS=3
ITERS=50
NFILES=20000
NSYNC=50
SEQ_MB=256

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DEV=$2; shift 2 ;;
	-m) MNT=$2; shift 2 ;;
	-r) ROUNDS=$2; shift 2 ;;
	-n) ITERS=$2; shift 2 ;;
	-f) NFILES=$2; shift 2 ;;
	-y) NSYNC=$2; shift 2 ;;
	-s) SEQ_MB=$2; shift 2 ;;
	-) break ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

die() { echo "bench_ab: $*" >&2; exit 1; }
[ $# -ge 6 ] && [ $(($# % 3)) -eq 0 ] ||
	{ echo "usage: $0 [options] KO TYPE MKFS KO TYPE MKFS [KO TYPE MKFS ...]" >&2; exit 2; }

KO=(); TYPE=(); OPTS=(); MKFS=(); LABEL=()
while [ $# -gt 0 ]; do
	ko=$1 t=$2 mk=$3; shift 3
	[ "$ko" = - ] || ko=$(readlink -f "$ko")
	KO+=("$ko")
	TYPE+=("${t%%,*}")
	if [ "$t" = "${t#*,}" ]; then OPTS+=(""); else OPTS+=("${t#*,}"); fi
	case "$(basename "${mk%% *}")" in
	mkspadfs|mkspadfs2) [ "$mk" = "${mk%% *}" ] && mk="$(readlink -f "$mk") --no-trim" ;;
	esac
	MKFS+=("$mk")
	LABEL+=("${t%%,*}")
done
N=${#TYPE[@]}

[ "$(id -u)" = 0 ] || die "needs root"
for i in $(seq 0 $((N - 1))); do
	if [ "${KO[$i]}" != - ]; then
		[ -r "${KO[$i]}" ] || die "no module at ${KO[$i]}"
	fi
	command -v "${MKFS[$i]%% *}" >/dev/null || die "no mkfs '${MKFS[$i]%% *}'"
done
fstype=$(findmnt -n -o FSTYPE -S "$DEV" 2>/dev/null | head -1)
target=$(findmnt -n -o TARGET -S "$DEV" 2>/dev/null | head -1)
known=" spadfs spadfs2 ${TYPE[*]} "
case "$known" in
*" $fstype "*) [ -n "$fstype" ] && [ "$target" = "$MNT" ] ||
	die "$DEV is not mounted at $MNT (found '$fstype' at '$target'); refusing to erase it" ;;
*) die "$DEV is mounted as '$fstype', not one of:$known; refusing to erase it" ;;
esac

# load every side's filesystem; module names differ, so all stay loaded
umount "$MNT" || die "cannot unmount $MNT"
for i in $(seq 0 $((N - 1))); do
	if [ "${KO[$i]}" = - ]; then
		grep -qw "${TYPE[$i]}" /proc/filesystems || modprobe "${TYPE[$i]}" ||
			die "filesystem ${TYPE[$i]} is not available"
	else
		name=$(basename "${KO[$i]}" .ko)
		if [ -e "/sys/module/$name" ]; then rmmod "$name" || die "cannot unload $name"; fi
		insmod "${KO[$i]}" || die "insmod ${KO[$i]} failed"
	fi
done

now() { date +%s%N; }
secs() { awk -v ns=$(($2 - $1)) 'BEGIN { printf "%.2f", ns / 1e9 }'; }
mnt() {
	if [ -n "$2" ]; then mount -t "$1" -o "$2" "$DEV" "$MNT"; else mount -t "$1" "$DEV" "$MNT"; fi ||
		die "mount -t $1 failed: $(dmesg | tail -2)"
}
remount() { umount "$MNT" || die "umount failed"; mnt "$1" "$2"; }

# P writers, each creating NSYNC 4 KiB files in its own directory with an
# fsync after each; prints total fsyncs per second
fsync_rate() {
	local p=$1 t0 t1 w
	for w in $(seq 1 "$p"); do mkdir "$MNT/s$w"; done
	sync -f "$MNT"
	t0=$(now)
	for w in $(seq 1 "$p"); do
		python3 -c '
import os, sys
d, n = sys.argv[1], int(sys.argv[2])
buf = b"x" * 4096
for i in range(n):
    fd = os.open("%s/f%d" % (d, i), os.O_WRONLY | os.O_CREAT, 0o644)
    os.write(fd, buf)
    os.fsync(fd)
    os.close(fd)' "$MNT/s$w" "$NSYNC" &
	done
	wait
	t1=$(now)
	for w in $(seq 1 "$p"); do rm -rf "$MNT/s$w"; done
	awk -v ns=$((t1 - t0)) -v n=$((p * NSYNC)) 'BEGIN { printf "%.0f", n / (ns / 1e9) }'
}

# run_side INDEX -> prints the 8 metrics
run_side() {
	local i=$1 type=${TYPE[$1]} opts=${OPTS[$1]} t0 t1 k c cr st un f1 f8 wr rd
	local -a mk
	read -ra mk <<<"${MKFS[$i]}"
	"${mk[@]}" "$DEV" >/dev/null 2>&1 || die "${MKFS[$i]} failed on $DEV"
	mnt "$type" "$opts"

	sync -f "$MNT"
	t0=$(now)
	for k in $(seq 1 "$ITERS"); do
		echo "$k" >"$MNT/c.$k"
		sync -f "$MNT"
	done
	t1=$(now)
	c=$(awk -v ns=$((t1 - t0)) -v n="$ITERS" 'BEGIN { printf "%.2f", ns / 1e6 / n }')

	mkdir "$MNT/d"
	t0=$(now)
	for k in $(seq 1 "$NFILES"); do printf 'x%d\n' "$k" >"$MNT/d/f$k"; done
	sync -f "$MNT"
	t1=$(now)
	cr=$(secs "$t0" "$t1")

	remount "$type" "$opts"
	t0=$(now)
	find "$MNT/d" -type f -printf '%s\n' >/dev/null
	t1=$(now)
	st=$(secs "$t0" "$t1")

	t0=$(now)
	rm -rf "$MNT/d"
	sync -f "$MNT"
	t1=$(now)
	un=$(secs "$t0" "$t1")

	f1=$(fsync_rate 1)
	f8=$(fsync_rate 8)

	t0=$(now)
	dd if=/dev/zero of="$MNT/big" bs=1M count="$SEQ_MB" conv=fsync status=none || die "write failed"
	t1=$(now)
	wr=$(awk -v ns=$((t1 - t0)) -v mb="$SEQ_MB" 'BEGIN { printf "%.1f", mb / (ns / 1e9) }')

	remount "$type" "$opts"
	t0=$(now)
	dd if="$MNT/big" of=/dev/null bs=1M status=none || die "read failed"
	t1=$(now)
	rd=$(awk -v ns=$((t1 - t0)) -v mb="$SEQ_MB" 'BEGIN { printf "%.1f", mb / (ns / 1e9) }')

	umount "$MNT" || die "umount failed"
	echo "$c $cr $st $un $f1 $f8 $wr $rd"
}

RES=$(mktemp)
trap 'rm -f "$RES"' EXIT
printf 'device %s, %d rounds, %d sides, order rotated each round, reformatted every time\n' "$DEV" "$ROUNDS" "$N"
for i in $(seq 0 $((N - 1))); do
	printf '  %-8s module %s, mkfs "%s"%s\n' "${LABEL[$i]}" "${KO[$i]}" "${MKFS[$i]}" "${OPTS[$i]:+, mount -o ${OPTS[$i]}}"
done
printf '%d commits, %d files, %d fsyncs per writer, %d MB per round\n\n' "$ITERS" "$NFILES" "$NSYNC" "$SEQ_MB"
printf '%-5s %-8s %9s %8s %7s %8s %8s %8s %8s %8s\n' round side commit-ms create-s stat-s unlink-s fsync1/s fsync8/s write-MB read-MB
for r in $(seq 1 "$ROUNDS"); do
	for j in $(seq 0 $((N - 1))); do
		i=$(( (j + r - 1) % N ))
		out=$(run_side "$i")
		[ -n "$out" ] || die "round $r side ${LABEL[$i]} failed"
		read -r c cr st un f1 f8 wr rd <<<"$out"
		printf '%-5s %-8s %9s %8s %7s %8s %8s %8s %8s %8s\n' "$r" "${LABEL[$i]}" "$c" "$cr" "$st" "$un" "$f1" "$f8" "$wr" "$rd"
		echo "$i $out" >>"$RES"
	done
done

labels="${LABEL[*]}"
awk -v n_sides="$N" -v labels="$labels" '
function median(a, n,   i, j, t) {
	for (i = 2; i <= n; i++) for (j = i; j > 1 && a[j-1] > a[j]; j--) { t = a[j]; a[j] = a[j-1]; a[j-1] = t }
	return n % 2 ? a[(n+1)/2] : (a[n/2] + a[n/2+1]) / 2
}
{ n[$1]++; for (k = 2; k <= 9; k++) v[$1, k, n[$1]] = $k }
END {
	split(labels, lab, " ")
	split("commit-ms create-s stat-s unlink-s fsync1/s fsync8/s write-MB/s read-MB/s", name, " ")
	split("1 1 1 1 0 0 0 0", lower_better, " ")
	printf "\nmedians; in brackets, relative to %s (+ is better)\n%-11s", lab[1], ""
	for (s = 1; s <= n_sides; s++) printf " %18s", lab[s]
	printf "\n"
	for (k = 2; k <= 9; k++) {
		printf "%-11s", name[k-1]
		for (s = 0; s < n_sides; s++) {
			for (i = 1; i <= n[s]; i++) x[i] = v[s, k, i]
			m[s] = median(x, n[s])
		}
		for (s = 0; s < n_sides; s++) {
			if (s == 0 || m[0] == 0 || m[s] == 0) { printf " %18.2f", m[s]; continue }
			rel = lower_better[k-1] ? m[0] / m[s] - 1 : m[s] / m[0] - 1
			printf " %10.2f [%+4.0f%%]", m[s], rel * 100
		}
		printf "\n"
	}
}' "$RES"

# leave DEV formatted and mounted as the first side
read -ra mk <<<"${MKFS[0]}"
"${mk[@]}" "$DEV" >/dev/null 2>&1 && mnt "${TYPE[0]}" "${OPTS[0]}"
