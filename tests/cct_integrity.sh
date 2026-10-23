#!/usr/bin/env bash
# The crash-count table has no checksum. This test proves it, and shows that the
# consequence is semantic, not cosmetic.
#
# Background: sckcct.c validates cct entries with one rule only -- entries ABOVE
# tx.cc must be zero, and no entry may have the 0x80000000 bit set. There is no
# __byte_sum over the cct anywhere (contrast super.c:507 for the txblock). So
# corruption confined to cct[tx.cc] -- the live entry every CC_VALID() decision
# reads -- is invisible.
#
# A fresh volume's whole table is zeros, so this test first PRIMES it with a
# plausible post-mount state (tests/prime.py), which is why it did not exist
# before: without priming, every class is a no-op and the harness reports NOOP.
#
# Modes:
#   default   record the baseline, always exit 0
#   --strict  exit non-zero if any corruption silently flips CC_VALID.
#             Use this after Phase 3 to prove the gap is closed.
#
# Runs without root.

set -uo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

STRICT=0
[ "${1:-}" = "--strict" ] && STRICT=1

SIZE="${TEST_IMAGE_SIZE:-16M}"
IMG="$WORKDIR/cct.img"
PRIMED="$WORKDIR/cct-primed.img"

make_image "$IMG" "$SIZE" "--checksums" || die "make_image failed"

# Prime: cc=4 with plausible transaction counts.
python3 "$TESTS_DIR/prime.py" "$IMG" --cc 4 --base-txc 1388 >&2 || die "prime failed"
cp -f "$IMG" "$PRIMED"

# The primed volume must still be clean, or we are testing our own fixture bug.
# check_clean takes <path> [fsck opts...]; -n keeps it read-only.
if ! check_clean "$PRIMED" -n; then
	die "primed volume is not clean -- fixture is broken, not the filesystem"
fi

CC=$(python3 -c "
import sys; sys.path.insert(0,'$TESTS_DIR')
from sbinfo import read_super
from corrupt import live_cc
print(live_cc('$PRIMED', read_super('$PRIMED')))")

read_val() {
	python3 -c "
import sys; sys.path.insert(0,'$TESTS_DIR')
from sbinfo import read_super, SECTOR
i=read_super('$1')
f=open('$1','rb'); f.seek(i['cct']*SECTOR + $CC*4)
print(int.from_bytes(f.read(4),'little'))"
}

valid_p() {
	# CC_VALID(cc,txc) = (s32)(cct[cc] - txc) >= 0   [spadfs.h:550]
	python3 -c "
c,t = $1, $2
s = (c - t) & 0xFFFFFFFF
if s >= 1<<31: s -= 1<<32
print('True' if s >= 0 else 'False')"
}

ORIG=$(read_val "$PRIMED")
ORIG_VALID=$(valid_p "$ORIG" "$ORIG")

printf '# Crash-count table integrity\n\n'
printf 'Primed volume: tx.cc=%s, cct[cc]=%s, CC_VALID=%s\n\n' "$CC" "$ORIG" "$ORIG_VALID"
printf '| Class | cct[cc] after | delta | CC_VALID | fsck | Verdict |\n'
printf '|---|---|---|---|---|---|\n'

silent_flips=0
silent_corrupt=0

for cls in bitflip byte_transpose word_swap word_dup xor_cancel zero_out; do
	img="$WORKDIR/cct-$cls.img"
	cp -f "$PRIMED" "$img"

	out=$(python3 "$TESTS_DIR/corrupt.py" "$img" cct_live "$cls" 2>&1)
	rc=$?
	if [ "$rc" -eq 3 ]; then
		printf '| %s | - | - | - | - | no-op, tested nothing |\n' "$cls"
		continue
	fi
	# Record where the corruptor actually wrote, so a future drift is visible
	# rather than silently changing what is under test.
	printf '#   %-15s %s\n' "$cls" "$out" >&2

	NEW=$(read_val "$img")
	NEW_VALID=$(valid_p "$NEW" "$ORIG")
	"$FSCK" -n -f "$img" >/dev/null 2>&1
	frc=$?

	if [ "$frc" -eq 0 ]; then
		verdict="**SILENT**"
		silent_corrupt=$((silent_corrupt + 1))
		[ "$NEW_VALID" != "$ORIG_VALID" ] && silent_flips=$((silent_flips + 1))
	else
		verdict="detected"
	fi

	printf '| %s | %s | %+d | %s | rc=%s | %s |\n' \
		"$cls" "$NEW" "$((NEW - ORIG))" "$NEW_VALID" "$frc" "$verdict"
done

printf '\n## Summary\n\n'
printf -- '- live entry corrupted without any fsck complaint: **%d**\n' "$silent_corrupt"
printf -- '- of those, CC_VALID flipped: **%d**\n' "$silent_flips"
printf '\nThe detections that *do* happen come from the ordering rule in\n'
printf 'sckcct.c (entries above tx.cc must be zero), not from a checksum --\n'
printf 'which is why classes that spill above cc are caught and classes confined\n'
printf 'to cct[cc] itself are not.\n'

if [ "$STRICT" -eq 1 ] && [ "$silent_corrupt" -gt 0 ]; then
	printf '\nSTRICT: %d silent corruption(s) remain. Phase 3 not done.\n' "$silent_corrupt"
	exit 1
fi
exit 0
