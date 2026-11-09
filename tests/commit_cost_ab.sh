#!/usr/bin/env bash
# A/B commit cost of two spadfs2 modules on the same device. RUN AS ROOT.
#
# commit_cost.sh times `echo > f; sync` once. Two problems made its numbers
# untrustworthy for comparing commit protocols:
#   - `sync` flushes EVERY filesystem on the machine, so the root
#     filesystem's journal and whatever else is dirty land in each "commit"
#   - single runs on different days: 27 ms (1 flush), 56.7 ms (3 flushes)
#     and then 72.2 ms with one flush REMOVED -- noise, not signal
#
# This script measures both modules in one sitting, alternating A and B
# round by round so drift (thermal, background I/O, the disk's own cache
# state) lands on both, and commits with syncfs on the spadfs volume only.
# Prints every round, then the median and spread per module and the
# difference of medians.
#
# The device is remounted with each module in turn (umount, rmmod, insmod,
# mount), exactly as the other root tests do, and is left mounted with B.
#
# Usage:
#   sudo bash tests/commit_cost_ab.sh [-d DEV] [-m MNT] [-n ITERS] [-r ROUNDS]
#        [--mkfs-a MKFS --mkfs-b MKFS] KO_A KO_B
#
# --mkfs-a/--mkfs-b compare two on-disk LAYOUTS rather than (or as well as)
# two modules: before each round DEV is reformatted with that side's
# mkspadfs2. THIS ERASES DEV every round -- use a scratch partition only.
# Give the same module twice to compare layouts alone.
#
# DEV must currently be a mounted spadfs2 volume at MNT (defaults as in
# commit_cost.sh). Writes $MNT/commitcost.* and removes them.
#
# Example -- old layout vs new (backups at the end), same module:
#   git worktree add /tmp/spadfs-head HEAD && make -C /tmp/spadfs-head mkspadfs2
#   sudo bash tests/commit_cost_ab.sh --mkfs-a /tmp/spadfs-head/mkspadfs2 \
#        --mkfs-b ./mkspadfs2 ./spadfs2.ko ./spadfs2.ko
#
# Example -- before vs after dropping the mirror flush:
#   git worktree add /tmp/spadfs-3flush HEAD
#   make -C /lib/modules/$(uname -r)/build M=/tmp/spadfs-3flush \
#        STANDALONE_BUILD=y CONFIG_SPADFS_FS=m SPADFS_NAME=spadfs2
#   sudo bash tests/commit_cost_ab.sh /tmp/spadfs-3flush/spadfs2.ko ./spadfs2.ko

set -uo pipefail

DEV=${DEV:-/dev/sda1}
MNT=${MNT:-/mnt/spadcrash}
ITERS=${ITERS:-100}
ROUNDS=${ROUNDS:-5}
MKFS_A=""
MKFS_B=""

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DEV=$2; shift 2 ;;
	-m) MNT=$2; shift 2 ;;
	-n) ITERS=$2; shift 2 ;;
	-r) ROUNDS=$2; shift 2 ;;
	--mkfs-a) MKFS_A=$(readlink -f "$2"); shift 2 ;;
	--mkfs-b) MKFS_B=$(readlink -f "$2"); shift 2 ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done
