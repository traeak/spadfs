#!/usr/bin/env bash
# Build the v1.0.22 reference stack next to spadfs2 (roadmap Task 0.4).
#
# Checks out the original SpadFS v1.0.22 (commit 59e33f2, the first commit of
# this mirror) into a git worktree, applies the patches in tests/reference/
# -- only what the running kernel needs to compile it, never behaviour -- and
# builds spadfs.ko (filesystem type "spadfs"), mkspadfs and spadfsck.
#
# Its names do not clash with spadfs2's (spadfs2.ko, type "spadfs2",
# mkspadfs2, spadfsck2), so both modules can be loaded at once and
# tests/bench_ab.sh can compare them on the same device.
#
# Usage: bash tests/build_reference.sh [DIR]      (default /tmp/spadfs-ref)
# No root needed. Remove with: git worktree remove DIR

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${1:-/tmp/spadfs-ref}"
REF=59e33f2

if [ -e "$DIR" ]; then
	echo "build_reference: $DIR exists; remove it first (git worktree remove $DIR)" >&2
	exit 1
fi
git -C "$REPO" worktree add -q --detach "$DIR" "$REF"
for p in "$REPO"/tests/reference/*.patch; do
	git -C "$DIR" apply "$p"
	echo "applied $(basename "$p")"
done
make -C "$DIR" >"$DIR/build.log" 2>&1 || { tail -20 "$DIR/build.log" >&2; exit 1; }
# Same rule as the main tree: only the known modpost warning is acceptable.
if grep -E 'warning:|error:' "$DIR/build.log" | grep -v MODULE_DESCRIPTION; then
	echo "build_reference: unexpected warnings, see $DIR/build.log" >&2
	exit 1
fi
for f in spadfs.ko mkspadfs spadfsck; do
	[ -e "$DIR/$f" ] || { echo "build_reference: $f was not built" >&2; exit 1; }
done
echo "reference v1.0.22 built in $DIR:"
echo "  module $DIR/spadfs.ko (type spadfs), tools $DIR/mkspadfs $DIR/spadfsck"
