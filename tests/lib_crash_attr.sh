# shellcheck shell=bash
# lib_crash_attr.sh — crash-attribution primitives for the spadfs2 crash harness.
#
# Sourced, never executed. No side effects beyond function definitions, and no
# global STATE_DIR: every function takes the state directory as an argument so
# the unprivileged test suite can point it at a temp dir.
#
# WHY THIS EXISTS
#
# crash_cycle.sh ends with `echo b >/proc/sysrq-trigger`. That kills the script
# mid-flight, so "the log just stops" is the only signal we get — and it looks
# identical whether the harness asked for the reboot or the kernel panicked.
# Every crash cycle was therefore unfalsifiable. These functions make the two
# cases distinguishable, and make "we captured nothing" impossible to mistake
# for "nothing happened".
#
# The marker lives OUTSIDE the filesystem under test, same reason the manifest
# does: a record stored on the subject cannot vouch for the subject.

# Directory fsync. bash cannot open a directory fd, so this goes through
# python3. Without it the rename is durable but the *directory entry* is not,
# and a power loss can drop the marker entirely — which converts a planned
# reboot into an unattributable one. That is strictly worse than not writing
# a marker, because it silently inverts the verdict.
fsync_dir() {
	python3 -c 'import os,sys
fd = os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECTORY)
os.fsync(fd)
os.close(fd)' "$1"
}

# Regular-file fsync, same rationale: we must be able to make our own copy
# durable before erasing the only other copy of the evidence.
fsync_file() {
	python3 -c 'import os,sys
fd = os.open(sys.argv[1], os.O_WRONLY)
os.fsync(fd)
os.close(fd)' "$1"
}

# Write a durable "this reboot is mine" marker.
# Callers MUST treat a non-zero return as a reason not to reboot: proceeding
# without a durable marker destroys the ability to attribute the reboot.
planned_write() {
	local sd=${1:-} cycle=${2:-} dev=${3:-} tmp
	[ -n "$sd" ] && [ -n "$cycle" ] || return 2
	mkdir -p "$sd" || return 1
	tmp="$sd/PLANNED.tmp.$$"
	{
		printf 'cycle=%s\n' "$cycle"
		printf 'dev=%s\n' "$dev"
		printf 'ts=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} >"$tmp" || return 1
	python3 -c 'import os,sys
fd = os.open(sys.argv[1], os.O_WRONLY)
os.fsync(fd)
os.close(fd)' "$tmp" || return 1
	# rename, not in-place: a half-written marker is worse than none.
	mv -f "$tmp" "$sd/PLANNED" || return 1
	fsync_dir "$sd" || return 1
	return 0
}

# Print the marker contents. Exit 1 when absent.
planned_read() {
	local sd=${1:-}
	[ -f "$sd/PLANNED" ] || return 1
	cat "$sd/PLANNED"
}

planned_cycle() {
	local sd=${1:-}
	[ -f "$sd/PLANNED" ] || return 0
	sed -n 's/^cycle=//p' "$sd/PLANNED" | head -1
}

# The whole point of RF2: a marker left over from cycle N must never vouch for
# cycle N+1. Matching is on the cycle number, not on mere existence.
planned_matches() {
	local sd=${1:-} want=${2:-}
	if [ -f "$sd/PLANNED" ] && [ "$(planned_cycle "$sd")" = "$want" ]; then
		printf 'planned\n'
	else
		printf 'unplanned\n'
	fi
}

planned_consume() {
	local sd=${1:-}
	[ -d "$sd" ] || return 0
	rm -f "$sd/PLANNED" || return 1
	fsync_dir "$sd" || return 1
	return 0
}

# ---------------------------------------------------------------- boot env --
# One source of truth for the reservation address/size and the GRUB entry title,
# so no other script hardcodes them. Last write wins.
boot_env_set() {
	local sd=${1:-} key=${2:-} val=${3:-} envf tmp
	[ -n "$sd" ] && [ -n "$key" ] || return 2
	mkdir -p "$sd" || return 1
	envf="$sd/boot_env"
	tmp="$sd/.boot_env.$$"
	{
		[ -f "$envf" ] && grep -v "^${key}=" "$envf" || true
		printf '%s=%s\n' "$key" "$val"
	} >"$tmp" || return 1
	mv -f "$tmp" "$envf" || return 1
	fsync_dir "$sd" || return 1
	return 0
}

boot_env_get() {
	local sd=${1:-} key=${2:-}
	[ -f "$sd/boot_env" ] || return 0
	sed -n "s/^${key}=//p" "$sd/boot_env" | tail -1
}

# --------------------------------------------------------------- verdicts ---
# The attribution table from the spec. Note that `panic` and `oops` both mean
# "a record was captured"; only `none` is the absence of evidence.
#
#   planned   + none  -> the harness asked for this reboot and nothing crashed.
#   planned   + rec   -> the marker claimed a clean reboot but the kernel
#                       oopsed anyway. Trust the record, not the marker.
#   unplanned + rec   -> a real crash.
#   unplanned + none  -> NOT clean. The box hung, lost power, or died harder
#                       than the capture path can record.
reboot_verdict() {
	local planned=${1:-unplanned} pstore=${2:-none} capture=${3:-live}
	case "$planned/$pstore" in
	planned/none)
		# No record is only reassuring if something was there to record it.
		if [ "$capture" = "live" ]; then
			printf 'PLANNED_CLEAN\n'
		else
			printf 'PLANNED_CLEAN_UNVERIFIED\n'
		fi
		;;
	planned/panic | planned/oops) printf 'PLANNED_CONTRADICTION\n' ;;
	unplanned/panic | unplanned/oops) printf 'CRASH_CONFIRMED\n' ;;
	unplanned/none) printf 'CRASH_NO_EVIDENCE\n' ;;
	*) printf 'CRASH_NO_EVIDENCE\n' ;;
	esac
}