[ $# -eq 2 ] || { echo "usage: $0 [-d DEV] [-m MNT] [-n ITERS] [-r ROUNDS] KO_A KO_B" >&2; exit 2; }
KO_A=$(readlink -f "$1")
KO_B=$(readlink -f "$2")

[ "$(id -u)" = 0 ] || { echo "commit_cost_ab: needs root" >&2; exit 1; }
for ko in "$KO_A" "$KO_B"; do
	[ -r "$ko" ] || { echo "commit_cost_ab: no module at $ko" >&2; exit 1; }
done
if [ -n "$MKFS_A$MKFS_B" ]; then
	{ [ -x "$MKFS_A" ] && [ -x "$MKFS_B" ]; } ||
		{ echo "commit_cost_ab: --mkfs-a and --mkfs-b must both name an mkspadfs2" >&2; exit 2; }
fi
fstype=$(findmnt -n -o FSTYPE -S "$DEV" 2>/dev/null | head -1)
target=$(findmnt -n -o TARGET -S "$DEV" 2>/dev/null | head -1)
if [ "$fstype" != spadfs2 ] || [ "$target" != "$MNT" ]; then
	echo "commit_cost_ab: $DEV is not a spadfs2 mount at $MNT (found '$fstype' at '$target')" >&2
	exit 1
fi
OPTS=$(findmnt -n -o OPTIONS -S "$DEV" | head -1)

stat_file=/sys/class/block/$(basename "$(readlink -f "$DEV")")/stat
sectors_written() { awk '{print $7}' "$stat_file"; }

# use_module KO [MKFS]: switch to KO; with MKFS, reformat DEV first
use_module() {
	umount "$MNT" || { echo "commit_cost_ab: cannot unmount $MNT" >&2; exit 1; }
	if [ -n "${2:-}" ]; then
		"$2" --no-trim "$DEV" >/dev/null 2>&1 ||
			{ echo "commit_cost_ab: $2 failed on $DEV" >&2; exit 1; }
	fi
	if [ -e /sys/module/spadfs2 ]; then
		rmmod spadfs2 || { echo "commit_cost_ab: cannot unload spadfs2" >&2; exit 1; }
	fi
	insmod "$1" || { echo "commit_cost_ab: insmod $1 failed" >&2; exit 1; }
	mount -t spadfs2 -o "$OPTS" "$DEV" "$MNT" ||
		mount -t spadfs2 "$DEV" "$MNT" ||
		{ echo "commit_cost_ab: cannot remount $DEV" >&2; exit 1; }
}

# one round: ms per commit and KiB written per commit
measure() {
	local i t0 t1 w0 w1
	sync -f "$MNT"
	w0=$(sectors_written)
	t0=$(date +%s%N)
	for i in $(seq 1 "$ITERS"); do
		echo "$i" >"$MNT/commitcost.$i"
		sync -f "$MNT"
	done
	t1=$(date +%s%N)
	w1=$(sectors_written)
	rm -f "$MNT"/commitcost.*
	sync -f "$MNT"
	awk -v ns=$((t1 - t0)) -v n="$ITERS" -v sec=$((w1 - w0)) \
		'BEGIN { printf "%.2f %.1f\n", ns / 1e6 / n, sec * 512 / 1024 / n }'
}

RES=$(mktemp)
trap 'rm -f "$RES"' EXIT

printf 'device %s, %d commits per round, %d rounds per module, alternating\n' "$DEV" "$ITERS" "$ROUNDS"
printf 'A = %s %s\nB = %s %s\n\n' "$KO_A" "${MKFS_A:+(mkfs $MKFS_A)}" "$KO_B" "${MKFS_B:+(mkfs $MKFS_B)}"
printf '%-6s %-3s %10s %12s\n' round mod 'ms/commit' 'KiB/commit'
for r in $(seq 1 "$ROUNDS"); do
	for m in A B; do
		ko=$KO_A mk=$MKFS_A
		[ "$m" = B ] && ko=$KO_B mk=$MKFS_B
		use_module "$ko" "$mk"
		read -r ms kib < <(measure)
		printf '%-6s %-3s %10s %12s\n' "$r" "$m" "$ms" "$kib"
		printf '%s %s %s\n' "$m" "$ms" "$kib" >>"$RES"
	done
done

awk '
function median(a, n,   i, j, t) {
	for (i = 2; i <= n; i++) for (j = i; j > 1 && a[j-1] > a[j]; j--) { t = a[j]; a[j] = a[j-1]; a[j-1] = t }
	return n % 2 ? a[(n+1)/2] : (a[n/2] + a[n/2+1]) / 2
}
{ n[$1]++; v[$1, n[$1]] = $2; k[$1] += $3 }
END {
	for (m in n) {
		for (i = 1; i <= n[m]; i++) { x[i] = v[m, i]; if (i == 1 || x[i] < lo[m]) lo[m] = x[i]; if (i == 1 || x[i] > hi[m]) hi[m] = x[i] }
		med[m] = median(x, n[m])
		printf "\n%s: median %.1f ms/commit (range %.1f-%.1f), %.1f KiB/commit", m, med[m], lo[m], hi[m], k[m] / n[m]
	}
	printf "\n\nB - A = %+.1f ms/commit (%+.0f%%)\n", med["B"] - med["A"], (med["B"] - med["A"]) * 100 / med["A"]
	if (hi["A"] >= lo["B"] && hi["B"] >= lo["A"])
		print "ranges overlap: the difference is within round-to-round noise"
}' "$RES"
