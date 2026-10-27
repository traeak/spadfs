#!/usr/bin/env bash
# Mount-path smoke test. RUN AS ROOT.
#
# Everything else in this suite works on plain image files with no kernel
# involvement. This is the first test that actually mounts spadfs2, and it
# targets three code paths that have never executed:
#
#   1. the mount path itself -- signature "FS SPAD2", version 1.0.0,
#      fs-spadfs2 alias, after the Phase 1 identity switch
#   2. spadfs_write_cct_crc() (super.c:696) -- Phase 2.3 added the kernel
#      writing the cct side CRC on every commit. mkfs writes it and fsck
#      reads it, but a real commit producing it is unverified. If the kernel
#      computes a different value than fsck expects, EVERY volume the kernel
#      writes gets flagged.
#   3. the anode trailer (Cat 2) -- compile-verified only. Anodes do not
#      exist until files are written, which requires a mount.
#
# SAFETY: operates only on a throwaway image via a loop device under a
# private mount point. Refuses to touch anything that already has a
# filesystem with data we did not create.
#
# Usage:  sudo bash tests/mount_smoke.sh [--keep]

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

if [ "$(id -u)" -ne 0 ]; then
	die "this test needs root (mount/losetup). Run: sudo bash tests/mount_smoke.sh"
fi

MNT="$REPO_ROOT/.smoke-mnt"
LOOP=""
IMG="$WORKDIR/mount-test.img"

cleanup() {
	local rc=$?
	set +e
	mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	[ "$KEEP" = "1" ] || { rm -rf "$MNT"; }
	return $rc
}
trap cleanup EXIT

banner() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
banner "0. module state"
# ---------------------------------------------------------------------------
if lsmod | grep -q '^spadfs2 '; then
	ok "spadfs2 already loaded"
else
	if insmod "$REPO_ROOT/spadfs2.ko" 2>/dev/null; then
		ok "insmod spadfs2.ko"
	else
		die "insmod failed: $(dmesg | tail -3)"
	fi
fi
grep -q spadfs2 /proc/filesystems \
	&& ok "spadfs2 present in /proc/filesystems" \
	|| { bad "not in /proc/filesystems"; exit 1; }

# ---------------------------------------------------------------------------
banner "1. format + loop + mount"
# ---------------------------------------------------------------------------
make_image "$IMG" 64M "--checksums" || die "make_image failed"
python3 "$TESTS_DIR/prime.py" "$IMG" --cc 1 --base-txc 100 >&2 || die "prime failed"
check_clean "$IMG" -n || die "formatted+primed volume is not clean before mount"
ok "volume clean before mount"

LOOP="$(losetup --find --show "$IMG")" || die "losetup failed"
ok "loop attached at $LOOP"

mkdir -p "$MNT"
if mount -t spadfs2 "$LOOP" "$MNT"; then
	ok "MOUNT SUCCEEDED -- identity switch works end to end"
else
	bad "mount failed"
	printf '       dmesg:\n'; dmesg | tail -12 | sed 's/^/         /'
	exit 1
fi
mount | grep spadfs2 | sed 's/^/       /'

# ---------------------------------------------------------------------------
banner "2. basic file operations"
# ---------------------------------------------------------------------------
for n in 1 2 3; do
	if echo "spadfs2 test payload $n" > "$MNT/file$n.txt" 2>/dev/null; then
		ok "create file$n.txt"
	else
		bad "create file$n.txt"
	fi
done

if [ "$(cat "$MNT/file2.txt" 2>/dev/null)" = "spadfs2 test payload 2" ]; then
	ok "read-back content matches"
else
	bad "read-back mismatch"
fi

# a bigger write forces real extent allocation -> anodes
if dd if=/dev/urandom of="$MNT/big.bin" bs=1M count=8 conv=fsync status=none 2>/dev/null; then
	ok "8 MiB write with fsync (exercises extent/anode allocation)"
else
	bad "8 MiB fsync write"
fi

# nested dirs
mkdir -p "$MNT/a/b/c" 2>/dev/null && echo x > "$MNT/a/b/c/deep.txt" 2>/dev/null \
	&& ok "nested mkdir + write" || bad "nested mkdir + write"

