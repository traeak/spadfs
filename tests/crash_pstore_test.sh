#!/usr/bin/env bash
# Unprivileged tests for the pstore harvest / ramoops bring-up helpers.
#
# No kernel needed: the fixtures are text files and every external command
# (modprobe, dmesg) is injected. What we are proving is the *interpretation*
# of the evidence, which is where a wrong answer silently corrupts a verdict.
#
# Review Focus pinned here:
#   RF5  ramoops_ensure must FAIL when 'attached' is absent, so an empty
#        /sys/fs/pstore is never mistaken for "no crash happened"

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
# shellcheck disable=SC1091
. "$TESTS_DIR/crash_pstore.sh"

assert_eq() {
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$3' want '$2'"; fi
}

# ------------------------------------------------------- classify --------
PANIC_TEXT='
[  122.345678] sysrq: Trigger a crash
[  122.345700] Kernel panic - not syncing: sysrq triggered crash
[  122.345800] CPU: 3 PID: 4112 Comm: bash Tainted: G     U
[  122.345900] Call Trace:
[  122.346000]  <TASK>
'
OOPS_TEXT='
[   88.100000] BUG: unable to handle kernel NULL pointer dereference at 0000000000000010
[   88.100100] PGD 0 P4D 0
[   88.100200] Oops: 0000 [#1] SMP NOPTI
[   88.100300] Call Trace:
'
BENIGN='
[   12.000000] spadfs2: module load
[   13.000000] EXT4-fs: mounted
'

assert_eq "classify panic text" "panic" "$(pstore_classify "$PANIC_TEXT")"
assert_eq "classify oops text" "oops" "$(pstore_classify "$OOPS_TEXT")"
assert_eq "classify benign text" "none" "$(pstore_classify "$BENIGN")"
assert_eq "classify empty text" "none" "$(pstore_classify "")"

# a log with BOTH must report panic, not oops — panic is the stronger claim
assert_eq "classify prefers panic when both present" "panic" \
	"$(pstore_classify "$OOPS_TEXT
$PANIC_TEXT")"

# ------------------------------------------------------- harvest --------
PS_EMPTY="$WORKDIR/pstore_empty"
PS_ONE="$WORKDIR/pstore_one"
PS_TWO="$WORKDIR/pstore_two"
SD="$WORKDIR/state"
mkdir -p "$PS_EMPTY" "$PS_ONE" "$PS_TWO" "$SD"

printf '%s' "$PANIC_TEXT" >"$PS_ONE/dmesg-ramoops-0"
printf '%s' "$OOPS_TEXT" >"$PS_TWO/dmesg-ramoops-0"
printf '%s' "$PANIC_TEXT" >"$PS_TWO/dmesg-ramoops-1"
# an unrelated pstore file must not be harvested
printf 'console stuff' >"$PS_TWO/console-ramoops-0"

assert_eq "harvest on empty pstore reports none" "none" \
	"$(pstore_harvest "$PS_EMPTY" "$SD" 3)"
assert_eq "harvest on empty pstore creates no panic file" "no" \
	"$([ -e "$SD/panic.3.0.txt" ] && echo yes || echo no)"

assert_eq "harvest one record reports panic" "panic" \
	"$(pstore_harvest "$PS_ONE" "$SD" 4)"
assert_eq "harvest writes the cycle-named record" "yes" \
	"$([ -f "$SD/panic.4.0.txt" ] && echo yes || echo no)"
assert_contains() { case "$2" in *"$3"*) ok "$1";; *) bad "$1" "missing: $3";; esac; }
assert_contains "harvested record keeps the oops text" "$(cat "$SD/panic.4.0.txt")" \
	"Trigger a crash"

assert_eq "harvest two records reports panic" "panic" \
	"$(pstore_harvest "$PS_TWO" "$SD" 5)"
