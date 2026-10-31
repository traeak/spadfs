#!/usr/bin/env bash
# Unprivileged tests for the GRUB boot drivers.
#
# The drivers take every external path and command from env-overridable
# variables (GRUB_CUSTOM, GRUB_CFG, GRUB_MKCONFIG, GRUB_REBOOT) so this file
# can exercise the real driver scripts against stubs, with no root, no GRUB,
# and no risk to the machine's bootloader.
#
# Review Focus pinned here:
#   RF6  apply twice must not duplicate the entry
#   RF7  teardown removes exactly the block; default entry unchanged
#   RF8  nowatchdog must not survive into the test entry

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"
. "$TESTS_DIR/lib_crash_attr.sh"

assert_eq() { # name expected actual
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$3' want '$2'"; fi
}
assert_contains() {
	case "$2" in
	*"$3"*) ok "$1" ;;
	*) bad "$1" "missing: $3" ;;
	esac
}
assert_absent() {
	case "$2" in
	*"$3"*) bad "$1" "unexpectedly present: $3" ;;
	*) ok "$1" ;;
	esac
}

T="$WORKDIR/grub"
SD="$WORKDIR/state"
mkdir -p "$T" "$SD"

# ---- fixture: the real default entry, with nowatchdog on the command line ----
cat >"$T/default_entry" <<'EOF'
menuentry 'Void Linux' --class gnu-linux --class gnu --class os {
	linux /boot/vmlinuz-7.2.12_1 root=UUID=688aa855-b9c6-48c8-a8d1-7dd0f7333cdf ro loglevel=4 mitigations=off nowatchdog amdgpu.ppfeaturemask=0xffffffff amd_pstate=active
	initrd /boot/initramfs-7.2.12_1.img
}
EOF
cp "$T/default_entry" "$T/grub.cfg"
# The real /etc/grub.d/40_custom is mode 755 with a 3-line header. The stub
# below mirrors grub-mkconfig's `test -x` filter, so the fixture must match
# reality or the suite cannot see the skip.
cat >"$T/40_custom" <<'EOF'
#!/bin/sh
exec tail -n +3 $0
# This file provides an easy way to add custom menu entries.
EOF
chmod 755 "$T/40_custom"

