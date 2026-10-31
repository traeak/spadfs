# Crash Attribution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every spadfs2 crash cycle self-identifying — the verifier can say whether the last reboot was harness-requested or a real kernel crash, and a real panic leaves a readable record.

**Architecture:** Pure attribution logic lives in one sourced bash library (`tests/lib_crash_attr.sh`) that is fully testable without root. Thin root-only drivers (`crash_boot_setup.sh`, `crash_boot_arm.sh`, `crash_boot_teardown.sh`, `crash_pstore.sh`) wrap GRUB and `pstore`, and take their external commands and paths from env-overridable variables so the drivers can be exercised against stubs. The existing `crash_cycle.sh` / `crash_verify.sh` call into the library rather than reimplementing any decision.

**Tech Stack:** bash, `shellcheck` 0.11.0 (the arbiter — there is no working LSP in this tree), `python3` for directory-`fsync`, `strace` to prove the fsync happened, GRUB (`grub-mkconfig`, `grub-reboot`), `pstore`/`ramoops`.

**Spec:** `docs/superpowers/specs/2026-10-26-crash-attribution-design.md`

## Global Constraints

- **Never auto-commit.** AGENTS.md Rule 8. Every task ends with `git add` and a report of what changed; wait for an explicit commit instruction. Do not run `git commit`.
- **`shellcheck` is the arbiter for shell**, the way kbuild is for C. Every new or modified `.sh` must pass `shellcheck -S warning` clean. Do not "fix" anything based on editor/LSP output (AGENTS.md Rule 1).
- **Unprivileged tests must not require root**: no mounting, no `modprobe`, no `/dev/sda`, no writes outside `$WORKDIR`. Root-only behaviour is tested through env-injected stubs.
- `STATE_DIR` default `/var/lib/spadfs-crash`; every library function takes it as an explicit argument and never reads the global, so tests can point it at a temp dir.
- Reservation values, verbatim from the spec: **8 MiB at `0x103e000000`**, top of System RAM `0x103f380000`.
- GRUB entry title is exactly `spadfs crash test`. Block delimiters are exactly `# BEGIN spadfs-crash-test` and `# END spadfs-crash-test`.
- Indent with tabs, matching `tests/crash_cycle.sh`.
- No spadfs2 kernel code, no on-disk format change. This plan touches `tests/` only.

## Review Focus

Failure modes the spec implies that no single happy-path test would catch. Each line is pinned to a test in the owning task below.

1. **A `PLANNED` marker that is not durable is worse than none** — it converts a real crash into a false "planned". Expect: `fsync` of the file *and* the parent directory, and a test that observes both syscalls.
2. **A stale `PLANNED` from cycle N vouching for cycle N+1** — Expect: the marker carries its cycle number and must match the cycle under verification, else the verdict is `unplanned`.
3. **`$` in `memmap=8M$0x103e000000` eaten by the shell or by GRUB** — the reservation silently becomes garbage and ramoops writes into live RAM, producing random corruption that looks like a spadfs bug. Expect: the renderer emits a literal `\$`, and a post-boot check asserts `/proc/cmdline` contains a literal `$`.
4. **`UNPLANNED` + no pstore record read as "clean"** — the box hung or lost power and we captured nothing. Expect: `CRASH_NO_EVIDENCE`, a FAIL, never a pass.
5. **`ramoops` loaded but the backend never attached** (bad address, reservation missing) — harvest finds nothing and everyone concludes "no crash". Expect: assert `pstore: attached` before treating an empty `/sys/fs/pstore` as meaningful.
6. **Applying boot setup twice duplicates the entry** — `grub-reboot` by title then picks the first match, possibly with stale arguments. Expect: replace-between-markers, never blind append.
7. **Teardown leaves the test entry behind, or `grub-mkconfig` drops it silently** — Expect: marker-free result asserted, plus a byte-comparison proving the default entry is unchanged.
8. **`nowatchdog` carried over from the default cmdline silently defeats the panic-on-lockup args** — the test entry is built by copying the default `linux` line, and that line contains `nowatchdog`, which disables the detectors *globally*. Appending `softlockup_panic=1` does not re-enable a watchdog that `nowatchdog` already turned off, so the entry would look correct and catch nothing. Expect: `nowatchdog` stripped from the copied args, asserted by test.

