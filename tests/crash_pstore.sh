# shellcheck shell=bash
# crash_pstore.sh — bring up ramoops and harvest panic records from pstore.
#
# Sourced by crash_verify.sh and by the tests. Every external command is
# env-injectable (MODPROBE, DMESG, FINDMNT, SYS_MODULE_DIR) so the whole
# interpretation layer is testable without a kernel to poke at.

# The reservation proofs live in lib_crash_attr.sh so the load path and the
# self-check cannot drift apart. Source them if the caller has not.
# shellcheck disable=SC1091
if ! declare -f region_reserved >/dev/null 2>&1; then
	. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_crash_attr.sh"
fi
#
# WHY THE INTERPRETATION MATTERS MORE THAN THE MECHANICS
#
# An empty /sys/fs/pstore means one of two completely different things:
#   - nothing crashed, or
#   - the capture path never worked
# Reading the first into the second is how a harness reports a clean run over a
# machine that was never recording anything. ramoops_ensure exists to make
# those two cases distinguishable, and it must fail loudly on the second.

# Human size -> bytes. The ramoops mem_size/record_size params are `ulong`
# and go through kstrtoul, which does NOT accept a "8M" suffix — even though
# the memmap= kernel parameter does. Passing "8M" to modprobe fails to parse.
# So: suffixes are accepted here and converted to a plain byte count.
size_to_bytes() {
	local s=${1:-}
	case "$s" in
	'' | *[!0-9KMGkmg]*) printf '0\n'; return 1 ;;
	*[Kk]) printf '%d\n' "$(( ${s%[Kk]} * 1024 ))" ;;
	*[Mm]) printf '%d\n' "$(( ${s%[Mm]} * 1024 * 1024 ))" ;;
	*[Gg] | *[Bb]) printf '%d\n' "$(( ${s%[GBgb]} * 1024 * 1024 * 1024 ))" ;;
	*) printf '%d\n' "$s" ;;
	esac
}

# Classify a chunk of kmsg text. Order matters: a panic log almost always
# contains oops markers too, and panic is the stronger claim.
pstore_classify() {
	local text=${1:-}
	if printf '%s\n' "$text" | grep -q 'Kernel panic - not syncing'; then
		printf 'panic\n'
	elif printf '%s\n' "$text" | grep -qE 'BUG: unable to handle|Oops:|kernel BUG at'; then
		printf 'oops\n'
	else
		printf 'none\n'
	fi
}

# Copy every dmesg-ramoops-* record into the state dir under a cycle-stamped
# name and print the classification of everything found.
#
# Absence of records is a RESULT, not an error: the caller needs to hear
# "none" and decide. Never fail here.
pstore_harvest() {
	local ps_dir=${1:-} sd=${2:-} cycle=${3:-}
	local combined="" n=0 f

	[ -n "$ps_dir" ] && [ -n "$sd" ] && [ -n "$cycle" ] || { printf 'none\n'; return 0; }
	mkdir -p "$sd" 2>/dev/null || { printf 'none\n'; return 0; }

	if [ -d "$ps_dir" ]; then
		for f in "$ps_dir"/dmesg-ramoops-*; do
			[ -e "$f" ] || continue
			if cp -f "$f" "$sd/panic.$cycle.$n.txt" 2>/dev/null; then
				combined="$combined$(cat "$f" 2>/dev/null)"
				# Make our copy durable BEFORE erasing the only other copy.
				fsync_file "$sd/panic.$cycle.$n.txt" 2>/dev/null || true
				# pstore releases a RAM slot only on unlink. Leaving the record
				# behind means the next mount re-presents the same crash, and it
				# gets filed under a NEW cycle number — manufactured evidence.
				rm -f "$f" 2>/dev/null || \
					echo "pstore_harvest: could not erase $f (needs root)" >&2
				n=$((n + 1))
			else
				echo "pstore_harvest: could not copy $f" >&2
			fi
		done
		fsync_dir "$sd" 2>/dev/null || true
	fi

	pstore_classify "$combined"
	return 0
}

