#!/usr/bin/env bash
# crash_boot_setup.sh — create the one-shot "spadfs crash test" GRUB entry.
#
# WHY ONE-SHOT
#
# Catching a kernel panic needs a reserved RAM region (memmap=) plus a panic
# timeout, because CONFIG_PANIC_TIMEOUT=0 on this host means a real panic
# hangs the machine forever. We do not want those arguments on every boot of
# the machine you work on — so they go on a dedicated menu entry, and
# crash_boot_arm.sh selects that entry for exactly one boot via grub-reboot.
# The default entry is never modified.
#
# DRY RUN BY DEFAULT. Nothing is written until --apply.
#
# Every path and command is env-overridable so the unprivileged test suite
# (tests/crash_boot_test.sh) can drive this against stubs.
#
# Usage: sudo tests/crash_boot_setup.sh [--apply]

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$TESTS_DIR/lib_crash_attr.sh"

GRUB_CUSTOM=${GRUB_CUSTOM:-/etc/grub.d/40_custom}
GRUB_CFG=${GRUB_CFG:-/boot/grub/grub.cfg}
GRUB_MKCONFIG=${GRUB_MKCONFIG:-grub-mkconfig}
GRUB_REBOOT=${GRUB_REBOOT:-grub-reboot}
STATE_DIR=${STATE_DIR:-/var/lib/spadfs-crash}

# From the spec: 8 MiB at 0x103e000000, just below the top of System RAM
# (0x103f380000), inside the largest contiguous range, 16 MiB aligned.
MEM_ADDR=${MEM_ADDR:-0x103e000000}
MEM_SIZE=${MEM_SIZE:-8M}

# The test entry's policy arguments. panic=10 because PANIC_TIMEOUT=0 means a
# panic otherwise never comes back. The lockup panics turn a hang into a
# captured event.
EXTRA_ARGS=${EXTRA_ARGS:-"panic=10 softlockup_panic=1 hardlockup_panic=1"}

# Tokens that must NOT be carried over from the default command line.
# `nowatchdog` disables the lockup detectors GLOBALLY, so leaving it in place
# makes softlockup_panic=1 and hardlockup_panic=1 inert — the entry looks
# correct in grub.cfg and catches nothing. Review Focus 8.
STRIP_TOKENS=${STRIP_TOKENS:-"nowatchdog"}

APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1
[ $# -gt 1 ] && { echo "unknown arg: $1" >&2; exit 2; }

# Only demand root when pointed at the real bootloader.
if [ "$GRUB_CUSTOM" = "/etc/grub.d/40_custom" ] && [ "$(id -u)" -ne 0 ]; then
	echo "must run as root when using the real $GRUB_CUSTOM" >&2
	exit 1
fi

[ -f "$GRUB_CFG" ] || { echo "no grub config at $GRUB_CFG" >&2; exit 1; }

# ---- take the default entry's kernel and initrd, as a PAIR from one block ----
# Two independent scans could pair kernel A with initramfs B, or pick up a
# recovery entry. See grub_default_entry.
DEFAULT_PAIR=$(grub_default_entry "$GRUB_CFG") || {
	echo "could not find a menuentry with both a linux and an initrd line in $GRUB_CFG" >&2
	exit 1
}
DEFAULT_TITLE=$(printf '%s\n' "$DEFAULT_PAIR" | sed -n 's/^title=//p')
LINUX_LINE=$(printf '%s\n' "$DEFAULT_PAIR" | sed -n 's/^linux=//p')
INITRD_LINE=$(printf '%s\n' "$DEFAULT_PAIR" | sed -n 's/^initrd=//p')
[ -n "$LINUX_LINE" ] || { echo "no linux line in the chosen entry" >&2; exit 1; }
[ -n "$INITRD_LINE" ] || { echo "no initrd line in the chosen entry" >&2; exit 1; }

echo "cloning entry : '$DEFAULT_TITLE'"
echo "default kernel line: $LINUX_LINE"
echo "default initrd line: $INITRD_LINE"
echo "  (confirm this is the kernel you intend to crash-test)"

# ---- strip the tokens that would defeat our own arguments ----
STRIPPED="$LINUX_LINE"
for tok in $STRIP_TOKENS; do
	before=$STRIPPED
	STRIPPED=$(grub_strip_token "$STRIPPED" "$tok")
	if [ "$before" = "$STRIPPED" ]; then
		echo "note: '$tok' not present on the default line, nothing stripped"
	else
		echo "stripped '$tok' from the test entry"
	fi
done

# ---- render ----
BLOCK=$(grub_entry_render "$STRIPPED" "$INITRD_LINE" "$MEM_ADDR" "$MEM_SIZE" "$EXTRA_ARGS")

echo
echo "----- block for $GRUB_CUSTOM -----"
printf '%s\n' "$BLOCK"
echo "--------------------------------"

if [ "$APPLY" -eq 0 ]; then
	echo
	echo "DRY RUN — nothing written. Re-run with --apply."
	echo "would upsert into: $GRUB_CUSTOM"
	echo "would run:         $GRUB_MKCONFIG -o $GRUB_CFG"
	echo "would record:      MEM_ADDR=$MEM_ADDR MEM_SIZE=$MEM_SIZE ENTRY_TITLE=$GRUB_ENTRY_TITLE in $STATE_DIR/boot_env"
	exit 0
fi

echo
# ---- back up before touching the bootloader (G4: reversible) ----
# grub-mkconfig output depends on the installed grub version, os-prober results
# and any hand edits. "Rebuild and hope" is not a rollback.
BACKUP_DIR="$STATE_DIR/backup"
mkdir -p "$BACKUP_DIR" || { echo "cannot create $BACKUP_DIR" >&2; exit 1; }
# Only the FIRST backup is the pristine one. Overwriting it on a second
# --apply would store a copy that already contains our block, and teardown
# would "restore" the thing it was supposed to remove.
[ -f "$BACKUP_DIR/40_custom.orig" ] || { [ -f "$GRUB_CUSTOM" ] && cp -f "$GRUB_CUSTOM" "$BACKUP_DIR/40_custom.orig"; }
[ -f "$BACKUP_DIR/grub.cfg.orig" ] || cp -f "$GRUB_CFG" "$BACKUP_DIR/grub.cfg.orig"
boot_env_set "$STATE_DIR" BACKUP_DIR "$BACKUP_DIR"
echo "backed up: $BACKUP_DIR/40_custom.orig, $BACKUP_DIR/grub.cfg.orig"

grub_block_upsert "$GRUB_CUSTOM" "$BLOCK" || { echo "upsert failed" >&2; exit 1; }
echo "upserted block into $GRUB_CUSTOM"

# shellcheck disable=SC2086
"$GRUB_MKCONFIG" -o "$GRUB_CFG" || { echo "grub-mkconfig failed" >&2; exit 1; }
echo "regenerated $GRUB_CFG"

boot_env_set "$STATE_DIR" MEM_ADDR "$MEM_ADDR"
boot_env_set "$STATE_DIR" MEM_SIZE "$MEM_SIZE"
boot_env_set "$STATE_DIR" ENTRY_TITLE "$GRUB_ENTRY_TITLE"
boot_env_set "$STATE_DIR" GRUB_CUSTOM "$GRUB_CUSTOM"
boot_env_set "$STATE_DIR" GRUB_CFG "$GRUB_CFG"
echo "recorded boot_env in $STATE_DIR"

# ---- name the failure mode precisely before the generic grep gate ----
# A non-executable fragment is skipped by grub-mkconfig's `test -x` filter,
# which produces EXACTLY the same "the $ was eaten" message as a genuinely
# eaten dollar. Check the bit first so the two causes are never confused.
if [ ! -x "$GRUB_CUSTOM" ]; then
	echo "FAIL: $GRUB_CUSTOM is not executable (mode $(stat -c '%a' "$GRUB_CUSTOM" 2>/dev/null))" >&2
	echo "      grub-mkconfig only reads grub.d fragments that pass \`test -x\`," >&2
	echo "      so the entry never reaches $GRUB_CFG. This is NOT an escaping bug." >&2
	exit 1
fi

# ---- prove the escape survived into the generated config ----
# The needle must contain a literal backslash followed by a literal dollar.
# In double quotes that is  \\  then  \$  :  \\ -> backslash,  \$ -> dollar.
# Writing only \\ drops the dollar entirely, giving "memmap=8M\0x..." which
# matches NOTHING -- so this gate rejects a CORRECT config. The sibling
# crash_attr_selfcheck.sh:106 has the right form; this one did not.
if grep -qF "memmap=${MEM_SIZE}\\\$${MEM_ADDR}" "$GRUB_CFG"; then
	echo "OK: generated grub.cfg contains the escaped memmap"
else
	echo "FAIL: generated grub.cfg does NOT contain memmap=${MEM_SIZE}\\\${MEM_ADDR}" >&2
	echo "      the \$ was eaten somewhere. Do not proceed." >&2
	exit 1
fi

echo
echo "next: sudo tests/crash_boot_arm.sh   (selects it for the next boot only)"
