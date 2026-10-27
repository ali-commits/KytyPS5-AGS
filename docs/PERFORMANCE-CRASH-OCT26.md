# Performance candidate: crash and follow-up runs, 26 October 2026

Both runs used performance candidate `4eb14381`, native SHA-256
`d2260bfc0a837249e9b13a33aa586ccd5afbada400978f19c9baa2fe96e86041`.
The user reported a crash in the first run and closing the second. The initial
level identification was withdrawn; the crash level is unknown.

## Recorded results

| Measurement | First run: 11:03:56 UTC | Second run: 11:16:02 UTC |
| --- | ---: | ---: |
| Duration | 12.0 minutes | 8.6 minutes |
| Process exit | 65, guest fault | 0, user closed |
| Whole-run average flip rate | 47.53 FPS | 48.97 FPS |
| Flip intervals over 100 ms | 15 | 37 |
| Longest flip interval | 617 ms | 950 ms |
| Peak process RSS | 16.11 GiB | 15.33 GiB |
| Peak device-wide VRAM | 9,457 MiB | 8,745 MiB |
| Major page faults | 1,925 | 31,378 |

Settings match except for the output paths of per-run diagnostics. Different
scenes, durations and cache history prevent a controlled FPS comparison. These
are existing user-run measurements, not new benchmarks. GPU memory measurements
cover the whole device.

## First run: damaged guest allocator pointer

At 11:15:58 UTC, guest thread `BackgroundTaskWorker` faults at `0x900aeb4be`.
The original executable and captured instruction bytes agree: `mov rcx,[rax-8]`.
The preceding load reads the allocator's free-list head at `R15+0x78`.

Both the captured memory slot and RAX contain `0x00010003270337c0`. Thus the
invalid pointer is already stored in guest memory. Its effective address is
noncanonical on this host's 48-bit virtual-address implementation. The exception
report's address zero must not be mistaken for a null-pointer diagnosis.

Producer instructions use ordinary pointers: local frees chain through
`[task-8]`, and remote frees use native `lock cmpxchg`. No pointer-tag removal,
EXTRQ or INSERTQ occurs in the consumer. Masking the high bit would conceal the
corruption rather than repair an established encoding rule. The writer of the
bad value remains unknown.

The historical Dude Raiding crash also had an unexpected bit 48 before the two
performance-candidate changes existed. This resemblance does not establish a
shared cause. Review of the candidate's changes found no direct guest-memory
writer: one handles unpublished Vulkan pipeline results, the other names native
threads. The logs do not count prefetch reclamations, so they cannot prove that
reclamation never occurred.

GDB reached the first process only after it was a zombie. Its generic ptrace
warning does not prove a permissions problem. The register and object windows
captured by the emulator remain useful evidence.

## Second run: host-kernel fault despite normal process exit

At **11:24:17 UTC / 19:24:17 MYT**, Linux reports a general-protection Oops in
`__refill_objects_node`, while `OdxAsyncLoader/570381` is executing `read()`.
The stack passes through slab allocation, Btrfs readahead and file-cache reads.
The noncanonical kernel pointer is `0xfffe88ce0b642bd0`; the failing access adds
eight bytes. A subsequent `tsk->plug` warning occurs while that thread exits.
The kernel's taint gains the `D` flag for an Oops.

This is a confirmed host-kernel failure. A shared corruption or stability issue
is plausible because the guest and kernel pointers both differ at bit 48 from
plausible canonical addresses. It is not proof of defective hardware, a
particular driver, or a particular kernel bug. No preceding MCE, EDAC, OOM or
NVIDIA Xid appears in the available boot journal. The user reports no recent
crashes in other applications; CPU/RAM tuning settings were not established.

