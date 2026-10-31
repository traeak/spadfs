#!/usr/bin/env bash
# Phase 3.2: crash-count table backup recovery matrix.
#
# Task 3.1 put a mirror of the cct on disk. This proves fsck can act on it:
#
#   primary | backup    | expected action
#   --------+-----------+-----------------------------------------------
#   good    | good      | nothing
#   good    | corrupt   | recreate the mirror from the primary
#   corrupt | good      | restore the primary from the mirror
#   corrupt | corrupt   | explicit destructive-path warning, fall to reset
#   corrupt | absent    | no recovery available
#
# WHY THE VOLUME IS PRIMED FIRST
#
# A freshly formatted cct is all zeros, so "corrupt the primary" and "restore
# from backup" would both be no-ops against an empty table and the test would
# pass without exercising anything. prime.py writes a plausible post-mount
# state (cc=4 with real transaction counts) so there is something to lose.
#
# WHY THE MIRROR IS RE-SYNCED AFTER PRIMING
#
# prime.py updates the primary and its CRC region only. The mirror left by
# mkspadfs is all zeros with CRCs that validly describe those zeros -- so it
# verifies as GOOD while being completely stale. That is not a fixture bug, it
# is the central finding of this task and is asserted explicitly in
# case_stale_backup_verifies below: verification proves a copy is internally
# consistent, never that it is current. Task 3.3 must add an epoch stamp to the
# mirror for the same reason.
#
# Runs without root.

set -uo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

SIZE="${TEST_IMAGE_SIZE:-16M}"

# --- helpers -------------------------------------------------------------

# field_sector <img> <field>
field_sector() {
	python3 -c "
import sys; sys.path.insert(0,'$TESTS_DIR')
from sbinfo import read_super
print(read_super('$1')['$2'])"
}

# corrupt <img> <field> [byte_within_region] -- flip bits in the DATA of a
# region, leaving its own trailer alone so the damage is caught by the layer
# above rather than by the unit seal.
corrupt() {
	local img="$1" field="$2" off="${3:-16}"
	python3 - "$img" "$field" "$off" "$TESTS_DIR" <<'PY'
import sys
img, field, off, tdir = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
sys.path.insert(0, tdir)
from sbinfo import read_super, SECTOR
sec = read_super(img)[field]
with open(img, 'r+b') as f:
    p = sec * SECTOR + off
    f.seek(p)
    b = f.read(1)
    f.seek(p)
    f.write(bytes([b[0] ^ 0xFF]))
print(f"corrupted 1 byte in {field} at sector {sec} +{off}", file=sys.stderr)
PY
}

# run fsck, capture output and rc without tripping set -e
run_fsck() {
	local img="$1"; shift
	FSCK_OUT="$WORKDIR/out.txt"
	"$FSCK" "$@" "$img" >"$FSCK_OUT" 2>&1
	FSCK_RC=$?
}

# assert_contains <name> <needle>
assert_contains() {
	if grep -qiF -- "$2" "$FSCK_OUT"; then
		ok "$1"
	else
		bad "$1 -- expected to see: $2"
		printf '       ---- fsck output ----\n'
		sed 's/^/       /' "$FSCK_OUT" | tail -25
	fi
}

assert_not_contains() {
	if grep -qiF -- "$2" "$FSCK_OUT"; then
		bad "$1 -- did NOT expect: $2"
	else
		ok "$1"
	fi
}

# stamp_is <desc> <img> <expected> -- compare cctstamp.py's one-line summary
stamp_is() {
	local desc="$1" img="$2" want="$3" got
	got=$(python3 -I "$TESTS_DIR/cctstamp.py" show "$img" | tr '\n' ' ')
	got=${got% }
	[ "$got" = "$want" ] && ok "$desc" || bad "$desc" "want '$want', got '$got'"
}

