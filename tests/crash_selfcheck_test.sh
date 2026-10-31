#!/usr/bin/env bash
# Unprivileged tests for the self-check decision functions.
#
# The self-check itself runs on real hardware and reboots it, so the parts that
# DECIDE are extracted here and tested against fixture files. What ships is the
# same logic these tests exercise.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
# shellcheck disable=SC1091
. "$TESTS_DIR/crash_attr_selfcheck.sh"

assert_eq() {
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$3' want '$2'"; fi
}
run() { # name expected actual_rc
	assert_eq "$1" "$2" "$3"
}

RESERVED="$WORKDIR/cmd_reserved"
BARE="$WORKDIR/cmd_bare"
printf '%s\n' 'BOOT_IMAGE=/boot/vmlinuz root=UUID=x loglevel=4 mitigations=off memmap=8M$0x103e000000 panic=10 softlockup_panic=1' >"$RESERVED"
printf '%s\n' 'BOOT_IMAGE=/boot/vmlinuz root=UUID=x loglevel=4 nowatchdog' >"$BARE"

# ------------------------------------------- check 1: literal $ ----------
rc=0
check_cmdline_reserved "$RESERVED" 8M 0x103e000000 >/dev/null 2>&1 || rc=$?
run "cmdline reservation present" "0" "$rc"

rc=0
check_cmdline_reserved "$BARE" 8M 0x103e000000 >/dev/null 2>&1 || rc=$?
run "cmdline reservation absent -> fail" "1" "$rc"

# a mangled reservation (the $ eaten) must NOT pass
MANGLED="$WORKDIR/cmd_mangled"
printf '%s\n' 'BOOT_IMAGE=/boot/vmlinuz memmap=8M0x103e000000 panic=10' >"$MANGLED"
rc=0
check_cmdline_reserved "$MANGLED" 8M 0x103e000000 >/dev/null 2>&1 || rc=$?
run "RF3: mangled memmap (no dollar) -> fail" "1" "$rc"

# wrong address must not pass
rc=0
check_cmdline_reserved "$RESERVED" 8M 0x9999000000 >/dev/null 2>&1 || rc=$?
run "wrong address does not satisfy the check" "1" "$rc"

# ------------------------------------------- check 2: panic timeout ------
rc=0
check_panic_timeout "$RESERVED" >/dev/null 2>&1 || rc=$?
run "panic= present" "0" "$rc"

rc=0
check_panic_timeout "$BARE" >/dev/null 2>&1 || rc=$?
run "no panic= -> fail (PANIC_TIMEOUT=0 hangs forever)" "1" "$rc"

# panic=0 is NOT a timeout — it means "hang"
ZERO="$WORKDIR/cmd_panic0"
printf '%s\n' 'root=UUID=x panic=0' >"$ZERO"
rc=0
check_panic_timeout "$ZERO" >/dev/null 2>&1 || rc=$?
run "panic=0 rejected (that is the hang setting)" "1" "$rc"

# ------------------------------------------- check 4: iomem ------------
IOMEM_OK="$WORKDIR/iomem_ok"
IOMEM_RAM="$WORKDIR/iomem_ram"
IOMEM_NONE="$WORKDIR/iomem_none"
cat >"$IOMEM_OK" <<'EOF'
00000000-00000fff : Reserved
01000000-09cfefff : System RAM
100000000-103e7fffff : Reserved
103e800000-103f37ffff : System RAM
EOF
cat >"$IOMEM_RAM" <<'EOF'
00000000-00000fff : Reserved
100000000-103f37ffff : System RAM
EOF
cat >"$IOMEM_NONE" <<'EOF'
00000000-00000fff : Reserved
EOF

rc=0
check_region_not_system_ram "$IOMEM_OK" 0x103e000000 >/dev/null 2>&1 || rc=$?
run "region covered by a Reserved entry -> ok" "0" "$rc"

rc=0
check_region_not_system_ram "$IOMEM_RAM" 0x103e000000 >/dev/null 2>&1 || rc=$?
run "region still System RAM -> fail (not reserved)" "1" "$rc"

rc=0
check_region_not_system_ram "$IOMEM_NONE" 0x103e000000 >/dev/null 2>&1 || rc=$?
run "no covering entry -> fail (inconclusive)" "1" "$rc"

# ------------------------------- positive-control gate (sysrq -c) --------
# Three states: no flag, flag with failed checks, flag with passed checks.
rc=0
control_allowed 0 0 >/dev/null 2>&1 || rc=$?
run "control refused without the flag" "1" "$rc"

rc=0
control_allowed 1 0 >/dev/null 2>&1 || rc=$?
run "control refused when checks failed" "1" "$rc"

rc=0
control_allowed 1 1 >/dev/null 2>&1 || rc=$?
run "control allowed only with flag AND green checks" "0" "$rc"

rc=0
control_allowed 0 1 >/dev/null 2>&1 || rc=$?
run "green checks alone never arm the control" "1" "$rc"

# ------------------------------------- positive control: record found ----
PS="$WORKDIR/pstore"
mkdir -p "$PS"
rc=0; control_record_found "$PS" >/dev/null 2>&1 || rc=$?
run "no records -> control not confirmed" "1" "$rc"
printf '%s\n' '[ 9.0] Kernel panic - not syncing: something else' >"$PS/dmesg-ramoops-0"
rc=0; control_record_found "$PS" >/dev/null 2>&1 || rc=$?
run "an unrelated panic is not the control" "1" "$rc"
printf '%s\n' '[ 9.1] sysrq: Trigger a crash' '[ 9.1] Kernel panic - not syncing: sysrq triggered crash' >"$PS/dmesg-ramoops-1"
rc=0; found=$(control_record_found "$PS" 2>/dev/null) || rc=$?
run "the sysrq control record is found" "0" "$rc"
run "and named" "$PS/dmesg-ramoops-1" "$found"
run "and left in place for the harvest" "yes" "$([ -f "$PS/dmesg-ramoops-1" ] && echo yes || echo no)"

summary
