#!/usr/bin/env bash
# Unprivileged tests for the crash-attribution library.
#
# Everything here runs WITHOUT root: no mounts, no modprobe, no /dev/sda.
# The library takes STATE_DIR as an explicit argument everywhere precisely so
# these tests can point it at a temp directory.
#
# Review Focus items pinned by this file:
#   RF1  marker durability    -> "fsyncs the file AND the directory"
#   RF2  stale marker         -> "does not vouch for cycle N+1"
#   RF4  no-evidence != clean -> "reboot_verdict table"

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/lib_crash_attr.sh"

# Every assertion reports itself. No trailing unconditional `ok` — a group
# that prints PASS next to its own FAILs is a false-green generator.
assert_eq() { # name expected actual
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$3' want '$2'"; fi
}

SD="$WORKDIR/state"
mkdir -p "$SD"

# ------------------------------------------------ marker round-trip ----
planned_write "$SD" 7 /dev/sda1
assert_eq "planned_write creates the marker file" "yes" \
	"$([ -f "$SD/PLANNED" ] && echo yes || echo no)"
assert_eq "planned_cycle round-trips the cycle" "7" "$(planned_cycle "$SD")"
assert_eq "planned_read shows dev" "/dev/sda1" "$(planned_read "$SD" | sed -n 's/^dev=//p')"
assert_eq "planned_read shows a timestamp" "yes" \
	"$([ -n "$(planned_read "$SD" | sed -n 's/^ts=//p')" ] && echo yes || echo no)"

# ------------------------------------------------ planned_read exit ----
planned_consume "$SD"
rc=0
planned_read "$SD" >/dev/null 2>&1 || rc=$?
assert_eq "planned_read exits 1 when absent" "1" "$rc"

# ------------------------------- RF2: stale marker must not vouch ------
planned_write "$SD" 7 /dev/sda1
assert_eq "RF2: cycle 7 marker reads planned for cycle 7" "planned" \
	"$(planned_matches "$SD" 7)"
assert_eq "RF2: cycle 7 marker does NOT vouch for cycle 8" "unplanned" \
	"$(planned_matches "$SD" 8)"

planned_consume "$SD"
assert_eq "absent marker reads unplanned" "unplanned" "$(planned_matches "$SD" 1)"

# ------------------------------------------- RF1: fsync is real --------
# bash cannot open a directory fd, so the helper must use python3 and fsync
# BOTH the marker file and the containing directory. Without the directory
# fsync the marker can be absent after power loss, which converts a planned
# reboot into an unattributable one.
TRACE="$WORKDIR/fsync.trace"
strace -f -e trace=openat,fsync -o "$TRACE" \
	bash -c ". '$TESTS_DIR/lib_crash_attr.sh'; planned_write '$SD' 99 /dev/sda1" \
	>/dev/null 2>&1
N_FSYNC=$(grep -c 'fsync(' "$TRACE" || true)
assert_eq "RF1: at least two fsync calls (file + dir)" "yes" \
	"$([ "${N_FSYNC:-0}" -ge 2 ] && echo yes || echo "no ($N_FSYNC)")"
# Anchor on the STATE DIR specifically. A bare `grep O_DIRECTORY` passes on
# bash's own startup path lookups, which proves nothing about our fsync.
DIR_LINE=$(grep -F "$SD" "$TRACE" | grep 'O_DIRECTORY' | head -1)
DIR_FD=$(printf '%s' "$DIR_LINE" | sed -n 's/.*= \([0-9]\+\)$/\1/p')
assert_eq "RF1: the state directory itself is opened as a directory" "yes" \
	"$([ -n "$DIR_FD" ] && echo yes || echo no)"
assert_eq "RF1: fsync is called on the state directory's fd" "yes" \
	"$( [ -n "$DIR_FD" ] && grep -qF "fsync($DIR_FD)" "$TRACE" && echo yes || echo no )"

# ------------------------------------------- consume is idempotent -----
planned_write "$SD" 5 /dev/sda1
assert_eq "precondition: marker exists before consume" "yes" \
	"$([ -e "$SD/PLANNED" ] && echo yes || echo no)"
planned_consume "$SD"
assert_eq "planned_consume removes the marker" "no" \
	"$([ -e "$SD/PLANNED" ] && echo yes || echo no)"
planned_consume "$SD"
assert_eq "planned_consume on an already-absent marker is not an error" "0" "$?"

