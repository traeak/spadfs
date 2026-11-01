#!/bin/bash
# cc_wrap.sh -- the crash-count ceiling, end to end, without root.
#
# cc is 16 bits and indexes the 65536-entry cct directly, so it cannot simply
# be widened (Phase 4 finding). It only advances on an unclean shutdown; at
# 0xffff the kernel mounts read-only. The way back is spadfsck's reset pass,
# which rewrites the cc/txc pair of every structure and zeroes the cct.
# From CC_WARN_THRESHOLD (struct.h) the kernel warns and fsck offers the
# reset. This file pins the fsck half:
#
#   1. below the threshold fsck says nothing
#   2. at and above it fsck reports HIGH CRASH COUNT
#   3. -n reports but writes nothing
#   4. -y resets: tx.cc == 0, the whole cct is zero, and a forced re-check
#      is clean -- every structure's cc/txc was rewritten consistently
#   5. a second -y on the reset volume changes nothing
#
# The kernel half (the mount-time warning, read-only at 0xffff) needs a mount:
# tests/cc_wrap_mount.sh.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"
require_tools

THRESHOLD=$((0xff00)) # CC_WARN_THRESHOLD, struct.h

tx_cc() { # image -> tx.cc as a decimal
	python3 -I - "$1" "$TESTS_DIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from sbinfo import read_super
p = sys.argv[1]
off = read_super(p)["txblock"] * 512
with open(p, "rb") as f:
    f.seek(off)
    print(int.from_bytes(f.read(8)[6:8], "little"))
PY
}

cct_nonzero() { # image -> count of non-zero cct entries
	python3 -I - "$1" "$TESTS_DIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
from sbinfo import read_super
p = sys.argv[1]
off = read_super(p)["cct"] * 512
with open(p, "rb") as f:
    f.seek(off)
    d = f.read(65536 * 4)
print(sum(1 for i in range(0, len(d), 4) if d[i:i + 4] != b"\0\0\0\0"))
PY
}

primed() { # cc -> path of a fresh image primed to that crash count
	local img="$WORKDIR/cc-$1.img"
	make_image "$img" 32M || die "mkfs failed"
	python3 -I "$TESTS_DIR/prime.py" "$img" --cc "$1" --base-txc 100 >/dev/null ||
		die "prime to cc $1 failed"
	printf '%s\n' "$img"
}

# --- 1/2: the threshold ---------------------------------------------------
img=$(primed $((THRESHOLD - 1)))
if "$FSCK" -n "$img" 2>&1 | grep -q 'HIGH CRASH COUNT'; then
	bad "cc just below the threshold is silent" "fsck warned at $((THRESHOLD - 1))"
else
	ok "cc just below the threshold is silent"
fi

for cc in $THRESHOLD $((0xffff)); do
	img=$(primed "$cc")
	want=$(printf 'HIGH CRASH COUNT REACHED (%04X)' "$cc")
	if "$FSCK" -n "$img" 2>&1 | grep -qF "$want"; then
		ok "cc $(printf %04x "$cc"): fsck reports it"
	else
		bad "cc $(printf %04x "$cc"): fsck reports it" "no '$want'"
	fi
done

# --- 3: -n is read-only ---------------------------------------------------
img=$(primed $((0xffff)))
before=$(sha256sum <"$img")
"$FSCK" -n "$img" >/dev/null 2>&1
if [ "$(sha256sum <"$img")" = "$before" ]; then
	ok "-n at the ceiling writes nothing"
else
	bad "-n at the ceiling writes nothing" "image changed"
fi

# --- 4: -y resets ---------------------------------------------------------
"$FSCK" -y "$img" >"$WORKDIR/reset.log" 2>&1
if grep -q 'resetting crash count table' "$WORKDIR/reset.log"; then
	ok "-y at the ceiling runs the reset pass"
else
	bad "-y at the ceiling runs the reset pass" "$(tail -3 "$WORKDIR/reset.log")"
fi
got=$(tx_cc "$img")
[ "$got" = 0 ] && ok "after reset tx.cc is 0" || bad "after reset tx.cc is 0" "tx.cc=$got"
got=$(cct_nonzero "$img")
[ "$got" = 0 ] && ok "after reset the cct is all zero" || bad "after reset the cct is all zero" "$got non-zero entries"
if check_clean "$img" -n; then
	ok "after reset a forced check is clean"
else
	bad "after reset a forced check is clean" "fsck -n -f not clean"
fi
if "$FSCK" -n "$img" 2>&1 | grep -q 'HIGH CRASH COUNT'; then
	bad "after reset the warning is gone" "still warns"
else
	ok "after reset the warning is gone"
fi

# --- 5: idempotent --------------------------------------------------------
before=$(sha256sum <"$img")
"$FSCK" -y "$img" >/dev/null 2>&1
if [ "$(sha256sum <"$img")" = "$before" ]; then
	ok "a second -y on the reset volume changes nothing"
else
	bad "a second -y on the reset volume changes nothing" "image changed"
fi

summary