# Load ramoops over the reserved region and PROVE the capture path is live.
#
# The gate is deterministic kernel state, not a log grep:
#   1. the ramoops module is actually loaded
#   2. a pstore filesystem is mounted
# The dmesg attach line is printed as corroboration but never required — the
# exact wording varies by kernel version and a false negative would block the
# whole harness while telling us nothing true.
#
# Returns 1 if the capture path is not live. Callers must treat that as
# "do not trust any absence of evidence downstream".
ramoops_ensure() {
	local mem_addr=${1:-} mem_size=${2:-}
	local modprobe=${MODPROBE:-modprobe}
	local dmesg=${DMESG:-dmesg}
	local findmnt=${FINDMNT:-findmnt}
	local sysmod=${SYS_MODULE_DIR:-/sys/module}
	local record_size=${RECORD_SIZE:-1M}
	local ecc=${ECC:-16}
	local cmdline_file=${CMDLINE:-/proc/cmdline}
	local iomem_file=${IOMEM:-/proc/iomem}
	local mrc=0 bytes rec_bytes

	[ -n "$mem_addr" ] && [ -n "$mem_size" ] || {
		echo "ramoops_ensure: need MEM_ADDR and MEM_SIZE" >&2
		return 2
	}

	bytes=$(size_to_bytes "$mem_size") || {
		echo "ramoops_ensure: unparseable mem_size '$mem_size'" >&2
		return 2
	}
	rec_bytes=$(size_to_bytes "$record_size") || {
		echo "ramoops_ensure: unparseable record_size '$record_size'" >&2
		return 2
	}

	# SAFETY GATE — runs before any modprobe, and before anything else.
	# ramoops writes to whatever address it is handed. If the region was not
	# reserved, that address belongs to the kernel and to running processes,
	# and loading the module corrupts live memory. There is no undo.
	#
	# Two separate proofs, because they catch different lies:
	#   1. cmdline_has_memmap — token-exact, so a truncated MEM_ADDR cannot
	#      slip through on a prefix match (C2a).
	#   2. region_reserved — proves the reservation was actually TAKEN, not
	#      merely requested. The kernel can reject or ignore memmap= while
	#      /proc/cmdline still echoes it (C2b).
	# Both must pass. If either cannot be read, we fail closed.
	if ! cmdline_has_memmap "$cmdline_file" "$mem_size" "$mem_addr"; then
		echo "ramoops_ensure: REFUSING — no token 'memmap=${mem_size}\\\$${mem_addr}' on this boot's command line" >&2
		echo "  (matched token-by-token; a prefix of a longer token does not count)" >&2
		echo "  Run crash_boot_setup.sh --apply, then crash_boot_arm.sh, and reboot." >&2
		return 3
	fi

	if ! region_reserved "$iomem_file" "$mem_addr" "$bytes"; then
		echo "ramoops_ensure: REFUSING — the region is not actually reserved" >&2
		echo "  memmap= is on the command line but /proc/iomem does not back that up." >&2
		echo "  Loading ramoops here would write into live RAM." >&2
		return 3
	fi

	# A non-zero rc here is not fatal on its own: modprobe errors when the
	# module is already loaded. The directory check below is what decides.
	"$modprobe" ramoops "mem_address=$mem_addr" "mem_size=$bytes" \
		"record_size=$rec_bytes" "ecc=$ecc" >/dev/null 2>&1 || mrc=$?

	if [ ! -d "$sysmod/ramoops" ]; then
		echo "ramoops_ensure: FAILED — ramoops is NOT loaded (modprobe rc=$mrc)." >&2
		echo "  Is the region reserved? memmap=$mem_size\$<addr> must be on THIS boot's" >&2
		echo "  command line, or ramoops would be writing into live RAM." >&2
		return 1
	fi

	if ! "$findmnt" -t pstore >/dev/null 2>&1; then
		echo "ramoops_ensure: FAILED — module loaded but no pstore mount." >&2
		echo "  mount -t pstore pstore /sys/fs/pstore" >&2
		return 1
	fi

	# corroboration only
	local att
	att=$("$dmesg" 2>/dev/null | grep -iE 'pstore|ramoops' | tail -3)
	[ -n "$att" ] && printf 'corroborating dmesg:\n%s\n' "$att"

	echo "ramoops_ensure: OK — capture path live ($mem_addr, $bytes bytes, record $rec_bytes, ecc $ecc)"
	return 0
}
