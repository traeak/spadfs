#!/bin/bash
# txb_backup.sh -- the txblock's backup copy, without root.
#
# The txblock holds the crash count, which apage index is live, the error
# flags and the next stable inode number, and it used to exist once. A
# damaged one stopped the mount, and spadfsck had to start from an empty
# txblock: it threw away the (intact) crash-count table, rebuilt the apage
# index from a scan and guessed cc and the inode counter.
#
# The superblock now points at a second copy (txblock_backup), which mkfs
# writes and the kernel rewrites at every txblock write. Only spadfsck reads
# it. This file pins the mkfs and fsck halves:
#
#   1. mkfs writes both copies, sealed, with the same contents
#   2. a damaged primary: -n reports it and names the intact backup, and
#      does NOT fall back to the empty txblock (no cct rebuild); -y restores
#      the primary with the backup's contents (a primed cc survives), and a
#      forced re-check is clean
#   3. a damaged backup: -n reports it; -y rewrites it; clean after
#   4. both damaged: the old empty-txblock path, reported
#
# The kernel half (both copies stay in step through real mounts, and a
# volume with a damaged primary mounts again after fsck -y) is in
# tests/mount_smoke.sh.

set -uo pipefail
# Output is captured first and grepped from a here-string: under pipefail,
# `spadfsck | grep -q` takes spadfsck's non-zero status (see cc_wrap.sh).

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"
require_tools

txinfo() { python3 "$TESTS_DIR/txbinfo.py" "$1"; }

# flip one bit at byte off of a sector
flip() {
	python3 -I - "$1" "$2" "$3" <<'PY'
import sys
p, sec, off = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(p, "r+b") as f:
    f.seek(sec * 512 + off)
    b = f.read(1)[0]
    f.seek(sec * 512 + off)
    f.write(bytes([b ^ 0x20]))
PY
}

field() { python3 "$TESTS_DIR/sbinfo.py" "$1" | sed -n "s/^$2=//p"; }

BASE="$WORKDIR/base.img"
make_image "$BASE" 64M >/dev/null || die "make_image failed"
# A primed cc (not mkfs's 0) shows a restore brings back the backup's
# contents rather than an empty txblock. prime.py keeps both copies in step.
python3 "$TESTS_DIR/prime.py" "$BASE" --cc 4 --base-txc 1388 >/dev/null || die "prime failed"
PRI=$(field "$BASE" txblock)
BAK=$(field "$BASE" txblock_backup)

log "== 1. mkfs writes both copies"
[ -n "$BAK" ] && [ "$BAK" != 0 ] && [ "$BAK" != "$PRI" ] \
	&& ok "superblock points at a separate backup block ($BAK, primary $PRI)" \
	|| bad "no txblock backup in the superblock" "txblock_backup='$BAK'"
info=$(txinfo "$BASE")
[ "$info" = "pri=ok bak=ok same=1 cc=4" ] \
	&& ok "both copies sealed, same contents ($info)" \
	|| bad "copies not both sealed and equal" "$info"
check_clean "$BASE" -n && ok "fresh primed volume is clean" || bad "fresh primed volume is not clean"

log "== 2. damaged primary"
IMG="$WORKDIR/pri.img"
cp "$BASE" "$IMG"
flip "$IMG" "$PRI" 40
out=$("$FSCK" -n -f "$IMG" 2>&1); rc=$?
[ "$rc" -ne 0 ] && grep -q "TX BLOCK $(printf '%X' "$PRI"): BAD CHECKSUM" <<<"$out" \
	&& ok "-n reports the damaged primary (rc=$rc)" \
	|| bad "-n does not report the damaged primary" "rc=$rc $(tail -5 <<<"$out")"
grep -q "TX BLOCK BACKUP $(printf '%X' "$BAK") IS INTACT" <<<"$out" \
	&& ok "-n names the intact backup" \
	|| bad "-n does not name the backup" "$(tail -5 <<<"$out")"
# The fallback's signature: the empty txblock has cc FFFF, which the cct
# backup check prints as the volume's cc. (That check also says "OUT OF
# DATE" on this fixture regardless -- prime.py primes the cct but not its
# backup -- so only the cc value tells the two paths apart.)
FALLBACK='VOLUME CC FFFF'
grep -qE "$FALLBACK" <<<"$out" \
	&& bad "-n still falls back to an empty txblock" "$(grep -E "$FALLBACK" <<<"$out" | head -3)" \
	|| ok "no empty-txblock fallback: the backup's contents are used"
"$FSCK" -y -f "$IMG" >/dev/null 2>&1
info=$(txinfo "$IMG")
[ "$info" = "pri=ok bak=ok same=1 cc=4" ] \
	&& ok "-y restores the primary from the backup, primed cc intact ($info)" \
	|| bad "-y did not restore the primary" "$info"
check_clean "$IMG" -n && ok "forced re-check after the restore is clean" \
	|| bad "volume not clean after the restore" "$("$FSCK" -n -f "$IMG" 2>&1 | tail -5)"

log "== 3. damaged backup"
IMG="$WORKDIR/bak.img"
cp "$BASE" "$IMG"
flip "$IMG" "$BAK" 40
out=$("$FSCK" -n -f "$IMG" 2>&1); rc=$?
[ "$rc" -ne 0 ] && grep -q "TX BLOCK BACKUP $(printf '%X' "$BAK"): BAD CHECKSUM" <<<"$out" \
	&& ok "-n reports the damaged backup (rc=$rc)" \
	|| bad "-n does not report the damaged backup" "rc=$rc $(tail -5 <<<"$out")"
"$FSCK" -y -f "$IMG" >/dev/null 2>&1
info=$(txinfo "$IMG")
[ "$info" = "pri=ok bak=ok same=1 cc=4" ] \
	&& ok "-y rewrites the backup ($info)" \
	|| bad "-y did not rewrite the backup" "$info"
check_clean "$IMG" -n && ok "clean after rewriting the backup" \
	|| bad "volume not clean after rewriting the backup"

log "== 4. both damaged"
IMG="$WORKDIR/both.img"
cp "$BASE" "$IMG"
flip "$IMG" "$PRI" 40
flip "$IMG" "$BAK" 40
out=$("$FSCK" -n -f "$IMG" 2>&1); rc=$?
[ "$rc" -ne 0 ] && grep -q "TX BLOCK $(printf '%X' "$PRI"): BAD CHECKSUM" <<<"$out" \
	&& grep -q "TX BLOCK BACKUP $(printf '%X' "$BAK"): BAD CHECKSUM" <<<"$out" \
	&& ok "-n reports both copies damaged (rc=$rc)" \
	|| bad "-n does not report both copies" "rc=$rc $(tail -5 <<<"$out")"
# the control for case 2's "no fallback" check: here the fallback must show
grep -qE "$FALLBACK" <<<"$out" \
	&& ok "with no intact copy fsck falls back to an empty txblock, and case 2's check sees it" \
	|| bad "fallback signature not seen; case 2's absence check proves nothing" "$(tail -5 <<<"$out")"

summary
