# shellcheck shell=bash
# Shared helpers for the SpadFS unprivileged test suite.
#
# Everything here runs WITHOUT root: no loop devices, no module loading, no
# mounting. The oracle is `spadfsck` operating on plain image files.

set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"

# Tool paths are env-overridable so the same suite can drive either stack:
#   - spadfs2 (this branch's default binaries)
#   - the v1.0.22 reference binaries, for the Phase 0.4 side-by-side rig
# Override with e.g.  MKSPADFS=/path/to/mkspadfs SPADFSCK=/path/to/spadfsck
SUFFIX="${SPADFS_TOOL_SUFFIX:-2}"
MK="${MKSPADFS:-$REPO_ROOT/mkspadfs$SUFFIX}"
FSCK="${SPADFSCK:-$REPO_ROOT/spadfsck$SUFFIX}"

PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=""

log()  { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die()  { printf 'FATAL: %s\n' "$*" >&2; exit 2; }

# Build products, not sources — fail loudly with the fix rather than mid-test.
require_tools() {
	local missing=""
	[ -x "$MK" ]   || missing="$missing $MK"
	[ -x "$FSCK" ] || missing="$missing $FSCK"
	if [ -n "$missing" ]; then
		die "missing build products:$missing  (run: make)"
	fi
}

ok()   { PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"; }

# Always returns 0 so a failure never aborts the run under set -e.
bad()  {
	FAIL=$((FAIL + 1))
	FAILED_NAMES="$FAILED_NAMES|$1"
	printf '  FAIL  %s\n' "$1"
	[ $# -gt 1 ] && printf '        %s\n' "$2"
	return 0
}

skip() { SKIP=$((SKIP + 1)); printf '  SKIP  %s — %s\n' "$1" "$2"; }

summary() {
	printf '\n%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
	if [ "$FAIL" -ne 0 ]; then
		printf 'failed:%s\n' "${FAILED_NAMES//|/ }"
		return 1
	fi
	return 0
}

# Kernel-log window for the root tests.
#
# Never count lines (`dmesg | wc -l`, then `tail -n +N`): once the ring
# buffer is full, old lines drop off as new ones arrive, the count stops
# moving, and "everything after line N" comes back EMPTY -- a false FAIL on a
# presence check and a false PASS on an absence check. A long run (e.g.
# crash_logwrites.sh mounting hundreds of crash states) fills the buffer.
#
# Instead write a unique marker into the log and read what follows it. If
# the marker itself has scrolled out, klog_since fails rather than return a
# misleadingly empty window; callers must stop on that.
KLOG_MARK=""
klog_mark() {
	KLOG_MARK="spadfs-test-mark-$$-$(date +%s%N)"
	echo "$KLOG_MARK" > /dev/kmsg
}
klog_since() {
	local all
	all=$(dmesg)
	if [ -z "$KLOG_MARK" ] || ! grep -qF "$KLOG_MARK" <<<"$all"; then
		echo "klog_since: test marker not in the kernel log (never set, or scrolled out)" >&2
		return 1
	fi
	sed -n "/$KLOG_MARK/,\$p" <<<"$all" | tail -n +2
}

# Scratch space, cleaned on exit.
WORKDIR="${TMPDIR:-/tmp}/spadfs-tests.$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT
