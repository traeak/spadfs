# shellcheck shell=bash
# Volume fixture creation for the unprivileged SpadFS test suite.

# make_image <path> <size-spec> [extra mkspadfs opts...]
#
# Two non-obvious requirements, both verified against the source:
#
#  1. The image file MUST be pre-created. mkspadfs opens it with
#     O_RDWR | O_EXCL and no O_CREAT (mkspadfs.c:888), so a missing file fails.
#
#  2. The fill pattern MUST NOT be zeros. A zero-filled (or sparse) image makes
#     the `zero_out` corruption class indistinguishable from untouched space,
#     producing false negatives in the detection harness. We tile a 4 KiB
#     pattern containing no 0x00 byte.
make_image() {
	local path="$1" size="$2"
	shift 2

	rm -f "$path"

	python3 - "$path" "$size" <<'PY' || return 1
import re
import sys

path, spec = sys.argv[1], sys.argv[2].upper()
m = re.fullmatch(r'(\d+)([KMGT]?)', spec)
if not m:
    sys.exit("make_image: bad size spec %r" % spec)
mult = {'': 1, 'K': 1024, 'M': 1024**2, 'G': 1024**3, 'T': 1024**4}[m.group(2)]
n = int(m.group(1)) * mult

# 0xA5..0xFF tiled, with any would-be 0x00 replaced so the image has no holes.
pat = bytes(0xA5 if (0xA5 + i) % 256 == 0 else (0xA5 + i) % 256 for i in range(4096))
with open(path, 'wb') as f:
    written = 0
    while written < n:
        chunk = pat[:min(len(pat), n - written)]
        f.write(chunk)
        written += len(chunk)
PY

	"$MK" --no-trim "$@" "$path" >/dev/null 2>&1
}

# check_clean <path> [fsck opts...] -> 0 if spadfsck reports the volume clean.
check_clean() {
	local path="$1"
	shift
	"$FSCK" -f "$@" "$path" >/dev/null 2>&1
}