assert_eq "harvest copied record 0" "yes" "$([ -f "$SD/panic.5.0.txt" ] && echo yes || echo no)"
assert_eq "harvest copied record 1" "yes" "$([ -f "$SD/panic.5.1.txt" ] && echo yes || echo no)"
assert_eq "harvest ignores non-dmesg pstore files" "no" \
	"$([ -e "$SD/panic.5.console.txt" ] && echo yes || echo no)"

# harvest must never fail on absence — absence is a result, not an error
pstore_harvest "$PS_EMPTY" "$SD" 6 >/dev/null 2>&1
assert_eq "harvest returns 0 on empty pstore" "0" "$?"

# ------------------------------------------------- ramoops_ensure --------
# RF5: the attach assertion is the whole point. Without it, "no records" and
# "capture never worked" are indistinguishable.
#
# RULING (see ledger): the hard gate is deterministic kernel STATE — the
# ramoops module is loaded and pstore is mounted — not a grep of a log string.
# The exact "attached" wording is kernel-version dependent and unverifiable
# offline; a false negative there would block the entire harness. The dmesg
# line is reported as corroboration but never required.
MODPROBE_CALLS="$WORKDIR/modprobe.calls"
: >"$MODPROBE_CALLS"

stub_modprobe() { # exit code
	printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$*" >> "%s"\nexit %s\n' \
		"$MODPROBE_CALLS" "$1" >"$WORKDIR/stub-modprobe"
	chmod +x "$WORKDIR/stub-modprobe"
}
stub_dmesg() {
	printf '#!/usr/bin/env bash\ncat <<XEOF\n%s\nXEOF\n' "$1" >"$WORKDIR/stub-dmesg"
	chmod +x "$WORKDIR/stub-dmesg"
}
stub_findmnt() { # exit code
	printf '#!/usr/bin/env bash\n[ %s -eq 0 ] && echo "/sys/fs/pstore pstore pstore rw,nosuid"\nexit %s\n' \
		"$1" "$1" >"$WORKDIR/stub-findmnt"
	chmod +x "$WORKDIR/stub-findmnt"
}

MOD_OK="$WORKDIR/modules_ok"
MOD_NONE="$WORKDIR/modules_none"
mkdir -p "$MOD_OK/ramoops" "$MOD_NONE"
stub_dmesg "[    5.100000] ramoops: attached.0: base 0x103e000000, size 0x800000"
stub_findmnt 0

# The reservation must be on THIS boot's command line before we may point
# ramoops at an address. Loading over unreserved RAM writes into memory the
# kernel is actively using -- that is a machine-wrecking mistake, not a
# test failure, so it is gated here and asserted below.
CMD_RESERVED="$WORKDIR/cmdline_reserved"
CMD_BARE="$WORKDIR/cmdline_bare"
printf '%s\n' 'BOOT_IMAGE=/boot/vmlinuz root=UUID=x loglevel=4 memmap=8M$0x103e000000 panic=10' >"$CMD_RESERVED"
printf '%s\n' 'BOOT_IMAGE=/boot/vmlinuz root=UUID=x loglevel=4 nowatchdog' >"$CMD_BARE"

# run_ensure CMDLINE SYSMOD IOMEM  -> sets rc from ramoops_ensure
run_ensure() {
	rc=0
	CMDLINE="$1" MODPROBE="$WORKDIR/stub-modprobe" DMESG="$WORKDIR/stub-dmesg" \
		FINDMNT="$WORKDIR/stub-findmnt" SYS_MODULE_DIR="$2" IOMEM="$3" \
		ramoops_ensure 0x103e000000 8M >/dev/null 2>&1 || rc=$?
}

# iomem fixtures. Target range is 0x103e000000-0x103e7fffff (8 MiB).
IOMEM_RES="$WORKDIR/iomem_reserved"
IOMEM_RAM="$WORKDIR/iomem_systemram"
IOMEM_SPILL="$WORKDIR/iomem_spill"
IOMEM_NEST="$WORKDIR/iomem_nested"
IOMEM_NONE="$WORKDIR/iomem_none"
cat >"$IOMEM_RES" <<'EOF'
00000000-09cfefff : System RAM
100000000-103dffffff : System RAM
103e000000-103e7fffff : Reserved
103e800000-103f37ffff : System RAM
EOF
cat >"$IOMEM_RAM" <<'EOF'
00000000-09cfefff : System RAM
100000000-103f37ffff : System RAM
EOF
cat >"$IOMEM_SPILL" <<'EOF'
103e000000-103e3fffff : Reserved
103e400000-103f37ffff : System RAM
EOF
cat >"$IOMEM_NEST" <<'EOF'
100000000-103f37ffff : System RAM
  103e000000-103e7fffff : Kernel code
