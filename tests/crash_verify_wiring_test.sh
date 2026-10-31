#!/usr/bin/env bash
# I5: test the crash_verify.sh WIRING, not just the library.
#
# The four attribution cells are decided by lib functions, but what the
# operator sees is produced by crash_verify.sh calling them in the right
# order and feeding the result into the final verdict. Deleting or reordering
# that call leaves every library test green while the guarantee is gone.
#
# Everything privileged is injected: fsck, mount, umount, modprobe, dmesg,
# findmnt, /proc/cmdline and /proc/iomem. No root, no device, no module.

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$TESTS_DIR/lib.sh"

assert_eq() {
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$3' want '$2'"; fi
}

# ---- shared fixtures -----------------------------------------------------
FIX="$WORKDIR/fix"
mkdir -p "$FIX"
dd if=/dev/zero of="$FIX/dev.img" bs=1k count=8 2>/dev/null

# cmdline with the reservation present / absent
printf '%s\n' 'BOOT_IMAGE=/vmlinuz root=UUID=x memmap=8M$0x103e000000 panic=10' \
	>"$FIX/cmd_reserved"
printf '%s\n' 'BOOT_IMAGE=/vmlinuz root=UUID=x loglevel=4' >"$FIX/cmd_bare"

# iomem proving / failing the reservation
cat >"$FIX/iomem_reserved" <<'EOF'
00000000-09cfefff : System RAM
103e000000-103e7fffff : Reserved
103e800000-103f37ffff : System RAM
EOF
cat >"$FIX/iomem_ram" <<'EOF'
100000000-103f37ffff : System RAM
EOF

# stub fsck: always clean
printf '#!/bin/sh\necho "checking super block..."\necho done.\nexit 0\n' >"$FIX/fsck"
# The mount stub needs the manifest; simpler: read it from an env file we write.
cat >"$FIX/mount" <<'EOF'
#!/usr/bin/env bash
# -t TYPE DEV MNT  -> materialise the manifest under MNT.
# The manifest's first field is the INDEX, exactly as crash_cycle.sh writes it;
# the on-disk name is c<CYCLE>_f<idx>.txt, which crash_verify.sh rebuilds.
mnt="${@: -1}"
list="${POPULATE_FROM:-}"
[ -n "$list" ] && [ -f "$list" ] || exit 1
while IFS=$'\t' read -r idx payload; do
	printf '%s\n' "$payload" >"$mnt/c${CYCLE_FOR_MOUNT}_f${idx}.txt"
done <"$list"
exit 0
EOF
chmod +x "$FIX/fsck" "$FIX/mount"
printf '#!/bin/sh\nexit 0\n' >"$FIX/umount"
chmod +x "$FIX/umount"
printf '#!/bin/sh\necho "/sys/fs/pstore pstore pstore rw"\n' >"$FIX/findmnt_ok"
printf '#!/bin/sh\nexit 1\n' >"$FIX/findmnt_no"
chmod +x "$FIX/findmnt_ok" "$FIX/findmnt_no"
printf '#!/bin/sh\nexit 0\n' >"$FIX/modprobe_ok"
chmod +x "$FIX/modprobe_ok"
printf '#!/bin/sh\necho "[ 5.0] ramoops: attached"\n' >"$FIX/dmesg"
chmod +x "$FIX/dmesg"

mkdir -p "$FIX/mods/ramoops" "$FIX/mods_empty"

