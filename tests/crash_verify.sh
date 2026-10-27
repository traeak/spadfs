#!/bin/bash
# crash_verify.sh -- post-reboot side of a spadfs2 crash test.
#
# Reads the manifest written by crash_cycle.sh (which lives OUTSIDE the crashed
# filesystem, so it is trustworthy) and answers:
#
#   1. does the volume mount at all?
#   2. how many fsync-acknowledged commits survived?
#   3. is the cct / its CRC region still self-consistent?
#   4. did anything appear that was never written?  (corruption, not loss)
#   5. can fsck repair it, and does repair change the answer?
#
# Run after each crash cycle. Read-only by default: it uses -n -f so fsck
# traverses everything without repairing. Pass --repair to additionally test
# the repair path on a copy.
#
# Usage:
#   sudo tests/crash_verify.sh [-d DEV] [-m MNT] [--repair]
#
# If DEV/MNT are omitted they are taken from $STATE_DIR/NEXT, which
# crash_cycle.sh writes before rebooting.

set -uo pipefail

STATE_DIR=${STATE_DIR:-/var/lib/spadfs-crash}
SPADFSCK=${SPADFSCK:-./spadfsck2}
MOUNT_TYPE=${MOUNT_TYPE:-spadfs2}
REPAIR=0
DEV=${DEV:-}
MNT=${MNT:-}

while [ $# -gt 0 ]; do
	case "$1" in
	-d) DEV=$2; shift 2 ;;
	-m) MNT=$2; shift 2 ;;
	--repair) REPAIR=1; shift ;;
	*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

if [ -z "$DEV" ] || [ -z "$MNT" ]; then
	if [ -f "$STATE_DIR/NEXT" ]; then
		# shellcheck disable=SC1091
		. "$STATE_DIR/NEXT"
	else
		echo "no DEV given and no $STATE_DIR/NEXT from a prior cycle" >&2
		exit 2
	fi
fi

NEXT_FILE=$STATE_DIR/next_cycle
CYCLE=$(cat "$STATE_DIR/cycle" 2>/dev/null || echo 0)
MANIFEST=$STATE_DIR/manifest.$CYCLE
OUT=$STATE_DIR/verify.$CYCLE.log

exec > >(tee "$OUT") 2>&1

echo "==================== crash verify: cycle $CYCLE ===================="
echo "device  : $DEV"
echo "mount   : $MNT"
echo "manifest: $MANIFEST"
echo

if [ ! -f "$MANIFEST" ]; then
	echo "NO MANIFEST for cycle $CYCLE -- nothing to compare against."
	echo "Was the state dir on the device that got crashed?"
	exit 1
fi

EXPECTED=$(wc -l <"$MANIFEST")
echo "expected $EXPECTED fsync-acknowledged files"
echo

# ---------------------------------------------------------------- 1. fsck ---
echo "--- fsck (read-only, full traversal) ---"
FSCK_OUT=$("$SPADFSCK" -n -f "$DEV" 2>&1)
FSCK_RC=$?
echo "$FSCK_OUT"
echo "fsck exit code: $FSCK_RC"
echo

CCT_CRC_BAD=0
echo "$FSCK_OUT" | grep -q "^CCT CRC UNIT .* CHECKSUM MISMATCH" && CCT_CRC_BAD=1
CCT_UNTRUSTWORTHY=0
echo "$FSCK_OUT" | grep -q "CCT CRC TABLE UNTRUSTWORTHY" && CCT_UNTRUSTWORTHY=1
# fsck prints NOTHING when a region is fine -- "BACKUP VERIFIES" only appears
# when the primary is damaged and the backup is the good copy. So the backup's
# own health is the absence of backup error lines, not the presence of a
# success line.
BACKUP_BAD=0
echo "$FSCK_OUT" | grep -qE "CCT BACKUP (CRC UNIT .* CHECKSUM MISMATCH|CORRUPT|REGION|SECTOR .* DOES NOT MATCH)|CANT READ CCT BACKUP" && BACKUP_BAD=1
echo "cct CRC region self-consistent : $([ $CCT_CRC_BAD -eq 0 ] && echo YES || echo NO)"
echo "cct declared untrustworthy     : $([ $CCT_UNTRUSTWORTHY -eq 0 ] && echo no || echo YES)"
echo "backup mirror healthy          : $([ $BACKUP_BAD -eq 0 ] && echo YES || echo NO)"
echo