EOF
printf '00000000-00000fff : Reserved\n' >"$IOMEM_NONE"

# ---------------- C2a: the cmdline match must be token-exact ----------------
assert_eq "cmdline token exact match" "0" \
	"$(cmdline_has_memmap "$CMD_RESERVED" 8M 0x103e000000 >/dev/null 2>&1; echo $?)"
assert_eq "C2a: a PREFIX address must not satisfy the gate" "1" \
	"$(cmdline_has_memmap "$CMD_RESERVED" 8M 0x103e00000 >/dev/null 2>&1; echo $?)"
assert_eq "C2a: a SUFFIX-truncated size must not satisfy" "1" \
	"$(cmdline_has_memmap "$CMD_RESERVED" 8 0x103e000000 >/dev/null 2>&1; echo $?)"
assert_eq "cmdline gate fails on the bare cmdline" "1" \
	"$(cmdline_has_memmap "$CMD_BARE" 8M 0x103e000000 >/dev/null 2>&1; echo $?)"

# ---------------- C2b / I6: full-range reservation proof -------------------
rr() { region_reserved "$1" 0x103e000000 8388608 >/dev/null 2>&1; echo $?; }
assert_eq "fully Reserved over the whole range -> pass" "0" "$(rr "$IOMEM_RES")"
assert_eq "I6: whole range is System RAM -> fail" "1" "$(rr "$IOMEM_RAM")"
assert_eq "I6: tail of the range spills into System RAM -> fail" "1" "$(rr "$IOMEM_SPILL")"
assert_eq "I6: nested System RAM parent -> fail (child name must not save it)" "1" "$(rr "$IOMEM_NEST")"
assert_eq "I6: no covering entry -> fail (inconclusive is not green)" "1" "$(rr "$IOMEM_NONE")"

# happy path
stub_modprobe 0
run_ensure "$CMD_RESERVED" "$MOD_OK" "$IOMEM_RES"
assert_eq "ensure passes: cmdline token + reservation proven" "0" "$rc"
assert_contains "ensure passed mem_address" "$(cat "$MODPROBE_CALLS")" "mem_address=0x103e000000"
assert_contains "ensure passed mem_size (bytes, not '8M')" "$(cat "$MODPROBE_CALLS")" "mem_size=8388608"
assert_contains "ensure requested ecc" "$(cat "$MODPROBE_CALLS")" "ecc="

# SAFETY: no reservation on the command line -> refuse, and never call modprobe
: >"$MODPROBE_CALLS"
run_ensure "$CMD_BARE" "$MOD_NONE" "$IOMEM_RES"
assert_eq "SAFETY: refuses when the region is not reserved" "3" "$rc"
assert_eq "SAFETY: modprobe was NOT called when unreserved" "" "$(cat "$MODPROBE_CALLS")"

# SAFETY: the refusal must win even if a module is already loaded
: >"$MODPROBE_CALLS"
run_ensure "$CMD_BARE" "$MOD_OK" "$IOMEM_RES"
assert_eq "SAFETY: refusal wins over an already-loaded module" "3" "$rc"
assert_eq "SAFETY: still no modprobe call" "" "$(cat "$MODPROBE_CALLS")"

