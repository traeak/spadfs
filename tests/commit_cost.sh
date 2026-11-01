#!/bin/bash
# commit_cost.sh -- Task 3.4: measure what the as-built cct mirror costs per
# commit, against the pre-3.3 baseline recorded on the same device.
#
# Pre-3.3 baseline (tests/HARDWARE-NOTES.md, first recorded in 71c6021):
#   100 x (echo > file; sync)  ->  2.687 s  =  ~27 ms per commit
#
# As built, 3.3 adds per commit: one mirror cct block, one stamp unit, and one
# extra flush (primary data + CRC -> flush -> mirror -> flush). This script
# reruns the exact baseline workload and also reads the device's own
# sectors-written counter, so the result is both a latency and a write volume.
#
# Usage:
#   sudo tests/commit_cost.sh [-d DEV] [-m MNT] [-n ITERS]
#
# MNT must already hold a mounted spadfs2 volume on DEV (tests/mount_smoke.sh
# or crash_cycle.sh leave one at /mnt/spadcrash). Writes $MNT/commitcost.* and
# removes them. Read-only with respect to everything else.

set -uo pipefail

DEV=${DEV:-/dev/sda1}
MNT=${MNT:-/mnt/spadcrash}
ITERS=${ITERS:-100}
BASELINE_MS=${BASELINE_MS:-27}
MOUNT_TYPE=${MOUNT_TYPE:-spadfs2}

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DEV=$2; shift 2 ;;
	-m) MNT=$2; shift 2 ;;
	-n) ITERS=$2; shift 2 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

fstype=$(findmnt -n -o FSTYPE -S "$DEV" 2>/dev/null | head -1)
target=$(findmnt -n -o TARGET -S "$DEV" 2>/dev/null | head -1)
if [ "$fstype" != "$MOUNT_TYPE" ] || [ "$target" != "$MNT" ]; then
	echo "commit_cost: $DEV is not a $MOUNT_TYPE mount at $MNT (found '$fstype' at '$target')" >&2
	exit 1
fi

# Field 7 of the block stat file is sectors written, in 512-byte units,
# regardless of the device's logical block size.
stat_file=/sys/class/block/$(basename "$(readlink -f "$DEV")")/stat
[ -r "$stat_file" ] || { echo "commit_cost: cannot read $stat_file" >&2; exit 1; }
sectors_written() { awk '{print $7}' "$stat_file"; }

# Settle anything already dirty so it is not billed to the first commit.
sync

w0=$(sectors_written)
t0=$(date +%s%N)
for i in $(seq 1 "$ITERS"); do
	echo "$i" >"$MNT/commitcost.$i"
	sync
done
t1=$(date +%s%N)
w1=$(sectors_written)

rm -f "$MNT"/commitcost.*
sync

awk -v ns=$((t1 - t0)) -v n="$ITERS" -v sec=$((w1 - w0)) -v base="$BASELINE_MS" 'BEGIN {
	ms = ns / 1e6 / n
	kib = sec * 512 / 1024 / n
	printf "commits            : %d\n", n
	printf "total              : %.3f s\n", ns / 1e9
	printf "per commit         : %.1f ms   (pre-3.3 baseline %d ms, %+.0f%%)\n", ms, base, (ms - base) * 100 / base
	printf "written per commit : %.1f KiB  (device counter, includes the data block)\n", kib
}'