# ---- stub grub-mkconfig: default entry + 40_custom ONLY IF EXECUTABLE ----
# Mirrors /usr/bin/grub-mkconfig:289 --
#   if grub_file_is_not_garbage "$i" && test -x "$i" ; then
# A stub that cats 40_custom unconditionally hides the exec-bit bug entirely.
cat >"$T/stub-mkconfig" <<EOF
#!/usr/bin/env bash
out=""
while [ \$# -gt 0 ]; do [ "\$1" = "-o" ] && { out="\$2"; shift 2; continue; }; shift; done
[ -n "\$out" ] || exit 2
{ cat "$T/default_entry"
  if [ -x "$T/40_custom" ]; then
      cat "$T/40_custom"
  else
      echo "# SKIPPED: 40_custom not executable"
  fi
} > "\$out"
echo "Generating grub configuration file ... done"
EOF
chmod +x "$T/stub-mkconfig"

# ---- stub grub-reboot: record the invocation ----
cat >"$T/stub-reboot" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$T/reboot.calls"
EOF
chmod +x "$T/stub-reboot"

# every driver call goes through this wrapper so the env is identical
run_setup() {
	GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_MKCONFIG="$T/stub-mkconfig" GRUB_REBOOT="$T/stub-reboot" \
	STATE_DIR="$SD" "$TESTS_DIR/crash_boot_setup.sh" "$@"
}
run_teardown() {
	GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_MKCONFIG="$T/stub-mkconfig" GRUB_REBOOT="$T/stub-reboot" \
	STATE_DIR="$SD" "$TESTS_DIR/crash_boot_teardown.sh"
}
run_arm() {
	GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_MKCONFIG="$T/stub-mkconfig" GRUB_REBOOT="$T/stub-reboot" \
	STATE_DIR="$SD" "$TESTS_DIR/crash_boot_arm.sh"
}

# --------------------------------- dry-run changes nothing -----------------
before_custom=$(md5sum <"$T/40_custom")
before_cfg=$(md5sum <"$T/grub.cfg")
run_setup >/dev/null 2>&1
assert_eq "dry-run leaves 40_custom untouched" "$before_custom" "$(md5sum <"$T/40_custom")"
assert_eq "dry-run leaves grub.cfg untouched" "$before_cfg" "$(md5sum <"$T/grub.cfg")"
assert_eq "dry-run writes no boot_env" "no" \
	"$([ -e "$SD/boot_env" ] && echo yes || echo no)"

# --------------------------------- --apply inserts the block ---------------
# The exit status is load-bearing: crash_boot_setup.sh gates on its own
# "did the escape survive into the generated config" check. Swallowing it
# here is how a broken gate reads as green.
apply_rc=0
run_setup --apply >/dev/null 2>&1 || apply_rc=$?
assert_eq "--apply exits 0 (its own escape gate passes)" "0" "$apply_rc"
# The gate must be able to fail: a stub mkconfig that eats the backslash
# (simulating the bug this whole check exists to catch) must make --apply
# exit non-zero. Without this, a grep that always matches looks identical to
# a grep that works.
cat >"$T/stub-mkconfig-lost-dollar" <<EOF
#!/usr/bin/env bash
out=""
while [ \$# -gt 0 ]; do [ "\$1" = "-o" ] && { out="\$2"; shift 2; continue; }; shift; done
[ -n "\$out" ] || exit 2
{ cat "$T/default_entry"; cat "$T/40_custom" 2>/dev/null; } | tr -d '\\\\' >"\$out"
EOF
chmod +x "$T/stub-mkconfig-lost-dollar"
teeth_rc=0
GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_MKCONFIG="$T/stub-mkconfig-lost-dollar" GRUB_REBOOT="$T/stub-reboot" \
	STATE_DIR="$WORKDIR/teeth_state" "$TESTS_DIR/crash_boot_setup.sh" --apply >/dev/null 2>&1 || teeth_rc=$?
assert_eq "gate fails when the escape is lost (check has teeth)" "1" "$teeth_rc"
# restore a good config for the rest of the suite
run_setup --apply >/dev/null 2>&1
CUSTOM=$(cat "$T/40_custom")
assert_contains "block inserted" "$CUSTOM" '# BEGIN spadfs-crash-test'
assert_contains "memmap escaped in the file" "$CUSTOM" 'memmap=8M\$0x103e000000'
# The generated config, not just the source file, must carry the backslash.
assert_contains "memmap escaped in the GENERATED grub.cfg" "$(cat "$T/grub.cfg")" 'memmap=8M\$0x103e000000'
# RF9: grub-mkconfig only reads /etc/grub.d/* that pass `test -x`. A tmp+mv
# rewrite inherits the umask (644) and the entry silently vanishes from the
# generated config while 40_custom itself still looks perfect.
assert_eq "RF9: 40_custom is still executable after --apply" "yes" \
	"$([ -x "$T/40_custom" ] && echo yes || echo no)"
assert_absent "RF9: grub-mkconfig did not skip 40_custom" "$(cat "$T/grub.cfg")" 'SKIPPED'
# RF9c: a fragment ALREADY broken to 644 must be repaired, not faithfully
# preserved. Preserving 644 preserves the breakage.
chmod 644 "$T/40_custom"
run_setup --apply >/dev/null 2>&1
assert_eq "RF9c: --apply repairs a 644 fragment to executable" "yes" \
	"$([ -x "$T/40_custom" ] && echo yes || echo no)"
assert_absent "RF9c: repaired fragment reaches grub.cfg" "$(cat "$T/grub.cfg")" 'SKIPPED'
# A deliberately strict 0700 must be respected, not widened to 755.
chmod 700 "$T/40_custom"
run_setup --apply >/dev/null 2>&1
assert_eq "RF9c: a stricter 0700 is preserved, not widened" "700" \
	"$(stat -c '%a' "$T/40_custom")"
assert_contains "entry title present" "$CUSTOM" "menuentry 'spadfs crash test'"
assert_contains "initrd carried over" "$CUSTOM" 'initrd /boot/initramfs-7.2.12_1.img'
assert_contains "panic timeout set" "$CUSTOM" 'panic=10'

# ------------------------------- RF8: nowatchdog must be gone --------------
TEST_LINE=$(printf '%s\n' "$CUSTOM" | sed -n '/spadfs crash test/,/^}/p' | grep 'linux ')
assert_absent "RF8: nowatchdog stripped from the test entry" "$TEST_LINE" 'nowatchdog'
assert_contains "RF8: softlockup_panic present" "$TEST_LINE" 'softlockup_panic=1'
assert_contains "RF8: hardlockup_panic present" "$TEST_LINE" 'hardlockup_panic=1'
# ...and the DEFAULT entry still has its nowatchdog — we must not touch it
assert_contains "default entry keeps its own nowatchdog" \
	"$(cat "$T/grub.cfg")" 'Void Linux'

# --------------------------------- boot_env recorded ----------------------
assert_eq "boot_env MEM_ADDR" "0x103e000000" "$(boot_env_get "$SD" MEM_ADDR)"
assert_eq "boot_env MEM_SIZE" "8M" "$(boot_env_get "$SD" MEM_SIZE)"
assert_eq "boot_env ENTRY_TITLE" "spadfs crash test" "$(boot_env_get "$SD" ENTRY_TITLE)"

# ------------------------------- RF6: apply twice, one block --------------
run_setup --apply >/dev/null 2>&1
assert_eq "RF6: exactly one BEGIN after two applies" "1" \
	"$(grep -c '^# BEGIN spadfs-crash-test$' "$T/40_custom")"
assert_eq "RF6: exactly one END after two applies" "1" \
	"$(grep -c '^# END spadfs-crash-test$' "$T/40_custom")"
assert_eq "RF6: exactly one menuentry for the test" "1" \
	"$(grep -c "menuentry 'spadfs crash test'" "$T/40_custom")"

# ------------------------------- arm passes the title ---------------------
run_arm >/dev/null 2>&1
assert_contains "arm passes the title from boot_env" "$(cat "$T/reboot.calls")" 'spadfs crash test'

# ------------------------------- RF7: teardown ---------------------------
run_teardown >/dev/null 2>&1
assert_absent "RF7: no markers left in 40_custom" "$(cat "$T/40_custom")" 'spadfs-crash-test'
assert_absent "RF7: no test entry left in grub.cfg" "$(cat "$T/grub.cfg")" "menuentry 'spadfs crash test'"
# the default entry survived byte-identical
awk '/^menuentry .Void Linux/,/^}/' "$T/grub.cfg" >"$T/default_entry.after"
cmp -s "$T/default_entry" "$T/default_entry.after" \
	&& ok "RF7: default entry byte-identical" \
	|| bad "RF7: default entry changed" "$(diff "$T/default_entry" "$T/default_entry.after" | head -5)"

# teardown is idempotent
run_teardown >/dev/null 2>&1
assert_eq "teardown is idempotent" "0" "$?"
assert_absent "teardown twice still clean" "$(cat "$T/40_custom")" 'spadfs-crash-test'

# ============ I3: kernel and initrd must be paired from ONE block ============
# Two independent "first match in the file" scans can pair kernel A with
# initramfs B, or pick up a recovery entry. The test then silently exercises
# a different kernel than the one under test.
PAIR_CFG="$WORKDIR/pair.cfg"
cat >"$PAIR_CFG" <<'EOF'
menuentry 'Fallback recovery' --class gnu-linux {
	linux /boot/vmlinuz-recovery root=/dev/ram0 single
}
menuentry 'Void Linux' --class gnu-linux {
	linux /boot/vmlinuz-7.2.12_1 root=UUID=abc nowatchdog
	initrd /boot/initramfs-7.2.12_1.img
}
EOF
PAIR=$(grub_default_entry "$PAIR_CFG")
assert_eq "I3: title comes from the block that has both lines" "Void Linux" \
	"$(printf '%s\n' "$PAIR" | sed -n 's/^title=//p')"
assert_eq "I3: linux line paired with its own block" \
	"linux /boot/vmlinuz-7.2.12_1 root=UUID=abc nowatchdog" \
	"$(printf '%s\n' "$PAIR" | sed -n 's/^linux=//p')"
assert_eq "I3: initrd line paired with the SAME block" \
	"initrd /boot/initramfs-7.2.12_1.img" \
	"$(printf '%s\n' "$PAIR" | sed -n 's/^initrd=//p')"
case "$(printf '%s\n' "$PAIR" | sed -n 's/^linux=//p')" in
*recovery*) bad "I3: recovery kernel must not be selected" ;;
*) ok "I3: recovery entry not selected" ;;
esac

