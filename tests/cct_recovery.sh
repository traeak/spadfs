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
	"$FSCK" "$@" "$img" >"$FSCK_OUT" 2>&1 || true
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
	# The mirror is stale, yet fsck must still call it usable -- verification
	# says nothing about currency. Assert that explicitly.
	assert_not_contains "stale mirror is NOT reported corrupt" \
		"CCT BACKUP CORRUPT"
	assert_not_contains "stale mirror triggers no restore" \
		"RESTORE CRASH COUNT TABLE FROM BACKUP"
	log "       ^ this is why Task 3.3 needs an epoch stamp on the mirror"
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
# case 2: primary corrupt, backup good -- restore offered, gated
# --------------------------------------------------------------------------
case_restore_gated() {
	local img="$WORKDIR/c2.img"
	make_synced_volume "$img" || { bad "case2 setup"; return; }
	corrupt "$img" cct 20 || { bad "case2 corrupt"; return; }

	# -y must NOT auto-approve a restore: it is Q_IMPORTANT.
	#
	# Note on what can be asserted from stdout: query() routes both the
	# prompt text and the "DANGEROUS ACTIONS CANNOT BE AUTOANSWERED" reply
	# through log_only_*, i.e. to the log file rather than stdout. So the
	# gate is asserted by its EFFECT here -- no restore happened -- and the
	# prompt text itself is asserted in case 3, which runs interactively
	# where query() uses _vprintf.
	run_fsck "$img" -f -y
	assert_contains "2: fsck detects the damaged primary" \
		"CRASH COUNT TABLE CORRUPT"
	assert_contains "2: fsck recognises the backup as a valid source" \
		"ITS BACKUP VERIFIES"
	assert_not_contains "2: -y did not silently restore" \
		"CRASH COUNT TABLE RESTORED FROM BACKUP"
	# and the primary is still the corrupted bytes -- nothing was written
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
		&& ok "2: primary left untouched under -y (still differs from mirror)" \
		|| bad "2: primary was modified despite -y gating the restore"
}

# --------------------------------------------------------------------------
# case 3: primary corrupt, backup good -- interactive YES performs restore
# --------------------------------------------------------------------------
case_restore_yes() {
	local img="$WORKDIR/c3.img"
	make_synced_volume "$img" || { bad "case3 setup"; return; }
	corrupt "$img" cct 24 || { bad "case3 corrupt"; return; }

	# No -y; feed explicit YES to the Q_IMPORTANT prompt. Extra YES lines are
	# harmless: they answer any later prompts the forced pass raises.
	printf 'YES\nYES\nYES\nYES\n' | "$FSCK" -f "$img" >"$WORKDIR/c3.out" 2>&1 || true

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

	# The restored volume must now verify clean, and the mirror and primary
	# must agree byte-for-byte again.
	local same
	same=$(python3 -c "
import sys; sys.path.insert(0,'$TESTS_DIR')
from sbinfo import read_super
d=read_super('$img')
f=open('$img','rb')
f.seek(d['cct']*512); prim=f.read(512*512)
f.seek(d['cct_backup']*512); bak=f.read(512*512)
print('SAME' if prim==bak else 'DIFFER')")
	[ "$same" = "SAME" ] && ok "3: restored primary matches the mirror" \
		|| bad "3: primary and mirror still differ after restore"

	run_fsck "$img" -f -y
	assert_not_contains "3: restored volume reports no backup corruption" \
		"CCT BACKUP CORRUPT"
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
# case 5: backup region absent
# --------------------------------------------------------------------------
case_no_backup() {
	local img="$WORKDIR/c5.img"
	make_synced_volume "$img" || { bad "case5 setup"; return; }
	python3 - "$img" "$TESTS_DIR" <<'PY' || { bad "case5 wipe"; return; }
import sys
img, tdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, tdir)
from sbinfo import read_super, SECTOR
d = read_super(img)
# zero the cct_backup pointer (offset 112) so the volume looks backup-less
with open(img, 'r+b') as f:
    f.seek(d['cct_crc'] * SECTOR)  # touch nothing; placeholder to keep seek valid
    f.seek(16384 * SECTOR + 112)
    f.write(b'\x00' * 8)
    # re-seal the superblock trailer so only the pointer is gone
    f.seek(16384 * SECTOR)
    blk = bytearray(f.read(512))
import zlib
del blk[508:512]
crc = zlib.crc32(bytes(blk) + b'\x00'*4, 0x82c653ae) & 0xFFFFFFFF
with open(img, 'r+b') as f:
    f.seek(16384 * SECTOR + 508)
    f.write(crc.to_bytes(4, 'little'))
print("cleared cct_backup pointer", file=sys.stderr)
PY
	run_fsck "$img" -f -y
	assert_contains "5: reports the missing backup region" \
		"NO CCT BACKUP REGION IN SUPERBLOCK"
}

# --- run ------------------------------------------------------------------

case_stale_backup_verifies
case_both_good
case_restore_gated
case_restore_yes
case_both_bad
case_no_backup

echo
summary