# The gating rule for the verifier, kept here as one tested function rather
# than prose inside crash_verify.sh. Attribution outranks every other check:
# a filesystem that fsck calls clean is worthless if we cannot say what
# happened to the machine that was writing to it.
verify_verdict() {
	local attr=${1:-CRASH_NO_EVIDENCE} other=${2:-PASS}
	case "$attr" in
	PLANNED_CLEAN) printf '%s\n' "$other" ;;
	# The recorder was known-dead, so "no panic record" carries no weight.
	# Refuse to call this a pass even when every other check is green.
	PLANNED_CLEAN_UNVERIFIED) printf 'FAIL(attribution-unverified)\n' ;;
	PLANNED_CONTRADICTION) printf 'FAIL(crash)\n' ;;
	CRASH_CONFIRMED) printf 'FAIL(crash)\n' ;;
	CRASH_NO_EVIDENCE) printf 'FAIL(attribution)\n' ;;
	*) printf 'FAIL(attribution)\n' ;;
	esac
}

# ------------------------------------------------------- grub rendering --
# The markers are functions, not literals sprinkled through the code, so the
# renderer (here) and the upsert/remove slicer (Task 3) cannot drift apart.
# A mismatch there means teardown silently fails to remove what setup added.
grub_block_begin() { printf '%s\n' '# BEGIN spadfs-crash-test'; }
grub_block_end()   { printf '%s\n' '# END spadfs-crash-test'; }

GRUB_ENTRY_TITLE='spadfs crash test'

# grub_entry_render LINUX_LINE INITRD_LINE MEM_ADDR MEM_SIZE EXTRA_ARGS
#
# THE `$` TRAP. `memmap=8M$0x103e000000` is destroyed twice on its way to the
# kernel: the shell expands `$0` to the running script's own name, and GRUB
# expands `$name` inside a menuentry string. The form that survives both is a
# literal backslash-dollar in the generated grub.cfg, so that is exactly what
# this function must emit.
#
# Getting this wrong is not a loud failure. The reservation silently becomes
# garbage, ramoops then writes into RAM the kernel is actively using, and what
# you observe three days later is "spadfs corrupts randomly" — not a config bug.
#
# Everything is built with printf and single-quoted format strings. No unquoted
# expansion touches the address.
grub_entry_render() {
	local linux_line=${1:-} initrd_line=${2:-}
	local mem_addr=${3:-} mem_size=${4:-} extra=${5:-}
	local dollar='$'

	grub_block_begin
	printf "menuentry '%s' --class spadfs-crash {\n" "$GRUB_ENTRY_TITLE"
	# \\%s  ->  a literal backslash, then the dollar sign
	printf '\t%s memmap=%s\\%s%s %s\n' \
		"$linux_line" "$mem_size" "$dollar" "$mem_addr" "$extra"
	printf '\t%s\n' "$initrd_line"
	# The closing brace. Without it the menuentry never terminates and
	# grub-mkconfig writes an unbalanced block into the live bootloader.
	printf '}\n'
	grub_block_end
}

# ------------------------------------------------------- block slicing --
# One implementation of "the region between the markers", shared by setup and
# teardown. Duplicating this slicing logic is exactly where a teardown bug
# would hide: setup writes between markers X/Y, teardown removes between
# markers A/B, and the entry outlives every "cleanup" you run.

