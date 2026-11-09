#!/bin/bash
# fsck_fblk_magic.sh -- spadfsck's repair of an fnode block unit with bad
# magic, without root.
#
# A directory's fnode block spans several 512-byte units when the block size
# is larger than 512. When an inner unit has bad magic, spadfsck rebuilds it
# as an empty unit: header, then one free fnode spanning the rest. Up to
# v1.0.22 the free fnode was written into fnodes[1] instead of fnodes[0], so
# the rebuilt unit began with a zero-length fnode and the same run reported
#   FNODE BLOCK ...: INVALID FNODE SIZE 0 AT 20
# and needed a second repair. The repair must produce a valid unit by itself:
#
#   1. -y reports the bad magic and nothing else
#   2. a forced re-check is clean

set -uo pipefail
# Output is captured first and grepped from a here-string: under pipefail,
# `spadfsck | grep -q` takes spadfsck's non-zero status (see cc_wrap.sh).

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"
require_tools

field() { python3 "$TESTS_DIR/sbinfo.py" "$1" | sed -n "s/^$2=//p"; }

# spadfsck's findings without its progress output
findings() {
	tr '\r' '\n' <<<"$1" | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' |
		grep -vE '^[0-9]+%|^ *\(?[0-9]+ of [0-9]+\)?|^checking|^done|^synchronizing|^ *$'
}

IMG="$WORKDIR/fblk.img"
# 4096-byte blocks, so the root directory block has inner units
make_image "$IMG" 64M --block-size 4096 >/dev/null || die "make_image failed"
ROOT=$(field "$IMG" root)
BITS=$(field "$IMG" sectors_per_block_bits)
[ "$BITS" -ge 1 ] || die "block size is 512; there are no inner units to damage"
# the root directory follows the root fnode block; damage its second unit
UNIT=$((ROOT + (1 << BITS) + 1))
printf '\xde\xad' | dd of="$IMG" bs=1 seek=$((UNIT * 512)) conv=notrunc status=none

out=$(findings "$("$FSCK" -y -f "$IMG" 2>&1)")
hex=$(printf '%X' "$UNIT")
grep -q "BAD MAGIC ON FNODE BLOCK $hex" <<<"$out" \
	&& ok "-y reports the bad magic on unit $hex" \
	|| bad "-y does not report the bad magic" "$out"
other=$(grep -v "BAD MAGIC ON FNODE BLOCK $hex" <<<"$out")
[ -z "$other" ] \
	&& ok "the rebuilt unit is valid at once: nothing else reported" \
	|| bad "the repair left a broken unit behind" "$other"
"$FSCK" -n -f "$IMG" >/dev/null 2>&1 \
	&& ok "forced re-check is clean" \
	|| bad "volume not clean after the repair" "$(findings "$("$FSCK" -n -f "$IMG" 2>&1)")"

summary
