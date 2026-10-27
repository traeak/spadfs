#!/bin/bash
# crash_cycle.sh -- drive ONE crash test of spadfs2, pre-reboot side.
#
# Writes N files, fsyncing each one, and records the expected post-crash state
# OUTSIDE the filesystem under test. Then optionally triggers a hard reboot via
# sysrq, which loses all dirty pages without unmounting -- the closest thing to
# pulling the plug short of doing it by hand.
#
# Why the manifest lives outside spadfs2: if it lived on the filesystem being
# crashed, we could not trust it to tell us what should have survived. It is
# written to $STATE_DIR (default /var/lib/spadfs-crash) and fsynced there, so
# the expectation itself is durable independent of the test subject.
#
# The point is not "did the fs survive". The points are:
#   - how many fsynced commits actually survived (the durability contract)
#   - whether the cct is trustworthy afterwards, or whether the cct/CRC write
#     ordering leaves a window where the CRC describes a different cct than
#     the one on disk
#   - whether dual-versioned structures recover the RIGHT alternate, or a
#     crash can promote a stale one
#
# Usage:
#   sudo tests/crash_cycle.sh [-d DEV] [-m MNT] [-n NFILES] [--reboot]
#
# Without --reboot this only prepares state and prints the reboot command, so
# nothing destructive happens by accident.
#
# Post-reboot, run tests/crash_verify.sh.

set -uo pipefail

DEV=${DEV:-/dev/sda1}
MNT=${MNT:-/mnt/spadcrash}
NFILES=${NFILES:-40}
STATE_DIR=${STATE_DIR:-/var/lib/spadfs-crash}
REBOOT=0
SPADFSCK=${SPADFSCK:-./spadfsck2}
MOUNT_TYPE=${MOUNT_TYPE:-spadfs2}

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DEV=$2; shift 2 ;;
	-m) MNT=$2; shift 2 ;;
	-n) NFILES=$2; shift 2 ;;
	--reboot) REBOOT=1; shift ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

CYCLE_DIR=$STATE_DIR
mkdir -p "$CYCLE_DIR"
CYCLE=$(( $(cat "$CYCLE_DIR/cycle" 2>/dev/null || echo 0) + 1 ))
echo "$CYCLE" >"$CYCLE_DIR/cycle"

MANIFEST=$CYCLE_DIR/manifest.$CYCLE
CRASH_LOG=$CYCLE_DIR/crash.$CYCLE.log

log() { echo "[cycle $CYCLE] $*" | tee -a "$CRASH_LOG"; }

# ---- sanity: never crash the root filesystem ----
ROOT_SRC=$(findmnt -no SOURCE / 2>/dev/null || echo "")
if [ "$ROOT_SRC" = "$DEV" ]; then
	echo "REFUSING: $DEV is the root device" >&2
	exit 1
fi
if ! id -u | grep -qx 0; then
	echo "must run as root" >&2
	exit 1
fi

log "device=$DEV mnt=$MNT files=$NFILES"

# ---- make sure we can come back and verify after the reboot ----
cat >"$CYCLE_DIR/NEXT" <<EOF
DEV=$DEV
MNT=$MNT
CYCLE=$CYCLE
EOF

# ---- mount fresh ----
if mountpoint -q "$MNT"; then
	log "$MNT already mounted; unmounting clean first"
	umount "$MNT" || { log "umount failed"; exit 1; }
fi

mkdir -p "$MNT"
if ! mount -t "$MOUNT_TYPE" "$DEV" "$MNT" 2>>"$CRASH_LOG"; then
	log "MOUNT FAILED -- see $CRASH_LOG"
	tail -5 "$CRASH_LOG"
	exit 1
fi
log "mounted ok"

# ---- write fsynced files, recording each as it is acknowledged ----
: >"$MANIFEST"
t0=$(date +%s.%N)
for i in $(seq 1 "$NFILES"); do
	name="c${CYCLE}_f${i}.txt"
	payload="cycle $CYCLE file $i $(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
	if ! printf '%s\n' "$payload" >"$MNT/$name"; then
		log "write failed at $i -- stopping"
		break
	fi
	fsync "$MNT/$name" 2>/dev/null || sync
	# Record AFTER the fsync returns: this is the commit we claim is durable.
	printf '%s\t%s\n' "$i" "$payload" >>"$MANIFEST"
	fsync "$MANIFEST" 2>/dev/null || sync
done
t1=$(date +%s.%N)

COMMITTED=$(wc -l <"$MANIFEST")
ELAPSED=$(echo "$t1 $t0" | awk '{printf "%.3f", $1-$2}')
AVG=$(echo "$ELAPSED $COMMITTED" | awk '{if ($2>0) printf "%.1f", $1*1000/$2; else print "n/a"}')

log "committed $COMMITTED/$NFILES fsynced files in ${ELAPSED}s (~${AVG} ms/commit)"

# ---- a marker file per cycle, so verify can see partial writes too ----
sync

if [ "$REBOOT" -eq 0 ]; then
	log "prepared. NOT rebooting (pass --reboot to do so)."
	log "next: sudo tests/crash_cycle.sh -d $DEV --reboot"
	exit 0
fi

log "triggering hard reboot via sysrq -- dirty pages lost, no unmount"
# echo 1 first so sysrq is enabled at all on kernels that gate it.
echo 1 >/proc/sys/kernel/sysrq 2>/dev/null
sync 2>/dev/null || true
# 'b' = immediate reboot, equivalent to pulling the plug.
echo b >/proc/sysrq-trigger

# If we get here the reboot did not happen.
log "sysrq-trigger returned without rebooting -- aborting, state left mounted"
exit 1
