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
# With --reboot it first runs reboot_preflight (crash_pstore.sh) and refuses
# to start unless: the panic capture path is live on this boot, the next boot
# is re-armed onto the test entry, and an ARMED record for this cycle is
# durable. The one-shot GRUB entry is used up by the boot that got us here,
# so without re-arming, the reboot after this cycle -- planned or a real
# panic -- would land on the default entry and nothing could be verified.
#
# Post-reboot, run tests/crash_verify.sh.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$TESTS_DIR/lib_crash_attr.sh"
# shellcheck disable=SC1091
. "$TESTS_DIR/crash_pstore.sh"

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
# The counter is written only after the preflight below, so a refused
# --reboot does not burn a cycle number.

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

if [ "$REBOOT" -eq 1 ]; then
	if ! reboot_preflight "$CYCLE_DIR" "$CYCLE"; then
		echo "REFUSING: crash cycle $CYCLE not started -- see above" >&2
		exit 1
	fi
	echo "[cycle $CYCLE] preflight ok: capture live, next boot armed, ARMED recorded"
fi
echo "$CYCLE" >"$CYCLE_DIR/cycle"

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

# The marker-before-trigger ordering is baked into planned_reboot so it can be
# tested without rebooting anything (I7). If the marker cannot be made
# durable, the trigger is never touched and we do not reboot.
#
# SYSRQ_TRIGGER is injectable; in production it is /proc/sysrq-trigger.
SYSRQ_TRIGGER=${SYSRQ_TRIGGER:-/proc/sysrq-trigger}

sync 2>/dev/null || true
if ! planned_reboot "$CYCLE_DIR" "$CYCLE" "$DEV" "$SYSRQ_TRIGGER"; then
	log "ABORT: no durable PLANNED marker -- NOT rebooting"
	log "the filesystem is left mounted; fix $CYCLE_DIR and re-run"
	exit 1
fi
log "PLANNED marker fsynced (cycle $CYCLE on $DEV); trigger pulled"

# If we get here the reboot did not happen.
log "sysrq-trigger returned without rebooting -- aborting, state left mounted"
exit 1