# The pairing invariant: whatever block is chosen, the linux and initrd must
# name the SAME kernel. Checked across a fixture where the blocks differ.
ORPHAN_CFG="$WORKDIR/orphan.cfg"
cat >"$ORPHAN_CFG" <<'EOF'
menuentry 'Only kernel' {
	linux /boot/vmlinuz-x root=UUID=x
}
menuentry 'Other' {
	linux /boot/vmlinuz-y root=UUID=y
	initrd /boot/initramfs-y.img
}
EOF
O_L=$(grub_default_entry "$ORPHAN_CFG" | sed -n 's/^linux=//p')
O_I=$(grub_default_entry "$ORPHAN_CFG" | sed -n 's/^initrd=//p')
O_KV=$(printf '%s' "$O_L" | sed -n 's|.*/vmlinuz-\([^ ]*\).*|\1|p')
O_KI=$(printf '%s' "$O_I" | sed -n 's|.*/initramfs-\([^ ]*\)\.img|\1|p')
assert_eq "I3: chosen linux and initrd name the same kernel" "$O_KV" "$O_KI"
assert_eq "I3: the paired block is 'Other', not the initrd-less first block" \
	"Other" "$(grub_default_entry "$ORPHAN_CFG" | sed -n 's/^title=//p')"

# ============ I4: apply must back up; teardown must not rebuild blindly ====
assert_eq "I4: --apply saved a 40_custom backup" "yes" \
	"$([ -f "$SD/backup/40_custom.orig" ] && echo yes || echo no)"
