# Jetsku int22 merge (2026-11-19)

`testing` merges Jetsku int22 (`u59-windows-20261118-int22`, `dd9f9901d`) in full, with Jetsku int21
`744705ee1` as the merge base (our last Jetsku sync, see [PORTED-OCT25](PORTED-OCT25.md)). int21..int22
is 1,100 commits; the merge conflicted in 174 files.

## What comes in

- Astro Bot and Astro's Playroom speed work: commit-thread cuts, NVIDIA codegen switches, eager write
  read-back for the sponge level (`KYTY_READBACK_EAGER_WRITES`), lane-local compute (`KYTY_LANE_LOCAL`).
- Shipped shader lists (`PipelineLists/`), Precompile, shader list export, cross-version list matching.
- Per-title presets `u59-preset.<TITLE_ID>.json`, installed next to the emulator. The launcher applies
  them; under AGS the emulator applies them itself (`KYTY_TITLE_PRESET` unset).
- Launcher: Recommended settings per game, "Faster GPU synchronisation (experimental)", 1920x1080
  default window, CPU check.
- The GPU hang fix for compute dispatches writing through computed addresses (BDA store log), native
  ray queries (`KYTY_RT_HW`, off), call-split dispatch, bindless tables, Ghost of Yotei / GT7 / Ratchet
  work (mostly behind default-off switches).
- Upstream KytyPS5/KytyPS5 through `bc934ef03`. It contains the commits this fork had ported by hand in
  0.8.0 (Pro mode, linear PSSR, Psml, DLSS for PSSR, kernel signal and flip changes); int22's versions
  replace ours, with the fork's adaptations (Linux NGX loading, PSSR DLSS independent of the game's
  upscaler) kept.
- Host code is built for x86-64-v3 (AVX2, BMI2, FMA); the emulator says so and exits on older CPUs.

## Kept from this fork

AGS adapter, streaming, Linux support, Reflex low latency, frame generation (lsfg-vk), FSR2/DLSS
upscaler, native resolution, per-version cheats, hang watchdog, pipeline-journal migration and
fast-first prefetch, CMASK fast-clear check, Linux `KYTY_CPU_RESERVE`, the whole-DWORD scalar bounds
check, unconditional buffer-range and invalidation fixes. Aftermath and the splash screen stay removed
(the splash HLE export is a no-op).

## Verification (Linux, RTX 3090)

AGS workflow tests 17/17; int22 supplemental tests 78/78; full GPU compute suite and its 16 variants
17/17. Astro Bot 01.019 runs with its title preset (180 s, two level loads, no errors). Windows and
the int22 Linux package are unverified.
