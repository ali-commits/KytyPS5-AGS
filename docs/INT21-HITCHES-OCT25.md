# Recovering shader-cache coverage and investigating later crashes

Baseline main: `44355273e19bd560f29f173e6754c333c249e231`. The user wants the
near-stutter-free experience of the earlier int19.1-based installation. The changes
below were not installed while the user performed the additional morning runs.

## Captured hiccups

Run `20261025T033011.644170Z` lasted about 35 minutes and ended with exit 65. Its
115,497 flip intervals include stage changes and loading: median 16.669 ms, p95
33.157 ms, p99 33.738 ms, 88 intervals over 100 ms, and a maximum of 1,149.675 ms.
The long intervals total 35.75 seconds. The pipeline counter records 348 first-use
graphics builds taking 30.97 seconds synchronously; 872 cache probes cost 847 ms.
This establishes a substantial synchronous compilation cost, not an exact
per-pause attribution. File/asset loading and GPU thermal limiting also occur.

Later runs still used the same installed baseline: `043712.342658Z` built 28 cold
graphics pipelines in 3.79 seconds and `044720.311425Z` built 120 in 11.87 seconds.
These reports must not be presented as results from this uninstalled candidate.

## Changes

- Enable `KYTY_PIPELINE_PREFETCH_FAST_FIRST=1`. With fast-first enabled, the
  existing prefetch flag alone did not create look-ahead workers. Exact-key
  look-ahead prepares work earlier; draws still wait for their correct pipeline.
  Pipeline skipping remains disabled.
- Recover pipeline journals across descriptor ABI changes. The int21 descriptor
  kind count changed the binding numbers of later stages, leaving all 5,452 old
  graphics recipes unusable under strict old-layout matching. Registry entries now
  retain the current descriptor bindings and native stage for an exact shader
  permutation. Replay reconstructs the complete current layout and push signature,
  verifies stages and unique bindings, then computes the current pipeline key.
  Missing metadata remains unavailable. Old layouts are never reused merely by
  ignoring their hashes. Retargeting is enabled in the supported preset.
- Fix the local-array shrink validator to accept the standard UBO layout that the
  compiler emits only when the device supports it. Previously valid dword-stride
  UBOs caused validation to reject the optimization. The GPU regression demonstrates
  a 1,024-byte function array becoming 32 bytes with that UBO present.
- Identify modules by the exact SPIR-V passed to the driver, after shrinking and
  diagnostic salting. Hashing the original words could mark an obsolete binary
  warm after a driver-input transformation changed. Both ordinary and plain
  shader variants use the actual driver-input hash. Program-cache source words
  and shader-journal formats remain unchanged.

The newest cache snapshot, including the user's subsequent runs, resolves 5,800
graphics recipes. Private preparation built 726 additional pipelines, found 5,074
already known, replayed 888 shader permutations and prewarmed 128 compute
pipelines, with no compile failures or cache-limit omissions. A repeat preparation
must build zero graphics pipelines. There are still 152 unavailable shader
permutations and 1,122 unavailable graphics recipes; no complete coverage of unseen
stages is claimed. Preserve the source journals and newly captured progress.

## Distinct crash evidence

The user identified **Dude Raiding** for the final crash in the 35-minute run.
Original executable disassembly places `0x90703b76d` in graphics-data release,
`GfxDataManager.cpp`, checking/decrementing `m_uLoadCount`. Its caller is model
cleanup in `GfxSceneManager.cpp`, passing the graphics-data pointer from model+0x18.
The saved model is `0x32c9374e0`, while `r14`, normally the resource pointer, contains
the invalid value `0x0001000000000000`.

The capture cannot distinguish already-corrupt model data from corruption of the
saved register during the intervening mutex call. The machine code matches the
original executable. `deadbeef00000007` is the normal stack canary. APR and mapping
epoch optimizations were off; static review found no established writer. This is
not assigned the earlier GPU loss or repaired atomic missed-wake cause.

An additional baseline run, `20261025T043323.249995Z`, exits at the existing texture
base-layer bounds check in `descriptors.cpp:711`, with zero cold graphics builds.
That check and layer normalization are unchanged between int20 and int21. The
capture lacks the descriptor values needed to distinguish malformed input from
an unsupported valid texture view.

Fatal reports now optionally capture bounded register-object windows through
checked OS memory-copy APIs, using the existing tested `FaultMemoryDump` selector.
This can compare model+0x18 with the saved resource pointer without a terminal
command or debugger race after process exit. The texture-layer failure also prints
the raw descriptor and dimensions. These are diagnostic improvements, not claims
that either later crash has been fixed; no pointer masking, layer clamping or
ignored cleanup operation is added.

## Validation and practical limits

The stale-layout GPU fixture fails on the baseline, then passes after rebuilding
layouts. It checks that all actual draw keys are covered, then checks a warm replay
builds nothing. The UBO/local-array fixture also fails on the baseline and passes
with matching validator options and the transformed module hash. Existing exact-key
look-ahead, draw-run and journal replay checks pass. Full evidence and final test
outcomes are in `~/.local/state/kytyps5-hitches-crash-20261025/`.

A host reboot interrupted an intermediate test batch; incomplete logs are retained
and must not be described as a passing suite. Completed checks are rerun afterward.
No game-level FPS comparison is claimed from cache preparation or synthetic fixtures.
Saves, current user settings, patches, trophies and other agents' checkouts are
preserved. This remains one normal KytyPS5 installation on main.