assert_eq "I4: --apply saved a grub.cfg backup" "yes" \
	"$([ -f "$SD/backup/grub.cfg.orig" ] && echo yes || echo no)"

# A marker stub: records that grub-mkconfig ran, at an absolute injected path.
cat >"$WORKDIR/stub-marker" <<EOF
#!/usr/bin/env bash
[ -n "\${MARKER:-}" ] && : >"\$MARKER"
out=""
while [ \$# -gt 0 ]; do [ "\$1" = "-o" ] && { out="\$2"; shift 2; continue; }; shift; done
[ -n "\$out" ] || exit 2
{ cat "$T/default_entry"; cat "$T/40_custom" 2>/dev/null; } >"\$out"
EOF
chmod +x "$WORKDIR/stub-marker"

# teardown with a backup present restores 40_custom from it
MARKER="$WORKDIR/mk1" GRUB_MKCONFIG="$WORKDIR/stub-marker" \
	GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_REBOOT="$T/stub-reboot" STATE_DIR="$SD" \
	"$TESTS_DIR/crash_boot_teardown.sh" >/dev/null 2>&1
assert_eq "I4: teardown restored 40_custom from the backup" \
	"$(cat "$SD/backup/40_custom.orig")" "$(cat "$T/40_custom")"

# RF9b: the exact state this bug produced on real hardware -- 40_custom was
# clobbered to 644 by an earlier upsert, the pristine backup is 755.
# `cp -f` onto an EXISTING destination keeps the destination's mode, so a
# naive restore gives back the right bytes and the wrong exec bit, and
# grub-mkconfig still skips the fragment.
chmod 644 "$T/40_custom"
assert_eq "RF9b: fixture is 644 before restore" "644" "$(stat -c '%a' "$T/40_custom")"
assert_eq "RF9b: backup is 755" "755" "$(stat -c '%a' "$SD/backup/40_custom.orig")"
MARKER="$WORKDIR/mk1b" GRUB_MKCONFIG="$WORKDIR/stub-marker" \
	GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_REBOOT="$T/stub-reboot" STATE_DIR="$SD" \
	"$TESTS_DIR/crash_boot_teardown.sh" >/dev/null 2>&1
assert_eq "RF9b: restore from a 755 backup leaves 40_custom executable" "yes" \
	"$([ -x "$T/40_custom" ] && echo yes || echo no)"

# teardown when NOTHING was installed must not touch the bootloader at all
cp "$T/default_entry" "$T/grub.cfg"
: >"$T/40_custom"
rm -rf "$SD/backup"
MARKER="$WORKDIR/mk2" GRUB_MKCONFIG="$WORKDIR/stub-marker" \
	GRUB_CUSTOM="$T/40_custom" GRUB_CFG="$T/grub.cfg" \
	GRUB_REBOOT="$T/stub-reboot" STATE_DIR="$SD" \
	"$TESTS_DIR/crash_boot_teardown.sh" >/dev/null 2>&1
assert_eq "I4: nothing removed -> grub-mkconfig never ran" "no" \
	"$([ -e "$WORKDIR/mk2" ] && echo yes || echo no)"

summary