# THE EXEC-BIT TRAP.
#
# /usr/bin/grub-mkconfig:289 reads a grub.d fragment only if it passes
# `test -x`:
#
#   if grub_file_is_not_garbage "$i" && test -x "$i" ; then
#
# Every writer here goes through `tmp` + `mv -f`, and a fresh redirect lands on
# the umask -- 0644 -- silently stripping the 0755 that /etc/grub.d/40_custom
# ships with. The fragment then stops reaching grub.cfg while the file itself
# still looks perfect, so the entry is gone and nothing says so. This helper is
# the single place that decision lives.
#
# A brand-new grub.d fragment must be created executable for the same reason.
_grub_chmod_like() {
	# _grub_chmod_like REF TMP  -- REF may not exist, and may already be broken.
	local ref=${1:-} tmp=${2:-} mode
	[ -n "$tmp" ] || return 2
	if [ -e "$ref" ] && [ -x "$ref" ]; then
		# Already executable: keep the site's own mode, including anything
		# stricter than 755 (0700 is legitimate here).
		mode=$(stat -c '%a' "$ref" 2>/dev/null) || mode=0755
	else
		# Missing, or already stripped of the exec bit by an earlier bug.
		# Preserving 644 would preserve the breakage, so fall back to the
		# distro default for a grub.d fragment.
		mode=0755
	fi
	chmod "$mode" "$tmp" || return 1
	return 0
}

# Replace the marked block, or append it if absent. Never blind-append.
grub_block_upsert() {
	local f=${1:-} block=${2:-} tmp
	[ -n "$f" ] || return 2
	mkdir -p "$(dirname "$f")" || return 1
	tmp="$f.tmp.$$"
	{
		if [ -f "$f" ]; then
			grub_block_keep_outside "$f"
		fi
		printf '%s\n' "$block"
	} >"$tmp" || { rm -f "$tmp"; return 1; }
	_grub_chmod_like "$f" "$tmp" || { rm -f "$tmp"; return 1; }
	mv -f "$tmp" "$f" || return 1
	return 0
}

# Print the file with the marked block removed (everything outside it).
grub_block_keep_outside() {
	local f=${1:-}
	[ -f "$f" ] || return 0
	awk -v B="$(grub_block_begin)" -v E="$(grub_block_end)" '
		$0 == B { skip = 1; next }
		$0 == E { skip = 0; next }
		!skip { print }
	' "$f"
}

grub_block_remove() {
	local f=${1:-} tmp
	[ -f "$f" ] || return 0
	tmp="$f.tmp.$$"
	grub_block_keep_outside "$f" >"$tmp" || { rm -f "$tmp"; return 1; }
	_grub_chmod_like "$f" "$tmp" || { rm -f "$tmp"; return 1; }
	mv -f "$tmp" "$f" || return 1
	return 0
}

grub_block_present() {
	local f=${1:-}
	[ -f "$f" ] || return 1
	grep -qF "$(grub_block_begin)" "$f"
}

# ------------------------------------------------- reservation proof --
# These live in the library, not in the self-check, because the load path
# (ramoops_ensure) and the self-check must agree. Two implementations of
# "is this memory actually reserved" is how one of them ends up wrong.

# cmdline_has_memmap CMDLINE_FILE SIZE ADDR
# Token-exact. A substring match here is the C2a hole: with grep -F, the
# needle "memmap=8M$0x103e00000" (one digit short) matches the real token
# "memmap=8M$0x103e000000", the gate passes, and modprobe is handed an
# address that was never reserved.
cmdline_has_memmap() {
	local f=${1:-} size=${2:-} addr=${3:-}
	local want tok line
	[ -r "$f" ] || return 1
	want="memmap=${size}\$${addr}"
	IFS= read -r line <"$f" || return 1
	set -f
	for tok in $line; do
		[ "$tok" = "$want" ] && { set +f; return 0; }
	done
	set +f
	return 1
}