# ------------------------------------------- reboot_verdict table ------
# RF4 lives here: unplanned + no record must NEVER be a clean result.
assert_eq "verdict planned+none"        "PLANNED_CLEAN"         "$(reboot_verdict planned none)"
assert_eq "verdict planned+panic"      "PLANNED_CONTRADICTION" "$(reboot_verdict planned panic)"
assert_eq "verdict planned+oops"       "PLANNED_CONTRADICTION" "$(reboot_verdict planned oops)"
assert_eq "RF4: verdict unplanned+none is NOT clean" "CRASH_NO_EVIDENCE" "$(reboot_verdict unplanned none)"
assert_eq "verdict unplanned+panic"    "CRASH_CONFIRMED"       "$(reboot_verdict unplanned panic)"
assert_eq "verdict unplanned+oops"     "CRASH_CONFIRMED"       "$(reboot_verdict unplanned oops)"

# ------------------------------------------- boot_env last-wins --------
boot_env_set "$SD" MEM_ADDR 0x103e000000
boot_env_set "$SD" MEM_SIZE 8M
boot_env_set "$SD" MEM_ADDR 0x103e100000
assert_eq "boot_env last-wins" "0x103e100000" "$(boot_env_get "$SD" MEM_ADDR)"
assert_eq "boot_env keeps the other key" "8M" "$(boot_env_get "$SD" MEM_SIZE)"
assert_eq "boot_env unknown key is empty" "" "$(boot_env_get "$SD" NOPE)"

# ============================ Task 2: grub_entry_render ==================
assert_contains() { # name haystack needle
	case "$2" in
	*"$3"*) ok "$1" ;;
	*) bad "$1" "missing: $3" ;;
	esac
}
assert_absent() { # name haystack needle
	case "$2" in
	*"$3"*) bad "$1" "unexpectedly present: $3" ;;
	*) ok "$1" ;;
	esac
}

# The `$` trap. `memmap=8M$0x103e000000` is mangled twice before it reaches
# the kernel: the shell eats `$0` (the script's own name) and GRUB treats `$x`
# as variable expansion. The rendered block must carry a literal backslash-dollar.
# If it does not, the reservation is garbage and ramoops writes into live RAM —
# which presents later as random spadfs corruption, not as a config bug.
R_EXTRA='panic=10 softlockup_panic=1 hardlockup_panic=1'
OUT=$(grub_entry_render 'linux /boot/vmlinuz-test root=UUID=deadbeef loglevel=4' \
	'initrd /boot/initramfs-test.img' 0x103e000000 8M "$R_EXTRA")

# single-quoted needle => literal backslash + dollar
assert_contains "memmap carries a literal backslash-dollar" "$OUT" 'memmap=8M\$0x103e000000'
# the unescaped form must NOT be what we emit
assert_absent "memmap is not emitted unescaped" "$OUT" 'memmap=8M$0x103e000000'
# RF3: if $0 got expanded by the shell, a script path shows up in the args
assert_absent "shell did not expand \$0 into a script path" "$OUT" '.sh '
assert_contains "linux line preserved verbatim" "$OUT" 'root=UUID=deadbeef'
assert_contains "initrd line present" "$OUT" 'initrd /boot/initramfs-test.img'
assert_contains "panic timeout present" "$OUT" 'panic=10'
assert_contains "entry title is exact" "$OUT" "menuentry 'spadfs crash test'"

# markers must be on their own lines — Task 3 slices between them
assert_eq "BEGIN marker is a whole line" "1" \
	"$(printf '%s\n' "$OUT" | grep -c '^# BEGIN spadfs-crash-test$')"
assert_eq "END marker is a whole line" "1" \
	"$(printf '%s\n' "$OUT" | grep -c '^# END spadfs-crash-test$')"

# purity: same inputs, byte-identical output
OUT2=$(grub_entry_render 'linux /boot/vmlinuz-test root=UUID=deadbeef loglevel=4' \
	'initrd /boot/initramfs-test.img' 0x103e000000 8M "$R_EXTRA")
assert_eq "renderer is pure (two calls identical)" "$OUT" "$OUT2"

# the escape survives a write/read round trip without being eaten twice
RB_FILE="$WORKDIR/rendered.cfg"
printf '%s\n' "$OUT" >"$RB_FILE"
assert_contains "escape survives write+read round trip" "$(cat "$RB_FILE")" 'memmap=8M\$0x103e000000'