# --- build a volume whose mirror is actually current ------------------------
#
# Corrupt the mirror, then let fsck's own recreate path rebuild it from the
# primed primary. Using the feature to set up the feature also confirms the
# recreate path works before we rely on it for the other cases.
make_synced_volume() {
	local dst="$1"
	make_image "$BASE_TMP" "$SIZE" "--checksums" || return 1
	python3 "$TESTS_DIR/prime.py" "$BASE_TMP" --cc 4 --base-txc 1388 >&2 || return 1
	corrupt "$BASE_TMP" cct_backup 8 || return 1
	# primary good + backup bad -> recreate, auto-answered by -y (not Q_IMPORTANT)
	run_fsck "$BASE_TMP" -f -y
	grep -qiF "CCT BACKUP RECREATED" "$FSCK_OUT" || return 1
	cp -f "$BASE_TMP" "$dst"
}

BASE_TMP="$WORKDIR/rec-base.img"

log ""
log "=== Phase 3.2: cct backup recovery matrix ==="

# --------------------------------------------------------------------------
# case: a stale mirror verifies as good (the finding, asserted not assumed)
# --------------------------------------------------------------------------
case_stale_backup_verifies() {
	local img="$WORKDIR/stale.img"
	make_image "$img" "$SIZE" "--checksums" || { bad "stale: make_image"; return; }
	python3 "$TESTS_DIR/prime.py" "$img" --cc 4 --base-txc 1388 >&2 || { bad "stale: prime"; return; }

	local p
	p=$(python3 -c "
import sys; sys.path.insert(0,'$TESTS_DIR')
from sbinfo import read_super
d=read_super('$img')
f=open('$img','rb')
f.seek(d['cct']*512); prim=f.read(512*512)
f.seek(d['cct_backup']*512); bak=f.read(512*512)
print('DIFFER' if prim!=bak else 'SAME')")

	if [ "$p" = "DIFFER" ]; then
		ok "primed primary and mkfs mirror actually differ"
	else
		bad "expected primed primary to differ from the mirror"
	fi

	run_fsck "$img" -f -y
	# The mirror is stale but internally consistent: verification alone says
	# nothing about currency, so it is NOT corrupt. Task 3.3's comparison and
	# stamp are what notice it, and the primary is intact, so -y refreshes it.
	assert_not_contains "stale mirror is NOT reported corrupt" \
		"CCT BACKUP CORRUPT"
	assert_not_contains "stale mirror triggers no restore" \
		"RESTORE CRASH COUNT TABLE FROM BACKUP"
	assert_contains "stale mirror is reported out of date" \
		"CCT BACKUP IS OUT OF DATE"
	assert_contains "stale mirror is refreshed from the intact primary" \
		"CCT BACKUP REFRESHED"
	stamp_is "refreshed mirror matches the primary, stamped with the volume's cc" \
		"$img" "magic=ok stamp_cc=4 stamp_txc=1776 sealed=yes tx_cc=4 primary_txc=1776 mirror=same"
	run_fsck "$img" -f -n
	assert_not_contains "second pass finds the mirror current" \
		"CCT BACKUP IS OUT OF DATE"
}

# --------------------------------------------------------------------------
# case: mkspadfs stamps the mirror from birth
# --------------------------------------------------------------------------
case_fresh_stamped() {
	local img="$WORKDIR/fresh.img"
	make_image "$img" "$SIZE" "--checksums" || { bad "fresh: make_image"; return; }
	stamp_is "fresh volume: mirror stamped cc 0 and identical to the primary" \
		"$img" "magic=ok stamp_cc=0 stamp_txc=0 sealed=yes tx_cc=0 primary_txc=0 mirror=same"
	run_fsck "$img" -f -n
	assert_not_contains "fresh volume: fsck does not call the mirror out of date" \
		"CCT BACKUP IS OUT OF DATE"
}

# --------------------------------------------------------------------------
# case 1: both good
# --------------------------------------------------------------------------
case_both_good() {
	local img="$WORKDIR/c1.img"
	make_synced_volume "$img" || { bad "case1 setup"; return; }
	run_fsck "$img" -f -y
	assert_not_contains "1: no restore offered when both are good" \
		"RESTORE CRASH COUNT TABLE FROM BACKUP"
	assert_not_contains "1: no recreate offered when both are good" \
		"RECREATE CCT BACKUP"
	assert_not_contains "1: no backup corruption reported" \
		"CCT BACKUP CORRUPT"
}

# --------------------------------------------------------------------------
# case 2: primary corrupt, backup current -- -y restores it
# --------------------------------------------------------------------------
case_restore_current() {
	local img="$WORKDIR/c2.img"
	make_synced_volume "$img" || { bad "case2 setup"; return; }
	corrupt "$img" cct 20 || { bad "case2 corrupt"; return; }

	# The mirror is stamped with the volume's own crash count, so restoring
	# it is at most a rollback of the commit in progress -- what a crash
	# does anyway. That is safe unattended, so -y answers it (Task 3.3).
	run_fsck "$img" -f -y
	assert_contains "2: fsck detects the damaged primary" \
		"CRASH COUNT TABLE CORRUPT"
	assert_contains "2: fsck recognises the backup as a valid source" \
		"ITS BACKUP VERIFIES"
	assert_contains "2: the backup is recognised as current" \
		"THE BACKUP IS FROM THE CURRENT CRASH EPOCH"
	assert_contains "2: -y restores a current backup" \
		"CRASH COUNT TABLE RESTORED FROM BACKUP"
	assert_not_contains "2: the pass is not abandoned" \
		"remains untrusted"
	stamp_is "2: restored primary matches the mirror" \
		"$img" "magic=ok stamp_cc=4 stamp_txc=1776 sealed=yes tx_cc=4 primary_txc=1776 mirror=same"
	run_fsck "$img" -f -n
	[ "$FSCK_RC" -eq 0 ] && ok "2: restored volume checks clean (rc=0)" \
		|| bad "2: restored volume rc=$FSCK_RC" "$(tail -5 "$FSCK_OUT")"
}

# --------------------------------------------------------------------------
# case 2b: primary corrupt, backup from an older epoch -- -y must refuse
# --------------------------------------------------------------------------
case_restore_stale_gated() {
	local img="$WORKDIR/c2b.img"
	make_image "$img" "$SIZE" "--checksums" || { bad "case2b make_image"; return; }
	# primed to cc 4, mirror left as mkfs wrote it: stamped cc 0
	python3 "$TESTS_DIR/prime.py" "$img" --cc 4 --base-txc 1388 >&2 || { bad "case2b prime"; return; }
	corrupt "$img" cct 20 || { bad "case2b corrupt"; return; }

	run_fsck "$img" -f -y
	assert_contains "2b: the backup is identified as from an older epoch" \
		"THE BACKUP IS FROM CRASH COUNT 0 BUT THE VOLUME IS AT 4"
	assert_contains "2b: the reversion risk is stated" \
		"SILENTLY REVERT"
	assert_not_contains "2b: -y does not restore a stale backup" \
		"CRASH COUNT TABLE RESTORED FROM BACKUP"
	local still_bad
	still_bad=$(python3 -c "
import sys; sys.path.insert(0,'$TESTS_DIR')
from sbinfo import read_super
d=read_super('$img')
f=open('$img','rb')
f.seek(d['cct']*512+20); a=f.read(1)
f.seek(d['cct_backup']*512+20); b=f.read(1)
print('DIFFER' if a!=b else 'SAME')")
	[ "$still_bad" = "DIFFER" ] \
		&& ok "2b: primary left untouched under -y (still differs from mirror)" \
		|| bad "2b: primary was modified despite -y gating the restore"
}

# --------------------------------------------------------------------------
# case 2c: primary corrupt, backup without a valid stamp -- -y must refuse
# --------------------------------------------------------------------------
case_restore_unstamped_gated() {
	local img="$WORKDIR/c2c.img"
	make_synced_volume "$img" || { bad "case2c setup"; return; }
	python3 -I "$TESTS_DIR/cctstamp.py" set "$img" magic 0 || { bad "case2c unstamp"; return; }
	corrupt "$img" cct 20 || { bad "case2c corrupt"; return; }

	run_fsck "$img" -f -y
	assert_contains "2c: a backup without a stamp is called unprovable" \
		"NO VALID EPOCH STAMP"
	assert_not_contains "2c: -y does not restore an unstamped backup" \
		"CRASH COUNT TABLE RESTORED FROM BACKUP"
}

# --------------------------------------------------------------------------
# case 2d: a stamp that disagrees with the mirror's data is not trusted
# --------------------------------------------------------------------------
case_stamp_mismatch() {
	local img="$WORKDIR/c2d.img"
	make_synced_volume "$img" || { bad "case2d setup"; return; }
	python3 -I "$TESTS_DIR/cctstamp.py" set "$img" txc 1777 || { bad "case2d restamp"; return; }
	corrupt "$img" cct 20 || { bad "case2d corrupt"; return; }

	run_fsck "$img" -f -y
	assert_contains "2d: stamp/data disagreement is reported" \
		"CCT BACKUP STAMP DOES NOT MATCH ITS DATA"
	assert_not_contains "2d: -y does not restore on a mismatched stamp" \
		"CRASH COUNT TABLE RESTORED FROM BACKUP"
}

# --------------------------------------------------------------------------
# case 3: primary corrupt, stale backup -- interactive YES performs restore
# --------------------------------------------------------------------------
case_restore_yes() {
	local img="$WORKDIR/c3.img"
	make_image "$img" "$SIZE" "--checksums" || { bad "case3 make_image"; return; }
	python3 "$TESTS_DIR/prime.py" "$img" --cc 4 --base-txc 1388 >&2 || { bad "case3 prime"; return; }
	corrupt "$img" cct 24 || { bad "case3 corrupt"; return; }

	# No -y; feed explicit YES to the Q_IMPORTANT prompt. Extra YES lines are
	# harmless: they answer any later prompts the forced pass raises.
	printf 'YES\nYES\nYES\nYES\nYES\nYES\n' | "$FSCK" -f "$img" >"$WORKDIR/c3.out" 2>&1 || true

	if grep -qiF "RESTORE CRASH COUNT TABLE FROM BACKUP?" "$WORKDIR/c3.out"; then
		ok "3: the Q_IMPORTANT prompt is shown interactively"
	else
		bad "3: restore prompt never appeared on stdout"
	fi
	if grep -qiF "CRASH COUNT TABLE RESTORED FROM BACKUP" "$WORKDIR/c3.out"; then
		ok "3: interactive YES restores the table"
	else
		bad "3: restore did not happen"
		tail -25 "$WORKDIR/c3.out" | sed 's/^/       /'
		return
	fi

	local same
	same=$(python3 -I "$TESTS_DIR/cctstamp.py" show "$img" | sed -n 's/^mirror=//p')
	[ "$same" = "same" ] && ok "3: restored primary matches the mirror" \
		|| bad "3: primary and mirror still differ after restore"

	run_fsck "$img" -f -y
	assert_not_contains "3: restored volume reports no backup corruption" \
		"CCT BACKUP CORRUPT"
	assert_not_contains "3: restored primary verifies against its CRCs" \
		"DOES NOT MATCH ITS CHECKSUM"
}

# --------------------------------------------------------------------------
# case 4: both corrupt -> destructive-path warning
# --------------------------------------------------------------------------
case_both_bad() {
	local img="$WORKDIR/c4.img"
	make_synced_volume "$img" || { bad "case4 setup"; return; }
	corrupt "$img" cct 28 || { bad "case4 corrupt primary"; return; }
	corrupt "$img" cct_backup 28 || { bad "case4 corrupt backup"; return; }

	run_fsck "$img" -f -y
	assert_contains "4: warns both copies are damaged" \
		"BOTH THE CRASH COUNT TABLE AND ITS BACKUP ARE DAMAGED"
	assert_contains "4: says plainly there is no non-destructive recovery" \
		"NO NON-DESTRUCTIVE RECOVERY IS AVAILABLE"
	assert_contains "4: warns the reset discards the epoch mapping" \
		"DISCARDS"
	assert_not_contains "4: does not offer a restore from a bad backup" \
		"RESTORE CRASH COUNT TABLE FROM BACKUP"
}

# --------------------------------------------------------------------------
# case 5: backup region absent -- created exactly once
# --------------------------------------------------------------------------
case_no_backup() {
	local img="$WORKDIR/c5.img"
	make_synced_volume "$img" || { bad "case5 setup"; return; }
	# Zero both backup pointers and reseal the superblock with CRC-32C. (An
	# earlier version resealed with zlib.crc32, i.e. CRC-32 with a stray
	# seed, so the superblock it produced did not actually verify.)
	python3 - "$img" "$TESTS_DIR" <<'INNER' || { bad "case5 wipe"; return; }
import struct, sys
img, tdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, tdir)
from sbinfo import SUPERBLOCK_SECTOR, SECTOR
from prime import crc32c_std
with open(img, 'r+b') as f:
    f.seek(SUPERBLOCK_SECTOR * SECTOR)
    blk = bytearray(f.read(SECTOR))
    blk[112:128] = b'\x00' * 16          # cct_backup, cct_backup_crc
    struct.pack_into('<I', blk, 508, crc32c_std(bytes(blk[:508])))
    f.seek(SUPERBLOCK_SECTOR * SECTOR)
    f.write(blk)
print("cleared both cct_backup pointers", file=sys.stderr)
INNER
	run_fsck "$img" -f -y
	assert_contains "5: reports the missing backup region" \
		"NO CCT BACKUP REGION IN SUPERBLOCK"
	assert_not_contains "5: the edited superblock still verifies" \
		"BAD CHECKSUM"
	local n
	n=$(grep -c 'CCT BACKUP CREATED AT' "$FSCK_OUT")
	[ "$n" -eq 1 ] && ok "5: the backup is created exactly once" \
		|| bad "5: backup created $n times (a second one leaks the first)"
	# Zeroing the pointers orphaned the old backup (512 sectors) and its
	# CRC region (5 units rounded up to whole blocks), so fsck must reclaim
	# exactly that much -- not that plus a second, leaked backup.
	local bs orphan
	bs=$(( 1 << $(python3 -I "$TESTS_DIR/sbinfo.py" "$img" | sed -n 's/^sectors_per_block_bits=//p') ))
	orphan=$(( 512 + (5 + bs - 1) / bs * bs ))
	if grep -qF "FILESYSTEM HAS $orphan LOST SECTORS IN 1 RUNS" "$FSCK_OUT"; then
		ok "5: only the orphaned old backup ($orphan sectors) is reclaimed"
	else
		bad "5: lost-sector count is not the orphaned old backup ($orphan)" \
			"$(grep -F 'LOST SECTORS' "$FSCK_OUT" | tr -d '\r' | sed 's/.*FILESYSTEM/FILESYSTEM/')"
	fi
	stamp_is "5: the new backup is a stamped copy of the primary" \
		"$img" "magic=ok stamp_cc=4 stamp_txc=1776 sealed=yes tx_cc=4 primary_txc=1776 mirror=same"
	run_fsck "$img" -f -y
	assert_not_contains "5: second pass creates nothing" \
		"CCT BACKUP CREATED"
	assert_not_contains "5: second pass finds no lost sectors" \
		"LOST SECTORS"
}

# --------------------------------------------------------------------------
# case 6: --reset-crash-counts keeps the CRCs and the mirror in step
# --------------------------------------------------------------------------
case_reset_crash_counts() {
	local img="$WORKDIR/c6.img"
	make_synced_volume "$img" || { bad "case6 setup"; return; }
	run_fsck "$img" -f -y --reset-crash-counts
	# rc bits 1 (modified) and 2 (reboot needed) are normal for a reset
	[ $(( FSCK_RC & ~3 )) -eq 0 ] && ok "6: reset pass completes with no error bits (rc=$FSCK_RC)" \
		|| bad "6: reset pass rc=$FSCK_RC" "$(tail -5 "$FSCK_OUT")"
	stamp_is "6: after the reset the mirror is a stamped copy of the new table" \
		"$img" "magic=ok stamp_cc=0 stamp_txc=0 sealed=yes tx_cc=0 primary_txc=0 mirror=same"
	run_fsck "$img" -f -y
	assert_not_contains "6: the reset table verifies against its CRCs" \
		"DOES NOT MATCH ITS CHECKSUM"
	assert_not_contains "6: the pass is not abandoned" \
		"remains untrusted"
	assert_not_contains "6: no restore from a pre-reset backup is offered" \
		"RESTORE CRASH COUNT TABLE FROM BACKUP"
	assert_not_contains "6: the mirror is current" \
		"CCT BACKUP IS OUT OF DATE"
}

# --- run ------------------------------------------------------------------

case_stale_backup_verifies
case_fresh_stamped
case_both_good
case_restore_current
case_restore_stale_gated
case_restore_unstamped_gated
case_stamp_mismatch
case_restore_yes
case_both_bad
case_no_backup
case_reset_crash_counts

echo
summary
