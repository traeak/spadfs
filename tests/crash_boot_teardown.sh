#!/usr/bin/env bash
# crash_boot_teardown.sh — remove the crash-test GRUB entry and restore the host.
#
# Reverses crash_boot_setup.sh completely: the marked block goes, grub.cfg is
# regenerated without it, and the boot_env record is cleared. Idempotent —
# running it when nothing was ever installed is a no-op, not an error.
#
# Usage: sudo tests/crash_boot_teardown.sh

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$TESTS_DIR/lib_crash_attr.sh"

GRUB_CUSTOM=${GRUB_CUSTOM:-/etc/grub.d/40_custom}
GRUB_CFG=${GRUB_CFG:-/boot/grub/grub.cfg}
GRUB_MKCONFIG=${GRUB_MKCONFIG:-grub-mkconfig}
STATE_DIR=${STATE_DIR:-/var/lib/spadfs-crash}

if [ "$GRUB_CUSTOM" = "/etc/grub.d/40_custom" ] && [ "$(id -u)" -ne 0 ]; then
	echo "must run as root when using the real $GRUB_CUSTOM" >&2
	exit 1
fi

if grub_block_present "$GRUB_CUSTOM"; then
	grub_block_remove "$GRUB_CUSTOM" || { echo "remove failed" >&2; exit 1; }
	REMOVED=1
	echo "removed the marked block from $GRUB_CUSTOM"
else
	REMOVED=0
	echo "no marked block in $GRUB_CUSTOM — nothing to remove"
fi

BACKUP_DIR=${BACKUP_DIR:-$STATE_DIR/backup}
if [ "$REMOVED" -eq 0 ] && [ ! -f "$BACKUP_DIR/grub.cfg.orig" ]; then
	# Nothing was installed and we have no record of what was there before.
	# Regenerating the operator's bootloader on that basis is exactly the
	# "teardown leaves things worse than found" failure. Don't.
	echo "nothing removed and no backup present — leaving $GRUB_CFG untouched"
	exit 0
fi

# Prefer restoring the exact bytes we found over rebuilding them. grub-mkconfig
# output depends on the installed grub version, os-prober results and any hand
# edits, so a regeneration is not a rollback.
if [ -f "$BACKUP_DIR/40_custom.orig" ]; then
	cp -f "$BACKUP_DIR/40_custom.orig" "$GRUB_CUSTOM"
	# `cp -f` onto an EXISTING destination keeps the DESTINATION's mode, so if
	# a previous upsert left this fragment 644 the restore would hand back the
	# right bytes and the wrong exec bit -- and grub-mkconfig's `test -x`
	# filter would keep skipping it. Take the mode from the backup explicitly.
	_grub_chmod_like "$BACKUP_DIR/40_custom.orig" "$GRUB_CUSTOM" \
		|| { echo "could not restore the exec bit on $GRUB_CUSTOM" >&2; exit 1; }
	echo "restored $GRUB_CUSTOM from backup"
fi
if [ -f "$BACKUP_DIR/grub.cfg.orig" ]; then
	cp -f "$BACKUP_DIR/grub.cfg.orig" "$GRUB_CFG"
	echo "restored $GRUB_CFG from backup (no regeneration)"
else
	"$GRUB_MKCONFIG" -o "$GRUB_CFG" || { echo "grub-mkconfig failed" >&2; exit 1; }
	echo "regenerated $GRUB_CFG"
fi

if grep -qF 'spadfs-crash-test' "$GRUB_CFG" 2>/dev/null; then
	echo "FAIL: a crash-test marker is still in $GRUB_CFG" >&2
	exit 1
fi
if grep -qF "menuentry '$GRUB_ENTRY_TITLE'" "$GRUB_CFG" 2>/dev/null; then
	echo "FAIL: the test entry is still in $GRUB_CFG" >&2
	exit 1
fi
echo "OK: no test entry and no markers in $GRUB_CFG"

rm -f "$STATE_DIR/boot_env"
echo "cleared $STATE_DIR/boot_env"

# A stale next_entry survives teardown and would boot the (now removed) test
# entry once. Clear it, and warn if ramoops is still holding the reservation.
if [ -n "${GRUBENV:-}" ] && [ -f "${GRUBENV:-}" ] && command -v grub-editenv >/dev/null 2>&1; then
	grub-editenv "$GRUBENV" unset next_entry 2>/dev/null || true
fi
if [ -e /sys/module/ramoops ]; then
	echo "WARNING: ramoops is still loaded. Release the reservation before teardown finishes:"
	echo "         sudo rmmod ramoops"
fi
echo "teardown complete."