# --------------------- Task 2: the strip helper is token-exact ----------------
# A substring match would mangle a longer parameter. Pin the boundary behaviour.
assert_eq "strip removes the exact token" \
	"linux /boot/v root=UUID=x loglevel=4" \
	"$(grub_strip_token 'linux /boot/v root=UUID=x nowatchdog loglevel=4' nowatchdog)"
assert_eq "strip leaves a longer parameter alone" \
	"linux /boot/v nowatchdog_extra=1" \
	"$(grub_strip_token 'linux /boot/v nowatchdog_extra=1' nowatchdog)"
assert_eq "strip leaves a token containing it at the end alone" \
	"linux /boot/v nonowatchdog" \
	"$(grub_strip_token 'linux /boot/v nonowatchdog' nowatchdog)"
assert_eq "strip of an absent token is a no-op" \
	"linux /boot/v ro quiet" \
	"$(grub_strip_token 'linux /boot/v ro quiet' nowatchdog)"

# ------------------- Task 5: verify_verdict gating (attribution wins) ----
# Attribution outranks every other check. A filesystem that fsck calls clean
# is worthless if we cannot say what happened to the machine writing to it.
assert_eq "clean attribution + clean checks = PASS" "PASS" "$(verify_verdict PLANNED_CLEAN PASS)"
assert_eq "clean attribution carries the other verdict through" \
	"FAIL(durability)" "$(verify_verdict PLANNED_CLEAN 'FAIL(durability)')"
assert_eq "no-evidence attribution overrides a clean fsck" \
	"FAIL(attribution)" "$(verify_verdict CRASH_NO_EVIDENCE PASS)"
assert_eq "confirmed crash overrides a clean fsck" \
	"FAIL(crash)" "$(verify_verdict CRASH_CONFIRMED PASS)"
assert_eq "contradiction overrides a clean fsck" \
	"FAIL(crash)" "$(verify_verdict PLANNED_CONTRADICTION PASS)"
assert_eq "unknown attribution is not clean" \
	"FAIL(attribution)" "$(verify_verdict '' PASS)"

# =============== C1: the rendered block must be valid GRUB ===============
# A menuentry that never closes is not a cosmetic problem: grub-mkconfig
# concatenates 40_custom verbatim, so an unbalanced block lands in the
# bootloader of a machine that gets used every day.
C1_L='linux /boot/vmlinuz-t root=UUID=t'
C1_I='initrd /boot/initramfs-t.img'
C1B=$(grub_entry_render "$C1_L" "$C1_I" 0x103e000000 8M "panic=10")

assert_eq "C1: exactly one line ending in an opening brace" "1" \
	"$(printf '%s\n' "$C1B" | grep -c '{ *$')"
assert_eq "C1: exactly one closing brace line" "1" \
	"$(printf '%s\n' "$C1B" | grep -c '^}$')"
# the closing brace must come AFTER the initrd line and BEFORE the END marker
assert_eq "C1: brace closes between initrd and END marker" "yes" "$(printf '%s\n' "$C1B" | awk '
	/^\tinitrd/ { i = NR }
	/^}$/      { c = NR }
	/^# END spadfs-crash-test$/ { e = NR }
	END { print (i && c && e && c > i && c < e) ? "yes" : "no" }')"
# whole-block brace balance
assert_eq "C1: braces balanced across the block" "0" "$(printf '%s\n' "$C1B" | awk '
	{ o += gsub(/{/, "{"); cl += gsub(/}/, "}") }
	END { print o - cl }')"

# ============ I2: a dead recorder must not yield a clean verdict ============
# "planned + no panic record" is only meaningful if the recorder was working.
# With capture absent, no record proves nothing — the same reasoning that makes
# "unplanned + none" a failure. A caveat line that leaves the exit code at 0
# is a finding an operator skims past.
assert_eq "I2: planned+none with live capture is clean" "PLANNED_CLEAN" \
	"$(reboot_verdict planned none live)"
assert_eq "I2: planned+none with DEAD capture is not clean" "PLANNED_CLEAN_UNVERIFIED" \
	"$(reboot_verdict planned none absent)"
assert_eq "I2: a captured panic contradicts the marker either way" "PLANNED_CONTRADICTION" \
	"$(reboot_verdict planned panic absent)"
assert_eq "I2: unplanned+none is still no-evidence when capture was dead" "CRASH_NO_EVIDENCE" \
	"$(reboot_verdict unplanned none absent)"