# ---- one scenario ------------------------------------------------------
# scenario CYCLE MAKE_PLANNED CAPTURE [RECORD]
# CAPTURE is which boots did their part:
#   live       crashed boot ARMED for this cycle AND this boot can read pstore
#   read_only  this boot can read, but no ARMED record (cycle ran unarmed)
#   armed_only ARMED record, but this boot cannot read (landed on default)
#   absent     neither
# RECORD=panic drops a dmesg-ramoops record into the fake pstore dir.
# Prints the final VERDICT value.
scenario() {
	local cycle=$1 make_planned=$2 capture=$3 record=${4:-}
	local sd="$FIX/state_$cycle" mnt="$FIX/mnt_$cycle" ps="$FIX/ps_$cycle"
	mkdir -p "$sd" "$mnt" "$ps"
	echo "$cycle" >"$sd/cycle"

	# manifest: 2 files, named exactly as crash_cycle.sh names them
	: >"$sd/manifest.$cycle"
	local list="$sd/populate.$cycle"
	: >"$list"
	local i
	for i in 1 2; do
		printf '%s\tcycle %s file %s abc\n' "$i" "$cycle" "$i" >>"$sd/manifest.$cycle"
		printf '%s\tcycle %s file %s abc\n' "$i" "$cycle" "$i" >>"$list"
	done

	# boot_env is what tells the verifier which region to look for.
	bash -c ". '$TESTS_DIR/lib_crash_attr.sh'; boot_env_set '$sd' MEM_ADDR 0x103e000000; boot_env_set '$sd' MEM_SIZE 8M"

	case "$capture" in
	live | armed_only)
		bash -c ". '$TESTS_DIR/lib_crash_attr.sh'; armed_write '$sd' '$cycle'" ;;
	esac

	if [ "$make_planned" = yes ]; then
		bash -c ". '$TESTS_DIR/lib_crash_attr.sh'; planned_write '$sd' '$cycle' /dev/fake"
	fi

	if [ "$record" = panic ]; then
		printf '%s\n' 'Kernel panic - not syncing: spadfs2 went wrong' \
			>"$ps/dmesg-ramoops-0"
	fi

	local cmd_env=(
		STATE_DIR="$sd" MNT="$mnt" DEV="$FIX/dev.img"
		SPADFSCK="$FIX/fsck" MOUNT="$FIX/mount" UMOUNT="$FIX/umount"
		MOUNT_TYPE=faketype PSTORE_DIR="$ps"
		POPULATE_FROM="$list" CYCLE_FOR_MOUNT="$cycle"
	)
	if [ "$capture" = live ] || [ "$capture" = read_only ]; then
		cmd_env+=(
			CMDLINE="$FIX/cmd_reserved" IOMEM="$FIX/iomem_reserved"
			MODPROBE="$FIX/modprobe_ok" DMESG="$FIX/dmesg"
			FINDMNT="$FIX/findmnt_ok" SYS_MODULE_DIR="$FIX/mods"
		)
	else
		cmd_env+=(
			CMDLINE="$FIX/cmd_bare" IOMEM="$FIX/iomem_ram"
			MODPROBE="$FIX/modprobe_ok" DMESG="$FIX/dmesg"
			FINDMNT="$FIX/findmnt_no" SYS_MODULE_DIR="$FIX/mods_empty"
		)
	fi

	local out
	out=$(env "${cmd_env[@]}" bash "$TESTS_DIR/crash_verify.sh" 2>&1)
	printf '%s\n' "$out" | grep -E '^VERDICT: ' | tail -1 | sed 's/^VERDICT: //'
}

# ---- the four cells, with capture live -----------------------------------
assert_eq "planned + no record + capture live = PASS" "PASS" \
	"$(scenario 20 yes live)"
assert_eq "planned + record present = contradiction" "FAIL(crash)" \
	"$(scenario 21 yes live panic)"
assert_eq "unplanned + no record = no evidence" "FAIL(attribution)" \
	"$(scenario 22 no live)"
assert_eq "unplanned + record = confirmed crash" "FAIL(crash)" \
	"$(scenario 23 no live panic)"

# ---- and with the recorder DEAD (I2) ------------------------------------
# planned + none must NOT be a clean pass when nothing was recording
assert_eq "I2: planned + none + capture DEAD is not PASS" "FAIL(attribution-unverified)" \
	"$(scenario 24 yes absent)"
assert_eq "I2: unplanned + none + capture dead still no-evidence" "FAIL(attribution)" \
	"$(scenario 25 no absent)"

# ---- I8: capture must be proven on BOTH boots ----------------------------
# This boot can read pstore, but the crashed boot left no ARMED record: the
# old verifier called this live and passed it.
assert_eq "I8: planned + none + readable but never armed is not PASS" \
	"FAIL(attribution-unverified)" "$(scenario 26 yes read_only)"
# Armed, but the reboot landed where pstore cannot be read (the one-shot trap).
assert_eq "I8: planned + none + armed but unreadable is not PASS" \
	"FAIL(attribution-unverified)" "$(scenario 27 yes armed_only)"
# An ARMED record from another cycle must not vouch for this one.
sd28="$FIX/state_28"
mkdir -p "$sd28"
bash -c ". '$TESTS_DIR/lib_crash_attr.sh'; armed_write '$sd28' 99"
assert_eq "I8: an ARMED record for another cycle does not count" \
	"FAIL(attribution-unverified)" "$(scenario 28 yes read_only)"
# and the verifier consumes it, so it cannot vouch next time either
[ ! -e "$FIX/state_20/ARMED" ] && ok "I8: verify consumes the ARMED record" \
	|| bad "I8: ARMED record left behind after verify"

summary
