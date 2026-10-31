#!/usr/bin/env bash
# crash_attr_selfcheck.sh — prove the crash-capture path works BEFORE trusting
# any cycle's verdict.
#
# The whole point of this script is that every other check in the harness is
# only as good as the evidence pipeline behind it. A reboot that leaves no log
# and a reboot that never happened look identical unless something was put in
# place to record them — and putting it in place is exactly the step that can
# silently fail (the `$` eaten from memmap, the reservation not taken, the
# module loaded but nothing mounted).
#
# Run this on the armed test boot. Four checks, all must be OK:
#   1. the memory reservation is on THIS boot's command line, with a literal $
#   2. a panic timeout is set (CONFIG_PANIC_TIMEOUT=0 means a panic hangs here
#      forever, so an unattended crash test never comes back)
#   3. pstore is mounted and ramoops is loaded
#   4. the reserved region is not claimed as System RAM
#
# Then, deliberately and only with --i-am-throwaway, crash the machine on
# purpose and confirm the record actually arrives. Until that positive control
# has passed once, "no panic record" means nothing.
#
# Usage:
#   sudo tests/crash_attr_selfcheck.sh
#   sudo tests/crash_attr_selfcheck.sh --i-am-throwaway

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$TESTS_DIR/lib_crash_attr.sh"
# shellcheck disable=SC1091
. "$TESTS_DIR/crash_pstore.sh"

# ------------------------------------------------------ decisions ---------

# check_cmdline_reserved FILE SIZE ADDR
# Thin wrapper over the library's token-exact matcher. One implementation is
# shared with the load path so the self-check and ramoops_ensure can never
# disagree about what "reserved" means.
check_cmdline_reserved() {
	cmdline_has_memmap "$@"
}

# check_panic_timeout FILE
# panic=0 is the "hang forever" setting, not a timeout. Reject it explicitly.
# Last-wins, matching the kernel: a default `panic=0` followed by our appended
# `panic=10` has an effective timeout of 10, so reading the FIRST match would
# fail the check on a correctly armed boot.
check_panic_timeout() {
	local f=${1:-} val
	[ -r "$f" ] || return 1
	val=$(grep -oE '(^| )panic=[-0-9]+' "$f" | tail -1 | sed 's/.*panic=//')
	[ -n "$val" ] || return 1
	[ "$val" != "0" ] || return 1
	return 0
}

# check_region_not_system_ram IOMEM_FILE ADDR [SIZE_BYTES]
# Wrapper over the library's full-range overlap proof. The old local version
# checked only the start address and returned on the first containing entry,
# so a nested "Kernel code" child under a System RAM parent passed while the
# enclosing region was live memory (I6).
check_region_not_system_ram() {
	local f=${1:-} addr=${2:-} size=${3:-8388608}
	region_reserved "$f" "$addr" "$size"
}

# control_allowed HAS_FLAG CHECKS_GREEN
# The positive control panics the machine on purpose. It requires the flag AND
# a fully green self-check, so a mistyped invocation cannot crash a boot that
# is not prepared for it.
control_allowed() {
	local flag=${1:-0} green=${2:-0}
	[ "$flag" = "1" ] || return 1
	[ "$green" = "1" ] || return 1
	return 0
}

# control_record_found PSTORE_DIR
# Did a previous positive control leave its record? Prints the first
# dmesg-ramoops file containing the sysrq crash line. Read-only: the record
# is left for crash_verify.sh's harvest, which owns erasing it.
control_record_found() {
	local ps=${1:-} f
	[ -d "$ps" ] || return 1
	for f in "$ps"/dmesg-ramoops-*; do
		[ -e "$f" ] || continue
		if grep -qF 'sysrq: Trigger a crash' "$f" 2>/dev/null; then
			printf '%s\n' "$f"
			return 0
		fi
	done
	return 1
}