# default (no third arg) stays live, so existing callers are unchanged
assert_eq "reboot_verdict defaults to live capture" "PLANNED_CLEAN" \
	"$(reboot_verdict planned none)"

assert_eq "I2: unverified clean can never PASS" "FAIL(attribution-unverified)" \
	"$(verify_verdict PLANNED_CLEAN_UNVERIFIED PASS)"
assert_eq "I2: unverified clean cannot mask a real failure either" "FAIL(attribution-unverified)" \
	"$(verify_verdict PLANNED_CLEAN_UNVERIFIED 'FAIL(durability)')"

# ============ I7: never pull the trigger without a durable marker ============
# "If the marker cannot be made durable, do NOT reboot" is the single most
# important consequence of RF1, and it had no test: the trigger was a
# hardcoded echo. planned_reboot takes the trigger path so this is provable.
TRIG="$WORKDIR/trigger"
BAD="$WORKDIR/badstate"
mkdir -p "$BAD"
chmod 555 "$BAD"

rc=0
planned_reboot "$BAD" 1 /dev/fake "$TRIG" >/dev/null 2>&1 || rc=$?
assert_eq "I7: marker failure aborts with non-zero" "1" "$rc"
assert_eq "I7: trigger was NEVER written when the marker failed" "no" \
	"$([ -e "$TRIG" ] && echo yes || echo no)"
chmod 755 "$BAD"

rc=0
planned_reboot "$BAD" 2 /dev/fake "$TRIG" >/dev/null 2>&1 || rc=$?
assert_eq "I7: durable marker -> proceeds" "0" "$rc"
assert_eq "I7: trigger received the reboot request" "b" "$(cat "$TRIG" 2>/dev/null)"
assert_eq "I7: marker is for the right cycle" "2" "$(planned_cycle "$BAD")"

# ------------------------------------------- arming: the one-shot trap ----
AS="$WORKDIR/arm-state"
mkdir -p "$AS"
ARM_CALLS="$WORKDIR/arm.calls"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$*" >> "%s"\nexit 0\n' "$ARM_CALLS" >"$WORKDIR/arm-ok"
printf '#!/usr/bin/env bash\nexit 1\n' >"$WORKDIR/arm-fail"
chmod +x "$WORKDIR/arm-ok" "$WORKDIR/arm-fail"

: >"$ARM_CALLS"
rc=0; GRUB_REBOOT="$WORKDIR/arm-ok" boot_arm "$AS" >/dev/null 2>&1 || rc=$?
assert_eq "boot_arm refuses without an ENTRY_TITLE" "1" "$rc"
assert_eq "boot_arm without a title calls nothing" "" "$(cat "$ARM_CALLS")"

boot_env_set "$AS" ENTRY_TITLE "spadfs crash test"
rc=0; GRUB_REBOOT="$WORKDIR/arm-ok" boot_arm "$AS" >/dev/null 2>&1 || rc=$?
assert_eq "boot_arm succeeds with a title" "0" "$rc"
assert_eq "boot_arm passes the title as ONE argument" "spadfs crash test" "$(cat "$ARM_CALLS")"
rc=0; GRUB_REBOOT="$WORKDIR/arm-fail" boot_arm "$AS" >/dev/null 2>&1 || rc=$?
assert_eq "boot_arm reports a grub-reboot failure" "1" "$rc"

# ARMED record: per-cycle, durable, consumable
assert_eq "no ARMED record -> capture absent" "absent" "$(capture_at_crash "$AS" 5)"
armed_write "$AS" 5
assert_eq "ARMED for cycle 5 -> capture live for 5" "live" "$(capture_at_crash "$AS" 5)"
assert_eq "ARMED for cycle 5 does not vouch for 6" "absent" "$(capture_at_crash "$AS" 6)"
TRACE2="$WORKDIR/armed.trace"
strace -f -e trace=fsync -o "$TRACE2" \
	bash -c ". '$TESTS_DIR/lib_crash_attr.sh'; armed_write '$AS' 9" >/dev/null 2>&1
n=$(grep -c 'fsync(' "$TRACE2" 2>/dev/null)
[ "${n:-0}" -ge 2 ] && ok "armed_write fsyncs the file and the directory ($n fsyncs)" \
	|| bad "armed_write fsync count" "saw ${n:-0}, want >= 2"
armed_consume "$AS"; armed_consume "$AS"
assert_eq "armed_consume is idempotent and leaves nothing" "no" \
	"$([ -e "$AS/ARMED" ] && echo yes || echo no)"

summary