# C2a regression: prefix-only cmdline must be refused, not accepted
PREFIX_CMD="$WORKDIR/cmd_prefix"
printf '%s\n' 'BOOT_IMAGE=x memmap=8M$0x103e000000 panic=10' >"$PREFIX_CMD"
: >"$MODPROBE_CALLS"
rc=0
CMDLINE="$PREFIX_CMD" MODPROBE="$WORKDIR/stub-modprobe" DMESG="$WORKDIR/stub-dmesg" \
	FINDMNT="$WORKDIR/stub-findmnt" SYS_MODULE_DIR="$MOD_OK" IOMEM="$IOMEM_RAM" \
	ramoops_ensure 0x103e00000 8M >/dev/null 2>&1 || rc=$?
assert_eq "C2a: wrong (prefix) address is refused" "3" "$rc"
assert_eq "C2a: and modprobe still never ran" "" "$(cat "$MODPROBE_CALLS")"

# C2b regression: cmdline says reserved, iomem says System RAM -> refuse
: >"$MODPROBE_CALLS"
run_ensure "$CMD_RESERVED" "$MOD_OK" "$IOMEM_RAM"
assert_eq "C2b: cmdline intent but no actual reservation -> refuse" "3" "$rc"
assert_eq "C2b: modprobe never ran against live RAM" "" "$(cat "$MODPROBE_CALLS")"

# RF5: modprobe "succeeded" but the module is NOT actually loaded -> fail
stub_modprobe 0
run_ensure "$CMD_RESERVED" "$MOD_NONE" "$IOMEM_RES"
assert_eq "RF5: ensure FAILS when the module did not load" "1" "$rc"

# RF5: module loaded but pstore not mounted -> fail
stub_findmnt 1
run_ensure "$CMD_RESERVED" "$MOD_OK" "$IOMEM_RES"
assert_eq "RF5: ensure FAILS when pstore is not mounted" "1" "$rc"
stub_findmnt 0

# modprobe errors because it is already loaded -> success
stub_modprobe 1
run_ensure "$CMD_RESERVED" "$MOD_OK" "$IOMEM_RES"
assert_eq "already-loaded module is not an error" "0" "$rc"

# modprobe fails AND module absent -> error
stub_modprobe 1
run_ensure "$CMD_RESERVED" "$MOD_NONE" "$IOMEM_RES"
assert_eq "modprobe failure with no loaded module is an error" "1" "$rc"

# unreadable iomem must fail closed, not open
run_ensure "$CMD_RESERVED" "$MOD_OK" "$WORKDIR/does_not_exist"
assert_eq "unreadable /proc/iomem fails closed" "3" "$rc"

# ============ I1: harvested records must be erased from pstore ============
# pstore frees a RAM slot only when the record is unlinked. Copy without
# erase means the SAME panic is re-presented on every later mount: cycle N+1
# harvests cycle N's crash and files it under the new cycle's name. In a
# harness whose whole purpose is trustworthy attribution, that is the worst
# possible failure — it manufactures evidence.
I1_PS="$WORKDIR/i1_pstore"
I1_SD="$WORKDIR/i1_state"
mkdir -p "$I1_PS" "$I1_SD"
printf '%s' "$PANIC_TEXT" >"$I1_PS/dmesg-ramoops-0"

assert_eq "I1: first harvest sees the panic" "panic" "$(pstore_harvest "$I1_PS" "$I1_SD" 11)"
assert_eq "I1: copy exists in the state dir" "yes" \
	"$([ -f "$I1_SD/panic.11.0.txt" ] && echo yes || echo no)"
assert_eq "I1: source record erased from pstore after harvest" "no" \
	"$([ -e "$I1_PS/dmesg-ramoops-0" ] && echo yes || echo no)"
assert_eq "I1: second harvest sees nothing" "none" "$(pstore_harvest "$I1_PS" "$I1_SD" 12)"
assert_eq "I1: second cycle created no panic file" "no" \
	"$([ -e "$I1_SD/panic.12.0.txt" ] && echo yes || echo no)"
# the first cycle's copy must still be intact after the erase
assert_eq "I1: earlier copy survives" "yes" \
	"$([ -f "$I1_SD/panic.11.0.txt" ] && grep -q 'Trigger a crash' "$I1_SD/panic.11.0.txt" && echo yes || echo no)"

