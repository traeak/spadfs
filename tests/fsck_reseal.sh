#!/usr/bin/env bash
# fsck must reseal the root's fixed_fnode_block with the CRC trailer after it
# repairs the directory size.
#
# The root lives in a fixed_fnode_block (CRC trailer). check_dir_size() and
# adjust_dir_size() used to reseal it with the chain XOR fold, so a "fixed"
# root failed its trailer: the next fsck reported BAD CHECKSUM ON FIXED FNODE
# BLOCK and the kernel refused to mount (EUCLEAN).
#
# The mkfs -> fsck suite cannot see this: mkfs writes a correct trailer and
# fsck never repairs anything. Only a repair followed by a re-check does.
#
# Steps: format, bump the root's stored size and reseal it correctly (so the
# only defect is the size), let fsck -y fix it, then require a valid trailer
# and a clean second fsck.

set -uo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

IMG=$WORKDIR/reseal.img

# root_fx IMG show|bump -- inspect or corrupt the root fixed_fnode_block.
# Prints "size=<n> crc=<ok|bad>". bump adds 512 to fnode0.size[0] and reseals.
root_fx() {
	python3 -I - "$TESTS_DIR/sbinfo.py" "$1" "$2" <<'PY'
import subprocess, sys
sbinfo, img, mode = sys.argv[1:4]
root = None
for line in subprocess.run([sys.executable, "-I", sbinfo, img],
                           capture_output=True, text=True, check=True).stdout.splitlines():
    k, _, v = line.partition("=")
    if k == "root":
        root = int(v, 0)
if root is None:
    sys.exit("no root in sbinfo output")

T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    T.append(c)
def crc32c(b):
    c = 0xffffffff
    for x in b:
        c = T[(c ^ x) & 0xff] ^ (c >> 8)
    return c ^ 0xffffffff

with open(img, "r+b") as f:
    f.seek(root * 512)
    b = bytearray(f.read(512))
    if mode == "bump":
        v = int.from_bytes(b[32:40], "little") + 512
        b[32:40] = v.to_bytes(8, "little")
        b[508:512] = crc32c(b[:508]).to_bytes(4, "little")
        f.seek(root * 512)
        f.write(b)
    ok = crc32c(b[:508]) == int.from_bytes(b[508:512], "little")
    print("size=%d crc=%s" % (int.from_bytes(b[32:40], "little"),
                              "ok" if ok else "bad"))
PY
}

log "== fsck reseals the root fixed_fnode_block with the CRC trailer =="

make_image "$IMG" 64M || die "make_image failed"
check_clean "$IMG" && ok "fresh volume is clean" || bad "fresh volume is clean"

before=$(root_fx "$IMG" show) || die "cannot read root"
bumped=$(root_fx "$IMG" bump) || die "cannot bump root"
[ "$bumped" != "$before" ] && [ "${bumped#*crc=}" = "ok" ] \
	&& ok "root size bumped, trailer still valid ($bumped)" \
	|| bad "bump did not take" "$before -> $bumped"

out=$("$FSCK" -f -y "$IMG" 2>&1)
grep -q 'DIRECTORY SIZE MISCOUNTED' <<<"$out" \
	&& ok "fsck detects the miscounted root size" \
	|| bad "fsck did not see the miscount" "$out"

after=$(root_fx "$IMG" show) || die "cannot re-read root"
[ "$after" = "$before" ] \
	&& ok "repair restored the size and left a valid trailer ($after)" \
	|| bad "root after repair" "want '$before', got '$after'"

out2=$("$FSCK" -f -n "$IMG" 2>&1); rc=$?
if [ "$rc" -eq 0 ] && ! grep -q 'BAD CHECKSUM' <<<"$out2"; then
	ok "second fsck is clean"
else
	bad "second fsck rc=$rc" "$(grep -E 'CHECKSUM|ERROR' <<<"$out2" | head -3)"
fi

summary
