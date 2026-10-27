#!/bin/bash
# mirror_cost.sh -- Task 3.4: measure what a synchronous cct mirror costs.
#
# Task 3.3 would make every commit also write the cct backup mirror. The cct is
# 256 KiB (CCT_SIZE = 65536*4), so a full-sync mirror (option 3A) turns a
# commit that today writes ~4.6 KiB into one that writes ~265 KiB.
#
#   current commit : 1 cct sector (512 B) + 1 CRC block (4096 B)  =   4608 B
#   with 3A mirror : + 512 cct sectors (262144 B) + CRC block      = 266240 B
#
# That is a 58x increase in per-commit write volume. Whether that is
# acceptable is the whole 3A-vs-3B question, and it should be answered with a
# measurement rather than a guess.
#
# The kernel does not write the mirror yet, so this measures at the block/file
# layer: the cost of making N bytes durable on this device, at the same fsync
# granularity a commit uses.
#
# Usage:
#   sudo tests/mirror_cost.sh [-d DEV] [-m MNT] [-n ITERS]
#
# Run on the mounted spadfs2 volume. Writes to $MNT/costbench.* and removes
# them. Does not touch metadata directly; it measures the device, which is what
# the mirror would have to pay for.

set -uo pipefail

DEV=${DEV:-/dev/sda1}
MNT=${MNT:-/mnt/spadcrash}
ITERS=${ITERS:-50}
MOUNT_TYPE=${MOUNT_TYPE:-spadfs2}

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DEV=$2; shift 2 ;;
	-m) MNT=$2; shift 2 ;;
	-n) ITERS=$2; shift 2 ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

if ! mountpoint -q "$MNT"; then
	echo "mounting $DEV at $MNT"
	mkdir -p "$MNT"
	mount -t "$MOUNT_TYPE" "$DEV" "$MNT" || { echo "mount failed" >&2; exit 1; }
fi

bench() {
	local label=$1 bytes=$2
	local f="$MNT/costbench.$$"
	# warm: create the inode once so we measure data durability, not allocation
	dd if=/dev/zero of="$f" bs=1 count=1 conv=fsync status=none 2>/dev/null

	local t0 t1
	t0=$(date +%s.%N)
	for _ in $(seq 1 "$ITERS"); do
		dd if=/dev/zero of="$f" bs="$bytes" count=1 conv=notrunc,fsync status=none
	done
	t1=$(date +%s.%N)

	local total per
	total=$(echo "$t1 $t0" | awk '{printf "%.3f", $1-$2}')
	per=$(echo "$total $ITERS" | awk '{printf "%.2f", $1*1000/$2}')
	printf '%-28s %8s B  %7s s total  %6s ms/commit\n' \
		"$label" "$bytes" "$total" "$per"
	echo "$per"
	rm -f "$f"
}

echo "==================== Task 3.4: mirror write cost ===================="
echo "device: $DEV   iterations: $ITERS   each write fsynced"
echo

# Capture the numbers without the printf noise polluting the variable.
base_line=$(bench "current commit payload" 4608 | tail -1)
mirr_line=$(bench "with 3A full mirror" 266240 | tail -1)
small_line=$(bench "1 sector only (floor)" 512 | tail -1)

echo
echo "--- derived ---"
awk -v b="$base_line" -v m="$mirr_line" -v s="$small_line" 'BEGIN {
	printf "  floor (512 B fsync)          : %6.2f ms\n", s
	printf "  current commit (4.6 KiB)     : %6.2f ms\n", b
	printf "  with full mirror (265 KiB)   : %6.2f ms\n", m
	printf "  mirror adds                  : %6.2f ms  (%.1fx)\n", m-b, (m-b)/b
	printf "  mirror as %% of commit        : %6.1f%%\n", (m-b)/m*100
}'
echo
echo "decision guidance:"
echo "  if the mirror delta is small relative to the commit, 3A (full sync"
echo "  mirror) is affordable and simplest -- the backup is always fresh."
echo "  if the delta dominates, prefer 3B (checkpointed mirror + epoch stamp),"
echo "  which pays the 256 KiB only every K commits and gates restore on the"
echo "  epoch so a stale mirror can never silently promote an older alternate."
echo
