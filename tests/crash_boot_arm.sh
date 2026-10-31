#!/usr/bin/env bash
# crash_boot_arm.sh — select the crash-test entry for exactly ONE boot.
#
# grub-reboot sets `next_entry` in grubenv. GRUB honours it unconditionally on
# the next boot and then clears it, so this needs no change to GRUB_DEFAULT and
# cannot persist by accident. The boot after that is your normal entry again.
#
# Usage: sudo tests/crash_boot_arm.sh

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "$TESTS_DIR/lib_crash_attr.sh"

GRUB_REBOOT=${GRUB_REBOOT:-grub-reboot}
GRUB_EDITENV=${GRUB_EDITENV:-grub-editenv}
GRUBENV=${GRUBENV:-/boot/grub/grubenv}
STATE_DIR=${STATE_DIR:-/var/lib/spadfs-crash}

TITLE=$(boot_env_get "$STATE_DIR" ENTRY_TITLE)
if [ -z "$TITLE" ]; then
	echo "no ENTRY_TITLE in $STATE_DIR/boot_env — run crash_boot_setup.sh --apply first" >&2
	exit 1
fi

echo "arming one-shot boot of: '$TITLE'"
if [ -f "$GRUBENV" ]; then
	echo "--- grubenv before ---"
	"$GRUB_EDITENV" "$GRUBENV" list 2>&1 || true
fi

"$GRUB_REBOOT" "$TITLE" || { echo "grub-reboot failed" >&2; exit 1; }

if [ -f "$GRUBENV" ]; then
	echo "--- grubenv after ---"
	"$GRUB_EDITENV" "$GRUBENV" list 2>&1 || true
fi

echo "armed. The NEXT boot uses '$TITLE'; the one after that is your default."
