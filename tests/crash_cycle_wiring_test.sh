#!/usr/bin/env bash
# I9: test the crash_cycle.sh WIRING of the reboot preflight.
#
# The library and reboot_preflight have their own tests, but a cycle that never
# calls the preflight -- or calls it after the writes -- leaves every one of
# them green while the one-shot trap is back: the reboot lands on the default
# entry and nothing can be verified. So drive the real script.
#
# Root-only commands are faked on PATH (id, mount, umount, mountpoint) and the
# sysrq trigger is a file, so nothing is mounted and nothing reboots.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
# shellcheck disable=SC1091
. "$TESTS_DIR/lib_crash_attr.sh"

assert_eq() {
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$3' want '$2'"; fi
}

BIN="$WORKDIR/bin"
mkdir -p "$BIN"
printf '#!/bin/sh\necho 0\n' >"$BIN/id"
printf '#!/bin/sh\nexit 0\n' >"$BIN/mount"
printf '#!/bin/sh\nexit 0\n' >"$BIN/umount"
printf '#!/bin/sh\nexit 1\n' >"$BIN/mountpoint"
ARM_CALLS="$WORKDIR/arm.calls"
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "%s"\nexit 0\n' "$ARM_CALLS" >"$WORKDIR/grub-reboot"
chmod +x "$BIN"/* "$WORKDIR/grub-reboot"

# capture-path fixtures: a live test boot
printf '%s\n' 'BOOT_IMAGE=/vmlinuz root=UUID=x memmap=8M$0x103e000000 panic=10' >"$WORKDIR/cmdline"
printf '%s\n' '00000000-09cfefff : System RAM' '103e000000-103e7fffff : Reserved' >"$WORKDIR/iomem"
mkdir -p "$WORKDIR/mods/ramoops"
printf '#!/bin/sh\nexit 0\n' >"$WORKDIR/modprobe"
printf '#!/bin/sh\necho "/sys/fs/pstore pstore pstore rw"\n' >"$WORKDIR/findmnt"
printf '#!/bin/sh\nexit 0\n' >"$WORKDIR/dmesg"
chmod +x "$WORKDIR/modprobe" "$WORKDIR/findmnt" "$WORKDIR/dmesg"

# run_cycle STATE_DIR [extra env...] -- crash_cycle.sh --reboot, faked root
run_cycle() {
	local sd=$1; shift
	: >"$ARM_CALLS"
	: >"$WORKDIR/trigger"
	env PATH="$BIN:$PATH" STATE_DIR="$sd" DEV=/dev/fake-crash MNT="$WORKDIR/mnt-$RANDOM" \
		NFILES=2 SYSRQ_TRIGGER="$WORKDIR/trigger" GRUB_REBOOT="$WORKDIR/grub-reboot" \
		CMDLINE="$WORKDIR/cmdline" IOMEM="$WORKDIR/iomem" MODPROBE="$WORKDIR/modprobe" \
		FINDMNT="$WORKDIR/findmnt" DMESG="$WORKDIR/dmesg" SYS_MODULE_DIR="$WORKDIR/mods" \
		"$@" bash "$TESTS_DIR/crash_cycle.sh" --reboot >"$WORKDIR/cycle.out" 2>&1
}

# ---- refused: not set up (no boot_env), so capture cannot be proven ------
SD1="$WORKDIR/state1"
mkdir -p "$SD1"
echo 4 >"$SD1/cycle"
rc=0; run_cycle "$SD1" || rc=$?
assert_eq "refused cycle exits non-zero" "1" "$rc"
assert_eq "refused cycle burns no cycle number" "4" "$(cat "$SD1/cycle")"
assert_eq "refused cycle arms nothing" "" "$(cat "$ARM_CALLS")"
assert_eq "refused cycle writes no manifest" "no" "$([ -e "$SD1/manifest.5" ] && echo yes || echo no)"
assert_eq "refused cycle pulls no trigger" "" "$(cat "$WORKDIR/trigger")"

# ---- refused: set up, but this boot is not the test boot ----------------
SD2="$WORKDIR/state2"
mkdir -p "$SD2"
echo 4 >"$SD2/cycle"
boot_env_set "$SD2" MEM_ADDR 0x103e000000
boot_env_set "$SD2" MEM_SIZE 8M
boot_env_set "$SD2" ENTRY_TITLE "spadfs crash test"
printf '%s\n' 'BOOT_IMAGE=/vmlinuz root=UUID=x nowatchdog' >"$WORKDIR/cmdline-default"
rc=0; run_cycle "$SD2" CMDLINE="$WORKDIR/cmdline-default" || rc=$?
assert_eq "default boot: cycle refused" "1" "$rc"
assert_eq "default boot: next boot not armed" "" "$(cat "$ARM_CALLS")"
assert_eq "default boot: no cycle number burned" "4" "$(cat "$SD2/cycle")"

# ---- accepted: live test boot ------------------------------------------
rc=0; run_cycle "$SD2" || rc=$?
# the trigger is a file, so "reboot did not happen" -> exit 1 is expected
assert_eq "accepted cycle reaches the trigger" "b" "$(cat "$WORKDIR/trigger")"
assert_eq "accepted cycle is cycle 5" "5" "$(cat "$SD2/cycle")"
assert_eq "accepted cycle re-armed the test entry" "spadfs crash test" "$(cat "$ARM_CALLS")"
assert_eq "accepted cycle recorded ARMED for cycle 5" "live" "$(capture_at_crash "$SD2" 5)"
assert_eq "accepted cycle wrote the PLANNED marker for cycle 5" "planned" "$(planned_matches "$SD2" 5)"
grep -q 'preflight ok' "$WORKDIR/cycle.out" \
	&& ok "accepted cycle reports the preflight" \
	|| bad "no preflight line" "$(tail -5 "$WORKDIR/cycle.out")"
# ordering: the preflight line must precede the first write
pf=$(grep -n 'preflight ok' "$WORKDIR/cycle.out" | head -1 | cut -d: -f1)
mt=$(grep -n 'mounted ok' "$WORKDIR/cycle.out" | head -1 | cut -d: -f1)
[ -n "$pf" ] && [ -n "$mt" ] && [ "$pf" -lt "$mt" ] \
	&& ok "preflight runs before the volume is mounted" \
	|| bad "preflight/mount order" "preflight line ${pf:-none}, mount line ${mt:-none}"

summary
