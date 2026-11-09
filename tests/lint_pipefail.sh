#!/usr/bin/env bash
# Fails if a shell test pipes into `grep -q`.
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

hits=$(grep -nE '\|[[:space:]]*grep[[:space:]]+(-[A-Za-z]*q|--quiet)' "$TESTS_DIR"/*.sh |
	grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' |
	grep -v '/lint_pipefail.sh:')
if [ -n "$hits" ]; then
	printf 'FAIL  pipe into grep -q (wrong under pipefail; capture then grep <<<):\n%s\n' "$hits"
	exit 1
fi
printf '  PASS  no shell test pipes into grep -q\n'
