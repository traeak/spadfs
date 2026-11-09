#!/bin/bash
# apage_index_crc.sh -- the apage index CRC, without root.
#
# Each apage index copy's first n_apages entries are covered by a CRC-32C
# kept in the txblock (struct txblock, apage_index_crc). This pins the mkfs
# and fsck halves:
#
#   1. mkfs stores the CRC of index 0, and a fresh volume checks clean
#   2. damage that passes every structural check of the index (the apage
#      pointers of two entries swapped) is reported as a bad index
#      checksum -- before, it surfaced only indirectly, as a bad checksum on
#      the unused apage the swapped pointer led to
#   3. -y rebuilds the index, records the new CRC, and a forced re-check is
#      clean
#
# The driver half (it updates the CRC at every index write and verifies the
# live copy at mount) needs a mount: tests/meta_csum_mount.sh and
# tests/mount_smoke.sh run workloads that split apages.

set -uo pipefail
# Output is captured first and grepped from a here-string: under pipefail,
# `spadfsck | grep -q` takes spadfsck's non-zero status (see cc_wrap.sh).

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"
require_tools

# image -> "copy=K active=N stored=<hex> computed=<hex>" (tests/apageidx.py)
index_crc() { python3 "$TESTS_DIR/apageidx.py" "$1"; }

# swap the apage pointers of the first two index entries of copy 0
swap_pointers() {
	python3 -I - "$1" "$TESTS_DIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from sbinfo import read_super
p = sys.argv[1]
off = read_super(p)["apage_index0"] * 512
with open(p, "r+b") as f:
    f.seek(off)
    e = bytearray(f.read(32))
    e[0:8], e[16:24] = e[16:24], e[0:8]
    f.seek(off)
    f.write(e)
PY
}

# "... stored=X computed=Y" -> success when X == Y
crc_matches() { local s=${1#*stored=}; s=${s%% *}; [ "$s" = "${1##*computed=}" ]; }

findings() {
	tr '\r' '\n' <<<"$1" | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' |
		grep -vE '^[0-9]+%|^ *\(?[0-9]+ of [0-9]+\)?|^checking|^done|^synchronizing|^ *$'
}

IMG="$WORKDIR/idx.img"
make_image "$IMG" 64M >/dev/null || die "make_image failed"

log "== 1. mkfs stores the CRC"
crc=$(index_crc "$IMG")
crc_matches "$crc" \
	&& ok "the txblock holds the live index copy's CRC ($crc)" \
	|| bad "stored and computed index CRC differ on a fresh volume" "$crc"
check_clean "$IMG" -n && ok "fresh volume is clean" || bad "fresh volume is not clean"

log "== 2. plausible damage is reported as a bad index"
swap_pointers "$IMG"
out=$(findings "$("$FSCK" -n -f "$IMG" 2>&1)")
grep -q "BAD CHECKSUM ON APAGE INDEX 0" <<<"$out" \
	&& ok "-n reports the swapped pointers as a bad index checksum" \
	|| bad "-n does not report a bad index checksum" "$out"

log "== 3. -y rebuilds and records the new CRC"
"$FSCK" -y -f "$IMG" >/dev/null 2>&1
crc=$(index_crc "$IMG")
crc_matches "$crc" \
	&& ok "after the rebuild the stored CRC matches the index ($crc)" \
	|| bad "stored CRC does not match the rebuilt index" "$crc"
check_clean "$IMG" -n && ok "forced re-check after the rebuild is clean" \
	|| bad "volume not clean after the rebuild" "$(findings "$("$FSCK" -n -f "$IMG" 2>&1)")"

summary