---

### Task 1: Attribution library — marker, boot env, verdict table

**Files:**
- Create: `tests/lib_crash_attr.sh`
- Create: `tests/crash_attr_test.sh`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `planned_write STATE_DIR CYCLE DEV` → writes `$STATE_DIR/PLANNED` with `cycle=`, `dev=`, `ts=` keys; fsyncs file and directory; returns 0
  - `planned_read STATE_DIR` → prints marker contents, exit 0 if present, exit 1 if absent
  - `planned_cycle STATE_DIR` → prints the marker's `cycle=` value, empty if absent
  - `planned_consume STATE_DIR` → removes the marker, fsyncs the directory, returns 0
  - `boot_env_set STATE_DIR KEY VALUE` / `boot_env_get STATE_DIR KEY` → last-wins `KEY=VALUE` in `$STATE_DIR/boot_env`
  - `reboot_verdict PLANNED PSTORE` → `PLANNED_CLEAN` | `PLANNED_CONTRADICTION` | `CRASH_CONFIRMED` | `CRASH_NO_EVIDENCE`

- [ ] **Step 1: Write the failing test** — `tests/crash_attr_test.sh`, sourced from `lib.sh` so it reuses `ok`/`bad`/`summary`. Cover:

```bash
# marker round-trip
planned_write "$SD" 7 /dev/sda1
[ "$(planned_cycle "$SD")" = "7" ] || bad "planned_cycle round-trip"

# Review Focus 2: a marker for cycle 7 must NOT vouch for cycle 8
[ "$(reboot_verdict "$(planned_matches "$SD" 8)" none)" = "CRASH_NO_EVIDENCE" ] \
    || bad "stale marker rejected"

# Review Focus 1: both fsyncs actually happen
strace -f -e trace=fsync,syncfs -o "$TRACE" bash -c "planned_write '$SD' 1 /dev/sda1"
grep -q 'fsync(' "$TRACE" || bad "no fsync observed"

# consume is idempotent and leaves nothing behind
planned_consume "$SD"; planned_consume "$SD"
[ ! -e "$SD/PLANNED" ] || bad "planned_consume left the marker"
```

- [ ] **Step 2: Run it, confirm it fails** — `bash tests/crash_attr_test.sh`. Expected: fails at source time / "command not found" for `planned_write`.

- [ ] **Step 3: Implement the four marker functions plus `planned_matches` and `boot_env_*`** in `tests/lib_crash_attr.sh`. `planned_matches STATE_DIR CYCLE` prints `planned` when the marker exists and its `cycle=` equals `$CYCLE`, else `unplanned` — this is the function that makes Review Focus 2 testable without the verdict knowing about files. Directory fsync uses `python3 -c 'import os,sys; fd=os.open(sys.argv[1], os.O_RDONLY); os.fsync(fd); os.close(fd)'` (bash cannot open a directory fd itself).

- [ ] **Step 4: Implement `reboot_verdict`** as the spec's four-row table over `(planned|unplanned) x (none|panic|oops)`. Note `panic` and `oops` collapse to "record present".

- [ ] **Step 5: Run the test, confirm pass** — `bash tests/crash_attr_test.sh`. Expected: all PASS, exit 0.

- [ ] **Step 6: Lint** — `shellcheck -S warning tests/lib_crash_attr.sh tests/crash_attr_test.sh`. Expected: clean.

- [ ] **Step 7: Stage** — `git add tests/lib_crash_attr.sh tests/crash_attr_test.sh`. Report to the user. **Do not commit.**

---