# region_reserved IOMEM_FILE ADDR SIZE_BYTES
# Proves the RESULT of the reservation, not the intent. Fails if ANY entry
# whose name contains "System RAM" overlaps the whole [addr, addr+size-1]
# range, and fails if nothing covers it at all.
#
# Overlap-based rather than "first entry containing the start", because
# /proc/iomem is hierarchical: a child line like "Kernel code" under a
# System RAM parent contains no "System RAM" text, so a start-address-only
# check passes while the enclosing region is live memory.
region_reserved() {
	local f=${1:-} addr=${2:-} size=${3:-}
	local tlo thi lo hi line found=0
	[ -r "$f" ] || { echo "region_reserved: cannot read $f" >&2; return 2; }
	case "$addr" in
	0[xX][0-9a-fA-F]*) ;;
	*) echo "region_reserved: bad address '$addr'" >&2; return 2 ;;
	esac
	[[ "$size" =~ ^[0-9]+$ ]] || { echo "region_reserved: bad size '$size'" >&2; return 2; }
	((size > 0)) || { echo "region_reserved: size must be > 0" >&2; return 2; }

	tlo=$((addr))
	thi=$((addr + size - 1))

	while IFS= read -r line || [ -n "$line" ]; do
		line=${line#"${line%%[![:space:]]*}"} # left-trim indentation
		[[ "$line" =~ ^([0-9a-fA-F]+)-([0-9a-fA-F]+) ]] || continue
		lo=$((16#${BASH_REMATCH[1]}))
		hi=$((16#${BASH_REMATCH[2]}))
		((lo <= thi && hi >= tlo)) || continue
		found=1
		if [[ "$line" == *"System RAM"* ]]; then
			printf 'overlapping entry: %s\n' "$line" >&2
			return 1
		fi
	done <"$f"

	((found)) || {
		echo "region_reserved: no iomem entry covers the target range" >&2
		return 1
	}
	return 0
}

# grub_default_entry GRUB_CFG
# Pick the first menuentry block that contains BOTH a linux and an initrd
# line, and return them together with that block's title.
#
# The old code ran two independent "first match in the whole file" scans.
# Those are not the same entry: with a recovery/fallback block first you get
# the recovery kernel, and with an initrd-less first block you pair kernel A
# with initramfs B. A crash test that silently runs a different kernel than
# the one under test makes every durability number it reports worthless.
#
# Emits: title=<t> / linux=<line> / initrd=<line>
grub_default_entry() {
	local f=${1:-}
	[ -r "$f" ] || return 1
	awk -v q="'" '
		/^menuentry[ \t]/ {
			inblk = 1; title = ""; lin = ""; ini = ""
			if (match($0, q "[^" q "]*" q))
				title = substr($0, RSTART + 1, RLENGTH - 2)
			next
		}
		inblk && /^[ \t]*linux[ \t]/  { if (lin == "") { l = $0; sub(/^[ \t]+/, "", l); lin = l } }
		inblk && /^[ \t]*initrd[ \t]/ { if (ini == "") { l = $0; sub(/^[ \t]+/, "", l); ini = l } }
		inblk && /^}/ {
			if (lin != "" && ini != "") {
				printf "title=%s\nlinux=%s\ninitrd=%s\n", title, lin, ini
				exit
			}
			inblk = 0
		}
	' "$f"
}

# planned_reboot STATE_DIR CYCLE DEV TRIGGER_FILE
# The reboot trigger, with the durability precondition baked in.
#
# The marker MUST be durable before the trigger is touched. A reboot we cannot
# attribute is worth less than no reboot at all, because it can be read back
# as a clean result. So: write the marker, and if that fails, return without
# ever writing the trigger file.
#
# TRIGGER_FILE is injectable so this is testable without rebooting anything;
# in production it is /proc/sysrq-trigger.
planned_reboot() {
	local sd=${1:-} cycle=${2:-} dev=${3:-} trigger=${4:-/proc/sysrq-trigger}

	if ! planned_write "$sd" "$cycle" "$dev"; then
		echo "planned_reboot: could not write a durable PLANNED marker -- NOT rebooting" >&2
		echo "  fix $sd and re-run; a reboot we cannot attribute is not a result" >&2
		return 1
	fi

	# Enable sysrq on kernels that gate it, then pull the trigger.
	if [ -w /proc/sys/kernel/sysrq ]; then
		echo 1 >/proc/sys/kernel/sysrq 2>/dev/null || true
	fi
	printf 'b' >"$trigger" || {
		echo "planned_reboot: could not write '$trigger'" >&2
		return 1
	}
	return 0
}

# Remove one whole whitespace-delimited token from a command line.
# Deliberately token-exact rather than regex: a substring match could mangle a
# longer parameter, and silently corrupting a kernel argument is the same class
# of failure as the `$` trap.
grub_strip_token() {
	local line=${1:-} token=${2:-}
	[ -n "$token" ] || { printf '%s\n' "$line"; return 0; }
	printf '%s\n' "$line" | awk -v t="$token" '
	{
		lead = ""; rest = $0
		if (match($0, /^[ \t]+/)) { lead = substr($0, 1, RLENGTH); rest = substr($0, RLENGTH + 1) }
		n = split(rest, a, /[ \t]+/)
		out = ""
		for (i = 1; i <= n; i++) {
			if (a[i] != t && a[i] != "") out = (out == "" ? a[i] : out " " a[i])
		}
		print lead out
	}'
}