# ------------------------------------------------------------- main -------
main() {
	local throwaway=0
	[ "${1:-}" = "--i-am-throwaway" ] && throwaway=1
	[ $# -gt 1 ] && { echo "unknown arg: $1" >&2; exit 2; }

	local mem_addr mem_size fails=0
	mem_addr=$(boot_env_get "${STATE_DIR:-/var/lib/spadfs-crash}" MEM_ADDR)
	mem_size=$(boot_env_get "${STATE_DIR:-/var/lib/spadfs-crash}" MEM_SIZE)
	: "${mem_addr:=0x103e000000}" "${mem_size:=8M}"

	echo "==================== crash attribution self-check ===================="
	echo "reservation: $mem_size at $mem_addr"
	echo

	report() { # n name rc detail
		if [ "$3" -eq 0 ]; then
			printf '  OK    %s\n' "$2"
		else
			printf '  FAIL  %s\n        %s\n' "$2" "$4"
			fails=$((fails + 1))
		fi
	}

	check_cmdline_reserved /proc/cmdline "$mem_size" "$mem_addr"
	report 1 "1. memmap reservation on this boot's cmdline (literal \$)" $? \
		"expected memmap=${mem_size}\\\$${mem_addr} in /proc/cmdline"

	check_panic_timeout /proc/cmdline
	report 2 "panic= set to a non-zero value" $? \
		"PANIC_TIMEOUT=0 means a panic hangs here forever"

	SYS_MODULE_DIR="${SYS_MODULE_DIR:-/sys/module}" ramoops_ensure "$mem_addr" "$mem_size" \
		>/dev/null 2>&1
	report 3 "capture path live (ramoops loaded, pstore mounted)" $? \
		"see ramoops_ensure output"

	check_region_not_system_ram /proc/iomem "$mem_addr"
	report 4 "reserved region is not claimed as System RAM" $? \
		"grep the region in /proc/iomem; System RAM there means it was never reserved"

	local rec
	if rec=$(control_record_found "${PSTORE_DIR:-/sys/fs/pstore}"); then
		echo
		echo "  POSITIVE CONTROL CONFIRMED: a deliberate panic's record survived"
		echo "        the reboot: $rec"
	fi

	echo
	if [ "$fails" -ne 0 ]; then
		echo "SELF-CHECK: $fails check(s) FAILED. Do not run crash cycles."
		echo "The capture path is not trustworthy; absence of evidence would"
		echo "mean nothing. Fix, then re-run."
		return 1
	fi
	echo "SELF-CHECK: all checks passed. Capture path is live."

	if [ "$throwaway" -eq 0 ]; then
		echo
		echo "Positive control NOT run. To prove a panic is actually recorded:"
		echo "  sudo $0 --i-am-throwaway"
		echo "(this reboots the machine immediately)"
		return 0
	fi

	if ! control_allowed 1 1; then
		echo "control gate refused" >&2
		return 1
	fi

	# The one-shot was used up getting onto this boot. Without re-arming,
	# panic=10 reboots into the default entry, which cannot read the record
	# and is free to overwrite it: the control would "fail" with the capture
	# path working perfectly.
	if ! boot_arm "${STATE_DIR:-/var/lib/spadfs-crash}"; then
		echo "could not re-arm the test entry -- NOT panicking" >&2
		return 1
	fi

	echo
	echo "POSITIVE CONTROL: triggering a deliberate kernel panic in 3 seconds."
	echo "The test entry is re-armed, so the machine comes back on it. Then run"
	echo "  sudo $0"
	echo "and expect 'POSITIVE CONTROL CONFIRMED'."
	sleep 3
	echo 1 >/proc/sys/kernel/sysrq 2>/dev/null || true
	echo c >/proc/sysrq-trigger
	echo "sysrq-trigger returned without crashing — that is itself a failure." >&2
	return 1
}

# Only run when executed directly, so the tests can source the decisions above.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
	main "$@"
fi
