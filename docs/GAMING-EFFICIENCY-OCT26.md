# Gaming efficiency investigation — 26 October 2026

Branch `perf/gaming-efficiency-20261026` starts at the user-validated local main
revision `a95ba42b3da845d12a1c4339da978186ccca4007`. The original checkout's
unfinished narrow-conversion changes are separate and remain untouched. No game,
launcher or streaming host is started during this investigation.

## Evidence from the existing user run

Capture `20261026T081012.909059Z` used the rebased Streaming installation,
`5fbfb07b`, whose renderer is based on that same main revision. It exited normally
after approximately 30.6 minutes. Whole-run measurements include loading and stage
transitions; they are not a scene-matched benchmark.

| Measurement | Recorded value |
| --- | ---: |
| Average flip rate | 53.28 FPS |
| Flip intervals over 100 ms | 17 |
| Longest flip interval | 1.317 s |
| Mean process CPU usage | 10.07 logical CPUs |
| Peak process resident memory | 17.78 GiB |
| Peak device-wide video memory | 14.32 GiB |
| GPU software thermal slowdown | 1,376 of 1,839 samples |

Resident memory also falls during the run, and device-wide GPU measurements do
not identify which allocations belong to the emulator. These figures do not prove
a leak. The GPU reaches 87°C; the thermal slowdown flag is direct evidence of a
separate constraint on performance, not a quantified explanation of the FPS.

## Selected changes

### Reclaim unused completed pipeline predictions under capacity pressure

`PrefetchState` holds predictions until their exact key is consumed or the cache
shuts down. A legitimate prediction can become obsolete when prepared resource
state fails commit validation. With 128 such entries, later requests are rejected
even after every worker finishes. Look-ahead can therefore stop for the rest of a
session, retaining unpublished pipeline handles and optimizer reservations.

At capacity, normal prefetch can reclaim the oldest completed, unclaimed result
through its existing discard path. An existing requested key, unfinished build,
or result already claimed by a draw must remain protected. The optional keyed
skip-builder retains its existing behavior, since evicting its completed results
could cause additional skipped draws.

The recorded user run retained two unused predictions; it did **not** demonstrate
128-entry saturation. This fixes a reproducible capacity failure and is not an
explanation of that run's average FPS.

### Name Linux workers even when Tracy is unavailable

Native thread naming was conditional on the manual-lifetime Tracy profiler being
available. Consequently several busy workers appeared as `kyty_emulator` in the
resource capture. The Linux fallback sets the calling thread's native name while
preserving the Tracy lifetime guard and the watchdog's full names. This improves
CPU attribution; it does not itself increase FPS.

## Next targets, ranked by current evidence

1. **Linux memory write tracking.** Existing logs report substantial accumulated
   fault-handler time, varying from roughly 3 to 20 ms per frame. This sums work
   across threads and must not be interpreted as an equivalent frame delay.
   Named worker samples and the existing fault-cost instrumentation should
   separate lock contention, protection calls and readback before changing the
   implementation. The opt-in userfaultfd path needs coherence validation before
   a gameplay experiment.
2. **Memory attribution.** Use the existing VRAM diagnostics to distinguish live
   textures, retained pools, buffers and driver allocations. Clearing working
   caches or reducing residency without that distinction could introduce stalls.
3. **Redundant mesh pipeline keys.** Topology and primitive restart remain in
   cache keys even when mesh pipeline creation ignores input assembly. A fixture
   should first prove identical rendering and pipeline reuse with those fields
   normalized; draw-time restart behavior must remain intact.
4. **Duplicate program-cache additions.** Failed duplicate insertion retains
   payload storage outside the byte counter. Normal callers suppress most
   duplicates, so ordinary-game impact is unproven. Its source participates in
   the shader fingerprint; a fix has a cache-rebuild cost to weigh.
5. **Repeated phi traversal.** Dead-phi analysis repeatedly traverses live phi
   graphs with linear membership checks. Establish a representative compilation
   benchmark before rewriting it; this also changes the shader fingerprint.

The shipped CPU-reservation setting is currently Windows-only. A Linux port needs
affinity and SMT-aware evidence rather than merely enabling the same preset flag.
No new shader correctness failure was confirmed beyond the separate pending
narrow-conversion fix.

## Validation

- The new capacity regression fails on the original implementation at prediction
  129. After the fix it passes, including exact pixel readback, consumption of the
  later prediction, bounded optimizer reservations and shutdown cleanup.
- The capacity case passes with layout interning enabled and disabled. Another
  12 CTest checks pass, covering prefetch, fast-first drawing, draw-run paths,
  journal recording/replay, skip-builder rendering and inline preparation.
- The native thread-name regression fails on the original implementation and
  passes after the fix. All five functional profiler test groups pass with
  `--no-benchmark`; all seven resource-capture Python tests pass.
- Both changes pass independent code review and changed-line formatting checks.
  The whole-file formatting hook would also rewrite unrelated existing code;
  those formatting-only edits were excluded from these focused commits.
- The matching Clang/LLD 23.1.3 build retains shader fingerprint
  `5a18411e7dc30c2ba48c76c644e165a6b50a02b3e25d339687637330d700a03b`.

The functional fixes are commits `3568ca3` (native thread names) and `502f442`
(prefetch capacity). No installation or saved-state changes are part of this
branch. No FPS or memory reduction is claimed from static analysis or synthetic
correctness tests.

Private evidence is retained in
`~/.local/state/kytyps5-gaming-efficiency-20261026/`, including the source-hashed
run summary and the original main-checkout diff.