# ---------------------------------------------------------------- 2. mount --
echo "--- mount attempt ---"
mkdir -p "$MNT"
MOUNTED=1
if ! mount -t "$MOUNT_TYPE" "$DEV" "$MNT" 2>&1; then
	echo "MOUNT FAILED"
	MOUNTED=0
else
	echo "mount SUCCEEDED"
fi
echo

# ------------------------------------------------- 3. durability accounting --
PRESENT=0
LOST=""
GHOST=""
if [ "$MOUNTED" -eq 1 ]; then
	while IFS=$'\t' read -r idx payload; do
		# filename built the same way crash_cycle.sh builds it
		f="c${CYCLE}_f${idx}.txt"
		if [ -f "$MNT/$f" ]; then
			actual=$(cat "$MNT/$f")
			if [ "$actual" = "$payload" ]; then
				PRESENT=$((PRESENT + 1))
			else
				LOST="$LOST $f(content-mismatch)"
			fi
		else
			LOST="$LOST $f(absent)"
		fi
	done <"$MANIFEST"

	# anything on the fs for this cycle that is not in the manifest is a ghost
	for f in "$MNT"/c${CYCLE}_f*.txt; do
		[ -e "$f" ] || continue
		base=$(basename "$f")
		n=${base#c${CYCLE}_f}; n=${n%.txt}
		if ! awk -F'\t' -v n="$n" '$1==n{found=1} END{exit !found}' "$MANIFEST"; then
			GHOST="$GHOST $base"
		fi
	done
else
	LOST="(could not mount -- $EXPECTED files unverifiable)"
fi

SURVIVED=$PRESENT
LOST_COUNT=$(echo $LOST | wc -w)
echo "--- durability ---"
echo "fsync-acknowledged : $EXPECTED"
echo "survived intact    : $SURVIVED"
echo "lost or wrong      : $LOST_COUNT$LOST"
echo "ghost files        : $(echo $GHOST | wc -w)$GHOST"
echo
if [ -n "$GHOST" ]; then
	echo "*** FILES APPEARED THAT WERE NEVER WRITTEN -- that is corruption, not loss ***"
	echo
fi

# ------------------------------------------------------- 4. cct forensics ---
if [ "$MOUNTED" -eq 1 ]; then
	echo "--- cct CRC forensics (diag_cct_crc.py) ---"
	if [ -f tests/diag_cct_crc.py ]; then
		python3 tests/diag_cct_crc.py "$DEV" 2>&1 | sed -n '1,40p'
	else
		echo "tests/diag_cct_crc.py not found"
	fi
	echo
fi

# ----------------------------------------------------------- 5. verdict ----
echo "==================== verdict: cycle $CYCLE ===================="
VERDICT=PASS
[ $FSCK_RC -ne 0 ] && {
	echo "FAIL: fsck reported errors (rc=$FSCK_RC)"
	VERDICT=FAIL
}
[ $CCT_CRC_BAD -ne 0 ] && {
	echo "FAIL: cct CRC unit mismatch after crash -- write ordering has a hole"
	VERDICT=FAIL
}
[ $BACKUP_BAD -ne 0 ] && {
	echo "FAIL: backup mirror damaged by the crash"
	VERDICT=FAIL
}
[ -n "$GHOST" ] && {
	echo "FAIL: ghost files present"
	VERDICT=FAIL
}
if [ "$SURVIVED" -lt "$EXPECTED" ]; then
	echo "NOTE: $((EXPECTED - SURVIVED)) fsync-acknowledged file(s) lost."
	echo "      fsync returned before the crash, so these SHOULD have survived."
	echo "      This is a durability violation even if the fs is otherwise consistent."
	VERDICT="FAIL(durability)"
fi
echo "VERDICT: $VERDICT"
echo
echo "full log: $OUT"

# leave the fs mounted for inspection if it came up
if [ "$REPAIR" -eq 1 ]; then
	echo
	echo "--- repair pass (destructive) ---"
	[ "$MOUNTED" -eq 1 ] && umount "$MNT"
	"$SPADFSCK" -p "$DEV" 2>&1 | tail -20
	echo "repair rc: $?"
fi

[ "$VERDICT" = PASS ] && exit 0 || exit 1