# symlink
ln -s file1.txt "$MNT/link" 2>/dev/null && ok "symlink create" || bad "symlink create"

# xattr
setfattr -n user.test -v hello "$MNT/file1.txt" 2>/dev/null \
	&& [ "$(getfattr -n user.test --only-values "$MNT/file1.txt" 2>/dev/null)" = "hello" ] \
	&& ok "xattr set/get" || warn "xattr unsupported here (skipped, not a failure)"

sync

# ---------------------------------------------------------------------------
banner "3. THE KEY CHECK: did the kernel write a valid cct CRC?"
# ---------------------------------------------------------------------------
# While still mounted, copy the backing store so we see the kernel's own
# output, then fsck it. This is the first time spadfs_write_cct_crc() has
# ever run against a real commit.
cp "$IMG" "$WORKDIR/after-mount.img" 2>/dev/null || cp --reflink=auto "$IMG" "$WORKDIR/after-mount.img"
OUT="$WORKDIR/fsck-live.txt"
"$FSCK" -n -f "$WORKDIR/after-mount.img" >"$OUT" 2>&1 || true

echo "  --- cct / crc lines ---"
grep -iE "crash count|crc|checksum" "$OUT" | sed 's/^/    /' | head -20

if grep -qiE "CRASH COUNT TABLE CORRUPT|DOES NOT MATCH ITS CHECKSUM|CRC UNIT.*MISMATCH" "$OUT"; then
	bad "kernel-written cct CRC does NOT match what fsck expects"
	printf '       >>> This is the Phase 2.3 hot-path bug we could not see unprivileged.\n'
else
	ok "kernel-written cct CRC verifies (spadfs_write_cct_crc is correct)"
fi

if grep -qiE "CCT BACKUP" "$OUT"; then
	echo "  --- backup lines (mirror is mkfs-only until Task 3.3) ---"
	grep -iE "CCT BACKUP" "$OUT" | sed 's/^/    /' | head -6
fi

# ---------------------------------------------------------------------------
banner "4. clean unmount + fsck"
# ---------------------------------------------------------------------------
sync
if umount "$MNT"; then
	ok "clean unmount"
else
	bad "umount failed: $(dmesg | tail -3)"
	exit 1
fi

OUT2="$WORKDIR/fsck-post.txt"
"$FSCK" -n -f "$IMG" >"$OUT2" 2>&1
rc2=$?
echo "  fsck -n -f rc=$rc2"
grep -viE "^[0-9]+%|^\s*$" "$OUT2" | tail -20 | sed 's/^/    /'
if [ "$rc2" -eq 0 ]; then
	ok "volume clean after real kernel writes"
else
	bad "fsck reports problems after normal use (rc=$rc2)"
fi

# ---------------------------------------------------------------------------
banner "5. anode trailer -- does it actually exist and verify?"
# ---------------------------------------------------------------------------
# Anodes are allocated for files with extents. Our 8 MiB file should have made
# some. If fsck says nothing about them, the trailer code path was never hit.
if grep -qiE "anode|ANODE" "$OUT2"; then
	echo "  anode-related fsck output:"
	grep -iE "anode" "$OUT2" | head -8 | sed 's/^/    /'
	ok "anode path exercised by fsck"
else
	warn "no anode output from fsck -- trailer path may still be unexercised"
	echo "       check: does the 8MiB file produce anodes at all?"
fi

# ---------------------------------------------------------------------------
banner "6. remount the kernel-written volume (round-trip through the module)"
# ---------------------------------------------------------------------------
if mount -t spadfs2 "$LOOP" "$MNT" 2>/dev/null; then
	ok "remount of kernel-written volume succeeds"
	if [ "$(cat "$MNT/file1.txt" 2>/dev/null)" = "spadfs2 test payload 1" ]; then
		ok "data survives unmount/remount intact"
	else
		bad "data did not survive remount"
	fi
	ls "$MNT" | sed 's/^/       /'
	umount "$MNT"
else
	bad "remount failed -- kernel cannot read what the kernel wrote"
	dmesg | tail -8 | sed 's/^/         /'
fi

losetup -d "$LOOP" 2>/dev/null; LOOP=""

printf '\n'
summary || true
printf '\nIf anything above failed, capture:  dmesg | tail -50\n'