### Task 2: GRUB entry renderer and the `$` escaping trap

**Files:**
- Modify: `tests/lib_crash_attr.sh` (add `grub_entry_render`)
- Modify: `tests/crash_attr_test.sh`

**Interfaces:**
- Consumes: `tests/lib_crash_attr.sh` from Task 1.
- Produces: `grub_entry_render LINUX_LINE INITRD_LINE MEM_ADDR MEM_SIZE EXTRA_ARGS` → prints the complete delimited block for `/etc/grub.d/40_custom`, with `memmap=<MEM_SIZE>\$<MEM_ADDR>` as a literal backslash-dollar.

- [ ] **Step 1: Write the failing tests** — Review Focus 3 lives here, and it is the one trap that silently corrupts RAM:

```bash
OUT=$(grub_entry_render "linux /boot/vmlinuz-x root=UUID=y" "initrd /boot/initramfs-x.img" \
                      0x103e000000 8M "panic=10")

# the literal escape must survive
case "$OUT" in *'memmap=8M\$0x103e000000'*) ok "literal backslash-dollar";;
              *) bad "memmap not escaped" "$OUT";; esac

# and $0 must NOT have been expanded to a script path
case "$OUT" in *' crash_boot_setup.sh'*) bad "shell ate \$0";; esac
case "$OUT" in *'grub_entry_render'*)   bad "renderer leaked its own name";; esac

# block boundaries exact (Task 3's replace/teardown depend on these strings)
grep -q '^# BEGIN spadfs-crash-test$' <<<"$OUT" || bad "missing BEGIN"
grep -q '^# END spadfs-crash-test$'   <<<"$OUT" || bad "missing END"
grep -q "menuentry 'spadfs crash test'" <<<"$OUT" || bad "missing title"
```

- [ ] **Step 2: Run, confirm fail.** Expected: `grub_entry_render: command not found`.

- [ ] **Step 3: Implement `grub_entry_render`.** Approach: build the block with a quoted heredoc (`<<'EOF'`) inside a function that substitutes via `printf`, never via unquoted expansion — the quoted heredoc is what stops the shell from eating `$0`. Emit `linux <args> memmap=<size>\$<addr> <extra>` then `initrd <path>`.

- [ ] **Step 4: Run, confirm pass.** Also verify the renderer is pure: two calls with the same args produce identical output.

- [ ] **Step 5: Lint** — `shellcheck -S warning tests/lib_crash_attr.sh tests/crash_attr_test.sh`. Expected: clean.

- [ ] **Step 6: Stage.** **Do not commit.**

---

### Task 3: Boot setup / arm / teardown drivers

**Files:**
- Create: `tests/crash_boot_setup.sh`
- Create: `tests/crash_boot_arm.sh`
- Create: `tests/crash_boot_teardown.sh`
- Create: `tests/crash_boot_test.sh`

**Interfaces:**
- Consumes: `grub_entry_render` (Task 2), `boot_env_set`/`boot_env_get` (Task 1).
- Produces:
  - `crash_boot_setup.sh [--apply]` — default is dry-run. Writes the block into `$GRUB_CUSTOM`, regenerates `$GRUB_CFG`, records `MEM_ADDR`/`MEM_SIZE`/`ENTRY_TITLE` in `$STATE_DIR/boot_env`.
  - `crash_boot_arm.sh` — one-shot select via `$GRUB_REBOOT "$ENTRY_TITLE"`.
  - `crash_boot_teardown.sh` — removes the delimited block, regenerates, asserts markers gone.
  - Env overrides, all defaulting to the real paths: `GRUB_CUSTOM=/etc/grub.d/40_custom`, `GRUB_CFG=/boot/grub/grub.cfg`, `GRUB_MKCONFIG=grub-mkconfig`, `GRUB_REBOOT=grub-reboot`. **These exist so the drivers are testable without root.**

