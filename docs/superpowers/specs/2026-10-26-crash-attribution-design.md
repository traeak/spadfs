# Crash Attribution for the spadfs2 Crash Harness

Date: 2026-10-26
Host: Void Linux, kernel `7.2.12_1`, UEFI + GRUB
Status: awaiting approval

## Intent

The crash harness cannot tell, after the fact, whether the last reboot was **requested**
by `crash_cycle.sh --reboot` or was an **actual kernel panic**. Both look identical:
the script stops logging. That makes every crash cycle unfalsifiable, and it already
cost a debugging round-trip when a designed reboot was read as a possible crash.

Two changes, together:

- **(a)** a durable *planned-reboot* marker, asserted by the verifier;
- **(b)** `pstore`/`ramoops`, so a real panic leaves a record that survives the reboot.

Neither touches spadfs2 itself. This is harness + host configuration.

## Goals

- **G1** — `crash_verify.sh` states definitively whether the last reboot was planned
  or unplanned, before it reports anything else.
- **G2** — a real panic or oops leaves `dmesg-ramoops-*` readable after reboot, with
  the oops text and call trace intact.
- **G3** — the machine's **default boot is untouched**. Only the crash-test boot
  carries the extra kernel arguments.
- **G4** — fully reversible with one command.
- **G5** — a deliberate, throwaway panic proves the capture path works *before* we
  trust a cycle's verdict.

## Non-goals

- Continuous console capture. `CONFIG_PSTORE_CONSOLE` is **not set**; adding it means
  rebuilding the kernel. Out of scope.
- Full `vmcore` / `kdump`. Debugger-grade, but its own project.
- Any change to spadfs2 on-disk format, checksums, or kernel code.

## Constraints verified on this host

| Fact | Consequence |
|---|---|
| `CONFIG_PSTORE=y`, `CONFIG_PSTORE_RAM=m`, `ramoops.ko.zst` present | ramoops loadable now, no rebuild |
| `reed_solomon.ko.zst` present | `ecc=16` usable on the ramoops region |
| `# CONFIG_PSTORE_CONSOLE is not set` | no `console-ramoops-*`; only `dmesg-ramoops-*` on panic/oops |
| `# CONFIG_PSTORE_PMSG is not set` | harness **cannot** write its own notes into ramoops via pmsg |
| Top of System RAM = `0x103f380000` (63.94 GiB total, `/sys/firmware/memmap`) | 8 MiB at `0x103e000000` sits inside the largest contiguous range, 16 MiB aligned |
| `CONFIG_PANIC_TIMEOUT=0` | a real panic **hangs forever** — the box never comes back unattended |
| `nowatchdog` on the current default cmdline | soft+hard lockup detectors off → a deadlock leaves no panic, no record |
| `/boot/grub/grub.cfg` is `0600 root` | setup script must run as root and read the `linux`/`initrd` lines from it |
| `grub-mkconfig`, `grub-reboot`, `grub-editenv` present; `grubenv` is a real 1024 B file | one-shot boot selection available |
| GRUB honours `next_entry` unconditionally | one-shot works **without** changing `GRUB_DEFAULT=saved` |

### The `$` trap

`memmap=8M$0x103e000000` is mangled twice before it reaches the kernel:

- **shell**: `$0x103e...` → `$0` (the script's own name) followed by `x103e...`
- **GRUB**: `$name` is variable expansion inside a `menuentry` string

Correct form: write the entry from a **quoted heredoc** (`<<'EOF'`) so the shell does
not expand, and emit `memmap=8M\$0x103e000000` so GRUB passes a literal `$`.
`/proc/cmdline` after boot is the check that settles it — see G5.

## Approach

**Chosen — B1: one-shot GRUB test entry + ramoops.**
A dedicated `menuentry` in `/etc/grub.d/40_custom` carries the memory reservation,
panic timeout and watchdog policy. `grub-reboot` selects it for exactly the next boot.
The default entry is never modified.

Rejected:

- **B2 `pstore_blk` on a raw partition** — the recorder would live on `/dev/sda`, the
  same disk under test. A crash harness must not store its evidence inside the subject.
  Also needs a repartition.
- **B3 kdump/kexec** — real vmcore, but needs a capture kernel and RAM-sized scratch.
  Separate project.

## Design

### 1. `tests/crash_boot_setup.sh` — create the test entry (root, idempotent)

- Read the default entry's `linux` and `initrd` lines out of `/boot/grub/grub.cfg`.
- Append to `/etc/grub.d/40_custom`, wrapped in marker comments
  (`# BEGIN spadfs-crash-test` / `# END spadfs-crash-test`), a `menuentry
  'spadfs crash test'` with those same paths plus:
  - `memmap=8M\$0x103e000000`
  - `panic=10`
  - watchdog args (see §5)
- `grub-mkconfig -o /boot/grub/grub.cfg`.
- Write the reservation address/size to `$STATE_DIR/boot_env` so the other scripts
  read one source of truth instead of each hardcoding it.
- **`--apply` gates the write.** Without it, print the exact diff and the generated
  entry, change nothing.

### 2. `tests/crash_boot_arm.sh` — select it once

- `grub-reboot "spadfs crash test"` → sets `next_entry`, consumed by that one boot.
- Prints `grub-editenv list` before and after so the arming is visible.

### 3. Planned-reboot marker (a)

In `crash_cycle.sh`, immediately **before** `echo b >/proc/sysrq-trigger`:

- write `$STATE_DIR/PLANNED` containing `cycle`, `DEV`, ISO timestamp;
- `fsync` the file **and** the containing directory — a marker that is not durable is
  worse than none, because it converts a crash into a false "planned".

In `crash_verify.sh`, **first thing**, before any fsck or mount:

- `PLANNED` present and matching this cycle → `reboot: PLANNED (harness-requested)`.
- absent or mismatched → `reboot: UNPLANNED — treat as a crash`, and the run is
  reported as FAIL until the pstore record says otherwise.
- The marker is **consumed** (removed) after reading, so a stale one cannot vouch for
  a later cycle.

### 4. ramoops load and read (b)

- On the test boot, `modprobe ramoops mem_address=0x103e000000 mem_size=8M
  record_size=1M ecc=16`, then assert the backend attached
  (`dmesg | grep 'pstore: attached'`).
- `crash_verify.sh` loads the module if absent, then copies every
  `/sys/fs/pstore/dmesg-ramoops-*` to `$STATE_DIR/panic.$CYCLE.txt` and prints the
  `Kernel panic` / `Oops` line plus the head of the call trace.
- **Cross-check, both directions:**
  - `UNPLANNED` **and** a ramoops record → confirmed crash; report the oops.
  - `UNPLANNED` **and no** record → say so plainly: nothing was captured; the box
    hung, lost power, or died harder than pstore can record. Do **not** call this clean.
  - `PLANNED` **and** a ramoops record → the marker lied or a panic happened anyway;
    surface the contradiction rather than trusting either alone.

### 5. Watchdog policy — my call, correct me

You did not say whether hangs are in scope, so I put them **in**. The test entry drops
`nowatchdog` and adds `softlockup_panic=1` and `hardlockup_panic=1`.

Reason: a filesystem deadlock is a *likely* failure mode for a filesystem under crash
test, and with the detectors off it produces no panic, no record, and a frozen machine.
Cost: a test boot may panic on a stall your latency-tuned daily driver tolerates — but
that affects only the one-shot test entry, never a normal boot. Removing it is one line
in `crash_boot_setup.sh`.

### 6. `tests/crash_boot_teardown.sh` — rollback (G4)

Delete the marker-delimited block from `/etc/grub.d/40_custom`, re-run
`grub-mkconfig`, clear `$STATE_DIR/boot_env`. Assert the default entry is
byte-identical to before setup.

## Verification plan (G5) — before trusting any verdict

1. `crash_boot_setup.sh` dry-run: inspect the generated entry and the `grub.cfg` diff.
2. `--apply`, then `crash_boot_arm.sh`.
3. Reboot. **Check `/proc/cmdline`** — must show `memmap=8M$0x103e000000` and
   `panic=10` with a literal `$`. This is what catches the escaping trap.
4. `modprobe ramoops ...`; confirm `pstore: attached` in `dmesg`.
5. **Deliberate positive control:** on a throwaway boot, `echo c > /proc/sysrq-trigger`.
   Reboot and confirm `dmesg-ramoops-0` exists and contains
   `sysrq: Trigger a crash` plus a trace. If this does not work, nothing downstream is
   trustworthy — stop here rather than run cycles.
6. Only then run a real `crash_cycle.sh --reboot` + `crash_verify.sh`.

## Open, left to the builder

- Exact `record_size` / `ecc` sizing for the 8 MiB region.
- Whether `ramoops` loads from the initramfs (earlier coverage, catches panics before
  `modprobe`) or on demand. On demand is simpler and our panics occur during `fsync`,
  long after boot — so on demand is the default here.
- Whether arming is folded into `crash_cycle.sh --reboot` or stays a separate step.
  Kept separate so an accidental `--reboot` cannot silently run on an unprepared boot.

  **Resolved 2026-10-30: folded in, behind a preflight.** Keeping it separate broke
  every cycle. `grub-reboot` arms exactly one boot, and getting onto the test boot
  uses it up, so the reboot *after* a cycle (planned, or `panic=10` after a real
  panic) landed on the default entry. That entry has no reservation, so the verifier
  could not read pstore (every cycle came back `FAIL(attribution-unverified)`) and the
  kernel was free to reuse the record's RAM.

  `crash_cycle.sh --reboot` now runs `reboot_preflight` before mounting anything:
  1. prove capture is live here (`ramoops_ensure`), which also proves this is the
     test boot. This is the accidental-run guard: on an unprepared boot it refuses.
  2. re-arm (`boot_arm`) so the next boot, planned or panic, is the test entry
  3. write a durable `ARMED` record for the cycle

  If any step fails, nothing starts and no cycle number is used up. The verifier
  calls capture live only if the crashed boot left `ARMED` for this cycle **and**
  this boot can read pstore. Before, it checked only the second, which is a fact
  about the wrong boot. The positive control re-arms before `echo c` for the same
  reason, and a later self-check run reports `POSITIVE CONTROL CONFIRMED`.
  `crash_boot_arm.sh` is now needed only once, to get onto the test boot.