# erase must not delete unrelated files that happen to be in the dir
I1_PS2="$WORKDIR/i1_pstore2"
mkdir -p "$I1_PS2"
printf '%s' "$OOPS_TEXT" >"$I1_PS2/dmesg-ramoops-0"
printf 'keep me' >"$I1_PS2/console-ramoops-0"
pstore_harvest "$I1_PS2" "$I1_SD" 13 >/dev/null 2>&1
assert_eq "I1: non-dmesg pstore files are left alone" "yes" \
	"$([ -f "$I1_PS2/console-ramoops-0" ] && echo yes || echo no)"

# ------------------------------------------------ reboot_preflight --------
# The one-shot trap: the boot that got us onto the test entry used it up, so a
# cycle must prove capture is live AND re-arm before risking a reboot -- and
# each failure must stop the later steps.
PF_REBOOT_CALLS="$WORKDIR/pf-reboot.calls"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$*" >> "%s"\nexit 0\n' "$PF_REBOOT_CALLS" >"$WORKDIR/pf-reboot-ok"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$*" >> "%s"\nexit 1\n' "$PF_REBOOT_CALLS" >"$WORKDIR/pf-reboot-fail"
chmod +x "$WORKDIR/pf-reboot-ok" "$WORKDIR/pf-reboot-fail"

# run_pf NAME CMDLINE GRUB_REBOOT_STUB [no_env] -> sets pf_rc, pf_sd
run_pf() {
	pf_sd="$WORKDIR/pf-$1"
	rm -rf "$pf_sd"; mkdir -p "$pf_sd"
	: >"$PF_REBOOT_CALLS"
	if [ "${4:-}" != no_env ]; then
		boot_env_set "$pf_sd" MEM_ADDR 0x103e000000
		boot_env_set "$pf_sd" MEM_SIZE 8M
		boot_env_set "$pf_sd" ENTRY_TITLE "spadfs crash test"
	fi
	pf_rc=0
	CMDLINE="$2" MODPROBE="$WORKDIR/stub-modprobe" DMESG="$WORKDIR/stub-dmesg" \
		FINDMNT="$WORKDIR/stub-findmnt" SYS_MODULE_DIR="$MOD_OK" IOMEM="$IOMEM_RES" \
		GRUB_REBOOT="$3" reboot_preflight "$pf_sd" 7 >/dev/null 2>&1 || pf_rc=$?
}
stub_modprobe 0
stub_findmnt 0

run_pf good "$CMD_RESERVED" "$WORKDIR/pf-reboot-ok"
assert_eq "preflight: passes on a live, armable test boot" "0" "$pf_rc"
assert_eq "preflight: re-arms the test entry by its title" "spadfs crash test" \
	"$(cat "$PF_REBOOT_CALLS")"
assert_eq "preflight: records ARMED for this cycle" "live" "$(capture_at_crash "$pf_sd" 7)"

run_pf noenv "$CMD_RESERVED" "$WORKDIR/pf-reboot-ok" no_env
assert_eq "preflight: refuses without boot_env" "1" "$pf_rc"
assert_eq "preflight: no boot_env -> nothing armed" "" "$(cat "$PF_REBOOT_CALLS")"

# Not on the test boot: the reservation is missing, so capture is not live.
run_pf bare "$CMD_BARE" "$WORKDIR/pf-reboot-ok"
assert_eq "preflight: refuses when capture is not live" "1" "$pf_rc"
assert_eq "preflight: capture not live -> next boot NOT armed" "" "$(cat "$PF_REBOOT_CALLS")"
assert_eq "preflight: capture not live -> no ARMED record" "absent" "$(capture_at_crash "$pf_sd" 7)"

run_pf armfail "$CMD_RESERVED" "$WORKDIR/pf-reboot-fail"
assert_eq "preflight: refuses when arming fails" "1" "$pf_rc"
assert_eq "preflight: arming failed -> no ARMED record" "absent" "$(capture_at_crash "$pf_sd" 7)"

summary