- [ ] **Step 1: Write the failing tests** — against a temp dir with stub `grub-mkconfig`/`grub-reboot` that record their invocation. Covers Review Focus 6 and 7:

```bash
# dry-run changes nothing
GRUB_CUSTOM=$T/40_custom GRUB_CFG=$T/grub.cfg GRUB_MKCONFIG=$T/stub-mkconfig \
    crash_boot_setup.sh
[ ! -e "$T/grub.cfg" ] || bad "dry-run wrote grub.cfg"

# --apply inserts between markers and records boot_env
crash_boot_setup.sh --apply
grep -q 'BEGIN spadfs-crash-test' "$T/40_custom" || bad "block not inserted"
[ "$(boot_env_get "$T/state" MEM_ADDR)" = "0x103e000000" ] || bad "boot_env MEM_ADDR"

# Review Focus 6: apply twice must NOT duplicate
crash_boot_setup.sh --apply
[ "$(grep -c 'BEGIN spadfs-crash-test' "$T/40_custom")" -eq 1 ] || bad "duplicated block"

# Review Focus 7: teardown removes exactly the block, default entry byte-identical
crash_boot_teardown.sh
grep -q 'spadfs-crash-test' "$T/40_custom" && bad "teardown left markers"
cmp -s "$T/default_entry.before" "$T/default_entry.after" || bad "default entry changed"

# Review Focus 8: nowatchdog must NOT survive into the rendered entry
case "$(cat "$T/40_custom")" in *nowatchdog*) bad "nowatchdog carried over";; esac
grep -q 'softlockup_panic=1' "$T/40_custom" || bad "missing softlockup_panic"
grep -q 'panic=10' "$T/40_custom" || bad "missing panic timeout"

# arm uses the title from boot_env, not a hardcoded string
crash_boot_arm.sh
grep -q "spadfs crash test" "$T/reboot.calls" || bad "arm did not pass title"
```

- [ ] **Step 2: Run, confirm fail.** Expected: drivers not found.

- [ ] **Step 3: Implement the three drivers.** `crash_boot_setup.sh` passes `EXTRA_ARGS` exactly `panic=10 softlockup_panic=1 hardlockup_panic=1`, and **must strip `nowatchdog` from the copied default `linux` line before rendering.** This host's default cmdline is `loglevel=4 mitigations=off nowatchdog amdgpu.ppfeaturemask=0xffffffff amd_pstate=active`; a naive copy inherits `nowatchdog`, which disables the lockup detectors globally and makes `softlockup_panic=1` inert (Review Focus 8). Strip the whole token with a word-boundary filter, not a substring `sed` that could mangle a longer parameter.

Extract the insert/replace/remove logic into `grub_block_upsert CUSTOM_FILE BLOCK_TEXT` and `grub_block_remove CUSTOM_FILE` in `lib_crash_attr.sh` so both setup and teardown share one implementation of "between the markers" — duplicated slicing logic is where a teardown bug would hide. Read the real `linux`/`initrd` lines from `$GRUB_CFG` with `awk` over the first `menuentry` that has a `linux` line.

- [ ] **Step 4: Run, confirm pass.**

- [ ] **Step 5: Lint** all new files. Expected: clean.

- [ ] **Step 6: Stage.** **Do not commit.**

---

### Task 4: pstore harvest and ramoops bring-up

**Files:**
- Create: `tests/crash_pstore.sh`
- Create: `tests/crash_pstore_test.sh`

**Interfaces:**
- Consumes: `reboot_verdict` (Task 1), `boot_env_get` (Task 1).
- Produces:
  - `pstore_classify TEXT` → `panic` | `oops` | `none`
  - `pstore_harvest PSTORE_DIR STATE_DIR CYCLE` → copies `dmesg-ramoops-*` to `$STATE_DIR/panic.$CYCLE.<n>.txt`, prints the classification, exit 0 always (absence is a result, not an error)
  - `ramoops_ensure MEM_ADDR MEM_SIZE` → `$MODPROBE ramoops mem_address=… mem_size=… record_size=1M ecc=16`, then asserts attachment
  - Env overrides: `PSTORE_DIR=/sys/fs/pstore`, `MODPROBE=modprobe`, `DMESG=dmesg`