Reboot before further gameplay comparisons. Linux's
[issue-reporting guidance](https://www.kernel.org/doc/html/latest/admin-guide/reporting-issues.html)
explains that a kernel can misbehave after an Oops and later problems may be
consequences of the first one. No kernel, driver, firmware, BIOS or global memory
setting was changed during this investigation.

## Performance attribution

After the first 60 seconds, the named command-processor thread averages 87.7% of
one logical CPU in run one and 83.6% in run two; its 95th percentile is about
96–97%. Its instantaneous sampled CPU is an efficiency core in 44.2% and 31.7%
of observations. These samples are not residency measurements.

Run one's 420–480-second interval averages 38.24 FPS with the command processor
at 94.7% CPU and the GPU at 56.7%, supporting a CPU-side bottleneck. Run two's
worst ten-second interval, 400–410 seconds, averages 14.15 FPS, with the command
processor at 82.1% and the GPU at 34.4%. It contains only 28 major faults and
3 MiB of disk reads. Its slowdown begins before the kernel Oops and before the
largest later page-in bursts. GPU thermal slowdown is absent during the later
420–480-second interval, so thermal throttling does not explain that collapse.

Later in run two, major faults and disk reads increase sharply while RSS falls.
This warrants memory-pressure investigation but does not establish swapping:
file-backed faults can also require disk reads. Likewise, the existing summed
fault-handler time includes concurrent work and is not equivalent to frame delay.

## Improvement and next proof

The resource collector now records process swap, host memory and swap
availability, system swap-in/out counters, memory pressure, and boot/kernel
identity and taint. Missing readings remain unknown. Sampling stays at its
existing low rate and does not scan process mappings or start a profiler.
Process `VmSwap` excludes swapped shared memory; host swap counts and pressure
therefore remain separate measurements.

New regressions fail on the previous collector and pass after the change. All
10 Python tests pass, including units, unavailable/malformed/denied readings,
counter resets and child-process cleanup. Independent review found no blocking
issue. This validates the collector, not host stability. No native or GPU stress
test was performed after discovering the kernel Oops.

The Python collector update is installed only in the Performance Test runtime;
its native emulator remains the tested `4eb14381` binary. Current and Streaming
installations, gameplay settings, saves and shader caches are unchanged.

After reboot, a user-run diagnostic session can enable the already available
`KYTY_CP_GAP_STATS=1` and `KYTY_FAULT_MAP=1` to separate command processing,
protection calls, tracker locks, recorder waits and starvation. These diagnostic
settings can affect overhead; that run should not be presented as a clean FPS
benchmark.

Linux CPU reservation is still a possible later experiment, not an implemented
fix here. Windows process-default CPU sets have no direct Linux equivalent;
new Linux threads inherit their creator's affinity. A naive port could confine
lazy preparation workers to the CP's core and make contention worse.

No crash fix or FPS improvement is claimed. The immediate prerequisite is a
fresh boot and a clean capture. Original run copies, hashes, analysis and the
kernel journal are retained privately under
`~/.local/state/kytyps5-bot-of-war-20261026/` (the folder retains its initial,
subsequently withdrawn level label).

## Follow-up after reboot — 27 October

The relevant crash is capture `20261027T030543.157624Z`, ending at **11:20:52
MYT** with exit 65 after about 15 minutes. A subsequent ten-second launch,
`20261027T032055.272318Z`, exited zero and is not the crash being diagnosed.
The boot ID changed, and kernel taint stayed at `12288` (`O/E`) throughout the
crash run. The current boot journal has no new Oops, MCE, NVIDIA Xid or OOM event.
This reproduces the guest failure after a reboot without a new kernel Oops.

This time `RoomLoad_ATQT` faults in guest cleanup at `0x9004eb61d`. A vector-like
allocation header contains `{0x0001000000000000, 0, 0}`. Its invalid start pointer
makes the size subtraction wrap and select a large-allocation cleanup branch;
reading `[start-8]` then faults before `free` is called. The pointer is already
present in captured memory, and all 96 nearby instruction bytes match the
original executable. This is a different guest subsystem with the same bit-48
pattern. The previous value and the writer remain unknown.

The run averaged **47.62 FPS**, with **16.77 GiB peak RSS**. At least **8.55 GiB
of host RAM remained available**. There were 89 major faults in total and none
in the final 129 seconds; memory-pressure stall time was about 0.37 seconds over
the whole run. Process VmSwap was zero, with its shared-memory limitation noted
above. Host swap counters also show no significant activity near failure.
Memory exhaustion or paging is therefore unsupported as this crash's cause.

During seconds 900–906, the command processor used 96.3% of one logical CPU while
the GPU averaged 58.5% utilization and the game averaged 34.5 FPS. The late
984 ms frame interval is close to failure and may include crash handling.

A bounded `memtester` smoke test returned zero, but its requested 128 MiB was
reduced by the user's locking limit to **8 MiB actually locked and tested**.
This small pass cannot establish the health of all RAM or exclude an intermittent
CPU/cache/memory error. Btrfs also reports historical `corrupt=1` counters on both
NVMe devices at boot; these are not fresh errors attributable to this run. Both
devices belong to the same filesystem, so an NVMe0 path alone cannot exclude
NVMe1 involvement.

The CP-gap and fault-map flags were absent from the crash run. They are now
temporarily enabled by the Performance Test launch command and its menu shortcut,
so the next user run records them automatically. Explicit environment values can
override these defaults. This is an instrumentation change, not a crash fix;
timing comparisons must account for diagnostic overhead. Native binaries,
Current/Streaming launchers, saves, shared settings and shader caches remain
unchanged. Evidence is in `~/.local/state/kytyps5-crash-20261027/`.
