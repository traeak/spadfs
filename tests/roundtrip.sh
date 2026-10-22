#!/usr/bin/env bash
# Task 0.2 — mkspadfs/spadfsck round-trip matrix.
#
# For each profile: create a volume, assert spadfsck -f reports it clean, AND
# assert the requested options actually landed in the superblock. Without the
# second half, an option that mkspadfs silently ignores would still pass.
#
# Expected values below were captured empirically from this build (see
# tests/sbinfo.py), not assumed from the README.
#
# Runs without root.

set -uo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
. "$TESTS_DIR/fixtures.sh"

require_tools

SIZE="${TEST_IMAGE_SIZE:-64M}"

# name | mkspadfs options | key=value assertions on sbinfo output
PROFILES=(
	"default||sectors_per_block_bits=3;flags_compat_none=0x00000007;flags_compat_ro=0x00000001;flags_compat_fsck=0x00000000;byte_sex=little"
	"block512|--block-size 512|sectors_per_block_bits=0"
	"block4096|--block-size 4096|sectors_per_block_bits=3"
	"unixnames|--unix-names|flags_compat_none=0x00000007"
	"spadnames|--spad-names|flags_compat_none=0x00000006"
	"dynmagic|--dynamic-magic|flags_compat_none=0x00000007"
	"staticmagic|--static-magic|flags_compat_none=0x00000005"
	"checksums|--checksums|flags_compat_fsck=0x00000000"
	"nochecksums|--no-checksums|flags_compat_fsck=0x00000001"
	"dirsizes|--directory-sizes|flags_compat_ro=0x00000001"
	"nodirsizes|--no-directory-sizes|flags_compat_ro=0x00000000"
	"bigendian|--big-endian|byte_sex=big"
	"littleendian|--little-endian|byte_sex=little"
	"cluster4096|--cluster-size 4096|sectors_per_cluster_bits=3"
	"cluster65536|--cluster-size 65536|sectors_per_cluster_bits=7"
	"combo-a|--unix-names --block-size 4096 --dynamic-magic --checksums|sectors_per_block_bits=3;flags_compat_none=0x00000007;flags_compat_fsck=0x00000000"
	"combo-b|--spad-names --block-size 512 --static-magic --no-checksums|sectors_per_block_bits=0;flags_compat_none=0x00000004;flags_compat_fsck=0x00000001"
	"combo-c|--big-endian --spad-names --no-directory-sizes|byte_sex=big;flags_compat_none=0x00000006;flags_compat_ro=0x00000000"
)

log "round-trip matrix (${#PROFILES[@]} profiles, image size $SIZE)"
log ""

for entry in "${PROFILES[@]}"; do
	IFS='|' read -r name opts asserts <<<"$entry"
	img="$WORKDIR/$name.img"

	if ! make_image "$img" "$SIZE" $opts; then
		bad "$name" "mkspadfs failed: $opts"
		continue
	fi

	if ! check_clean "$img"; then
		bad "$name" "spadfsck -f reported problems on a virgin volume"
		continue
	fi

	info="$(python3 "$TESTS_DIR/sbinfo.py" "$img")"
	failed_assert=""
	IFS=';' read -ra pairs <<<"$asserts"
	for kv in "${pairs[@]}"; do
		key="${kv%%=*}"
		want="${kv#*=}"
		got="$(printf '%s\n' "$info" | grep "^${key}=" | head -1 | cut -d= -f2-)"
		if [ "$got" != "$want" ]; then
			failed_assert="$key want=$want got=${got:-<absent>}"
			break
		fi
	done

	if [ -n "$failed_assert" ]; then
		bad "$name" "option did not land on disk: $failed_assert"
	else
		ok "$name"
	fi
done

summary
