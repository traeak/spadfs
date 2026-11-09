#!/usr/bin/env bash
# Shell-test lint: two patterns that silently turn checks into false passes.
#
# 1. Piping into `grep -q`.
#
# Every root and crash script here runs under `set -o pipefail` (or is
# sourced into one). There, `producer | grep -q pattern` takes the
# producer's exit status: spadfsck exits non-zero whenever it reports
# anything, and a producer writing more than a pipe buffer dies of SIGPIPE
# once grep -q has matched. Either way a match reads as "no match" -- a false
# FAIL on a presence check and a false PASS on an absence check. That cost a
# root run of meta_csum_mount.sh 4 bogus failures and hid the same hole in
# cc_wrap, crash_pstore and crash_verify.
#
# Capture the output, then grep a here-string:
#     out=$(cmd); grep -q pattern <<<"$out"

set -uo pipefail
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

#
# 2. Counting kernel log lines (`dmesg | wc -l`, then `tail -n +N`). Once
# the ring buffer is full the count stops moving and the window comes back
# empty; absence checks then pass whatever the kernel said. Use klog_mark /
# klog_since from lib.sh. That cost cct_mirror_mount.sh two bogus failures
# after a crash_logwrites.sh run, and hid the same hole in cc_wrap_mount.sh
# and cct_crc_mount.sh.

rc=0
src() {
	grep -nE "$1" "$TESTS_DIR"/*.sh |
		grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' |
		grep -v '/lint_shell.sh:'
}

hits=$(src '\|[[:space:]]*grep[[:space:]]+(-[A-Za-z]*q|--quiet)')
if [ -n "$hits" ]; then
	printf 'FAIL  pipe into grep -q (wrong under pipefail; capture then grep <<<):\n%s\n' "$hits"
	rc=1
else
	printf '  PASS  no shell test pipes into grep -q\n'
fi

hits=$(src 'dmesg[[:space:]]*\|[[:space:]]*wc[[:space:]]+-l')
if [ -n "$hits" ]; then
	printf 'FAIL  kernel log window by line count (use klog_mark/klog_since):\n%s\n' "$hits"
	rc=1
else
	printf '  PASS  no kernel log window by line count\n'
fi
exit "$rc"