- [ ] **Step 1: Write the failing tests** — fixtures are text files, no kernel needed. Covers Review Focus 5:

```bash
pstore_classify "$(cat fixtures/panic.txt)"  → panic     # 'Kernel panic - not syncing:'
pstore_classify "$(cat fixtures/oops.txt)"   → oops      # 'BUG: unable to handle kernel'
pstore_classify "nothing interesting"        → none

# harvest with an empty pstore dir reports none and creates no panic files
pstore_harvest "$T/empty_pstore" "$T/state" 3
[ "$(pstore_harvest ... )" = "none" ] && [ ! -e "$T/state/panic.3.0.txt" ]

# harvest preserves the record for later reading
pstore_harvest "$T/pstore_with_record" "$T/state" 3   → panic
grep -q 'Trigger a crash' "$T/state/panic.3.0.txt"

# Review Focus 5: ramoops_ensure FAILS when 'attached' is absent,
# so an empty pstore is never mistaken for "no crash"
STUB_DMESG_NO_ATTACH=1 ramoops_ensure 0x103e000000 8M; [ $? -ne 0 ] || bad "ensure passed without attach"
```

- [ ] **Step 2: Run, confirm fail.**

- [ ] **Step 3: Implement.** `pstore_classify` greps `Kernel panic - not syncing` first, then `BUG:`/`Oops:` — order matters, a panic log contains both. `ramoops_ensure` must treat "module already loaded" as success and "loaded but not attached" as hard failure, and print the reason.

- [ ] **Step 4: Run, confirm pass.**

- [ ] **Step 5: Lint.** Expected: clean.

- [ ] **Step 6: Stage.** **Do not commit.**

---

### Task 5: Wire attribution into the existing cycle and verifier

**Files:**
- Modify: `tests/crash_cycle.sh`
- Modify: `tests/crash_verify.sh`

**Interfaces:**
- Consumes: `planned_write`, `planned_matches`, `planned_consume`, `reboot_verdict` (Task 1); `pstore_harvest`, `ramoops_ensure` (Task 4); `boot_env_get` (Task 1).
- Produces: `crash_verify.sh` gains an attribution section that runs **before** fsck/mount, and its final `VERDICT` can no longer be `PASS` unless attribution is `PLANNED_CLEAN`.

- [ ] **Step 1: Write the failing test** — extend `tests/crash_attr_test.sh` with the integration decision, driven through the library rather than the real scripts (the real scripts need root and a disk):

```bash
# the verify verdict function: attribution gates everything else
verify_verdict PLANNED_CLEAN "$other_pass"   → PASS
verify_verdict CRASH_NO_EVIDENCE "$other_pass" → FAIL(attribution)
verify_verdict CRASH_CONFIRMED "$other_pass"   → FAIL(crash)
verify_verdict PLANNED_CONTRADICTION "$other_pass" → FAIL(crash)
```

- [ ] **Step 2: Run, confirm fail.**

- [ ] **Step 3: Add `verify_verdict ATTR OTHER` to `lib_crash_attr.sh`** so the gating rule is one function with a test, not prose inside `crash_verify.sh`.

- [ ] **Step 4: Modify `crash_cycle.sh`** — source `lib_crash_attr.sh`; immediately before `echo b >/proc/sysrq-trigger` (and **not** on the `--reboot`-absent early-exit path) call `planned_write "$CYCLE_DIR" "$CYCLE" "$DEV"`. If `planned_write` fails, abort before rebooting: rebooting without a durable marker destroys the ability to attribute the reboot at all.

- [ ] **Step 5: Modify `crash_verify.sh`** — insert an attribution section as the first thing after the header, before `--- fsck ---`:

```bash
ATTR=$(planned_matches "$STATE_DIR" "$CYCLE")
PSTORE=$(pstore_harvest "$PSTORE_DIR" "$STATE_DIR" "$CYCLE")
VERDICT_ATTR=$(reboot_verdict "$ATTR" "$PSTORE")
```

Print the verdict plus the harvested oops head (first 30 lines of `panic.$CYCLE.*.txt`) when present. Then `planned_consume "$STATE_DIR"`. Feed `VERDICT_ATTR` into `verify_verdict` at the existing verdict block.

- [ ] **Step 6: Run the unprivileged suite, confirm pass** — `bash tests/crash_attr_test.sh && bash tests/crash_pstore_test.sh && bash tests/crash_boot_test.sh`.

- [ ] **Step 7: Lint the modified scripts** — `shellcheck -S warning tests/crash_cycle.sh tests/crash_verify.sh`. Expected: no *new* findings versus the pre-change baseline; record the baseline count first.

- [ ] **Step 8: Stage.** **Do not commit.**

---

### Task 6: On-host self-check and positive control

**Files:**
- Create: `tests/crash_attr_selfcheck.sh`

**Interfaces:**
- Consumes: `boot_env_get`, `ramoops_ensure`, `pstore_harvest`.
- Produces: a root-run script that proves the capture path works before any cycle verdict is trusted. Exit 0 = capture path verified.

This task cannot be unit-tested — the script *is* the test. Its value is that it makes the spec's Verification plan runnable in one command instead of by hand.

- [ ] **Step 1: Implement the four checks**, each printing `OK`/`FAIL` and the observed value:
  1. `/proc/cmdline` contains the literal string `memmap=8M$0x103e000000` — **with `$`, not `\$` and not an expanded path**. This is the Review Focus 3 check on real hardware.
  2. `panic=` present in `/proc/cmdline` (else a panic hangs the box forever — `CONFIG_PANIC_TIMEOUT=0`).
  3. `pstore: attached` present in `dmesg`, and `/sys/fs/pstore` is a `pstore` filesystem (`findmnt -t pstore`).
  4. The reserved region is not claimed by anything else: `grep -i 103e000000 /proc/iomem` shows it as reserved/ramoops, not `System RAM`.

- [ ] **Step 2: Add the positive control behind a deliberate opt-in** — `--i-am-throwaway` triggers `echo c >/proc/sysrq-trigger`. Guard it hard: refuse unless the flag is given *and* the script confirms it is running on the armed test boot (check 1 passed). Without that double gate, a mistaken run panics the daily-driver boot. Document that after this control, the next boot must show `panic.$CYCLE.0.txt` containing `sysrq: Trigger a crash`.

- [ ] **Step 3: Run the self-check on the armed test boot** — `sudo tests/crash_attr_selfcheck.sh`. Expected: 4 OK. Any FAIL means stop; do not run cycles.

- [ ] **Step 4: Run the positive control** — `sudo tests/crash_attr_selfcheck.sh --i-am-throwaway`, reboot, then `sudo tests/crash_attr_selfcheck.sh` again plus `pstore_harvest`. Expected: record present, `pstore_classify` = `panic`.

- [ ] **Step 5: Lint** — `shellcheck -S warning tests/crash_attr_selfcheck.sh`. Expected: clean.

- [ ] **Step 6: Stage.** **Do not commit.**

---

## Out of Order: what the executor must not do

- Do not run `git commit` (AGENTS.md Rule 8).
- Do not `modprobe ramoops` with an address that was not reserved by `memmap=` on the current cmdline — that writes into live RAM. `crash_attr_selfcheck.sh` check 1 exists to prevent exactly this.
- Do not treat an empty `/sys/fs/pstore` as evidence of no crash without the `pstore: attached` assertion (Review Focus 5).
- Do not enable `CONFIG_PSTORE_CONSOLE`-dependent features; that config is unset and rebuilding the kernel is out of scope.
