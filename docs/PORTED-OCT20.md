# October 20 upstream ports

Candidate branch: `ports/oct20-high-next`, based on streaming/main `b4a7b481`.
The candidate is installed separately as **KytyPS5 Ported** (`kytyps5-ported`).
**KytyPS5 Streaming** (`kytyps5-streaming`) remains the baseline. Main and the
streaming branch are not advanced by this work.

## Selected fixes

| Upstream commit | Behavior |
| --- | --- |
| `440245f6` | Bound image copies by both source and destination mip extents. |
| `099de8de` | Publish CPU PM4 WRITE_DATA DWORDs once through volatile stores. |
| `e5f0ee7f` | Preserve finite buffer descriptors across sparse PRT holes. |
| `fde0b98e` | Include the payload extent of nonempty zero-stride vector OOB2 buffers. |
| `cd4eab9d` | Cap sentinel buffer writes, as well as reads, to the device storage limit. |
| `7675579e` | Implement real kernel/POSIX fsync and durable Linux file flushing. |
| `22ef16a0` | Emit device acquire barriers and coherent buffers for GL0/GL1 invalidation. |
| `e9bcc77e` | Evaluate runtime-uniform scalar 64-bit equality/inequality resource guards. |
| `ac3711ce` | Select the lower primary mip for linear explicit-LOD gathers. |
| `defb2f11` | Correct MED3 signed-zero ties and share operand classifications. |
| `33208a54` | Remove the artificial 64 image/sampler-pair cap. |

Full commit identities and original upstream classification are recorded in
`~/.local/state/kytyps5-ported-20261020/selected-ports.json` and the prior upstream
review under `~/.local/state/kytyps5-consolidation-20261020/`.

## Fork adaptations

Buffer sizing is shared by serial bindings, draw-preparation plans and the
specialization-write overlap guard. Finite buffers probe their entire effective
range for committed memory; otherwise the fork's default null-normalization
would discard descriptors whose first page is a sparse hole. Sparse buffer uploads copy committed VM segments and zero both reserved and
unregistered gaps in staging and direct stream copies. Sentinel/oversized ranges retain
prefix clamping. Device caps reserve the alignment prefix for
both writable and read-only sentinels. The fork already rejects writes that
overlap captured specialization reads during materialization; upstream's
separate snapshot-read narrowing mechanism is not introduced.

GL0/GL1 decoding already existed. Coherent-buffer requirements belong to this
fork's SPIR-V requirement analysis rather than upstream's ShaderInfo structure.
The MED3 correction also applies to the fork's fast min/max path. Scalar buffer
loads keep an explicit whole-DWORD bounds check because NVIDIA can return data
from a one-byte native range through its robust-buffer fast path. Biased or
mip-snapped gather modes retain the existing approximate fallback; supported
linear gathers now select the correct primary mip. The 64-bit guard regression
exercises the retained-plan evaluator and descriptor reachability, rather than
upstream's different conditional scalar-read/JIT architecture.

Mixed scalar/vector uses sharing one zero-stride descriptor retain upstream's
existing `scalar` metadata limitation. No unrelated planner/JIT, benchmark-group,
mutex, GDS, EQAA, theme or revert changes are included.

## Verification

Build test executables with `cmake --build _Build/local-release --target kyty_tests -j8`.
Run `ctest --test-dir _Build/local-release --output-on-failure -j2`.
The native target is `kyty_emulator`.

Regressions exercise sparse descriptor materialization and native readback,
shrinking volume copies, zero-stride vector/scalar/OOB rules in wave32/wave64,
unaligned writable/read-only sentinel caps, expanded-write specialization
aliases, full-width resource guards, more than 64 sampled pairs, SPIR-V cache
fences and coherent reloads, linear gather mip boundaries, MED3 result bits and
POSIX fsync errors/offset preservation. Linux syscall tracing checks that fsync
reaches the host kernel.

Machine-specific build/test logs, installation provenance and protection hashes
are kept in `~/.local/state/kytyps5-ported-20261020/`.

## Comparing the installations

Open **KytyPS5 Streaming** or **KytyPS5 Ported** from the application menu, one at
a time. Both started with the same launcher, preset and copied game configuration,
saves, patches and trophies. The stutter repair below updates Ported only. The candidate has independent writable directories:

- Program: `~/.local/opt/kytyps5-ported/20261020-high-next`
- Settings and streaming profile: `~/.config/kytyps5-ported/20261020-high-next`
- Saves and caches: `_SaveData` and `_PipelineCache` inside its program directory

Changes made by playing one version do not propagate to the other. Streaming
identity/pairings are copied into separate files for sequential comparisons.
The launcher retains its selected-game **Precompile Shaders** button and progress
bar. Prepare/warm both versions before comparing gameplay: code-generation
changes invalidate the candidate's copied program cache automatically.

Use the same game, save checkpoint, level, resolution, presentation mode and
profiling options, and alternate the run order. Enable the launcher's profiler
option for Tracy captures and use the same instrumentation in both versions.
Shader/pipeline preparation timing is separate from gameplay FPS; a faster
preparation run does not establish a gameplay speedup.

## Verified on October 20, 2026

The Release build and all 432 CTest checks passed: 431 core checks in 332.15 s
with two jobs, plus the launcher streaming check run separately in 81.91 s.
The six focused GPU regression runs passed on the NVIDIA GPU, including the
four-layout sparse-buffer readback and default/explicit scalar bounds paths.
Linux syscall tracing observed successful `fsync` on the test save file.

## Ported shader-cache migration repair (October 20)

The first reported Ported gameplay run spent 98,353.6 ms in 755 fast-first
pipeline builds; the worst recorded compile stall was 1,591.1 ms. Streaming's
comparison run had 782 driver-cache hits, five fast builds and a 17.0 ms worst
compile stall. These are compile counters, not frame-time/FPS benchmarks.

The ports changed translated shader content. The copied known-pipeline index
still described old module hashes, and the bundled RETARGET=0 preset disabled
migration. Even with retargeting enabled, replay skipped a known old pipeline
before resolving the current modules. Replay now resolves compatible current
permutation identities and checks the derived current key before skipping.
The launcher's Precompile Shaders action explicitly enables retargeting even
when the parent environment disables it. Ported's installed preset enables it
for normal starts as well.

A second gap affected mixed flat/smooth vertex parameters: stored input bytes
predate alias metadata, while static keys already contain it. Restore and
validate the aliases from the recorded static key before shader replay and
pipeline-list normalization. Keep the journal layout and full derived-key
validation intact.

The new GPU regression records real aliased vertex shaders, substitutes
unavailable old module hashes and known old keys, and verifies that retargeting
builds all pipelines and the next process reuses them without another build.
It failed before each repair and passes afterward. The launcher regression
checks that preparation overrides an inherited RETARGET=0. All 80 related
pipeline, program-cache, draw-preparation and launcher CTests pass.

Preparing a private clone of the user's game cache rebuilt 1,074 graphics
pipelines and restored all 880 shader permutations. A repeat prepared zero
additional graphics pipelines, recognized 3,891 recorded recipes as warm, and
completed native preparation in 1.4 seconds. Nine legacy recipes have a zero
permutation identity for one stage, so preparation correctly reports them as
unavailable rather than guessing a shader. New areas can still introduce
previously unrecorded shaders/pipelines; gameplay route verification remains
necessary. Preserve both journals, saves and the Streaming installation.

Machine-local evidence and pre-repair backups are in
`~/.local/state/kytyps5-ported-stutter-20261020/`.

## FPS candidate: cached sparse-buffer probes (October 20)

The sparse-buffer adaptation replaced the old cached committed-prefix query
with a locked search in `HasCommittedRange` for every qualifying buffer during
materialization. Restore the existing generation-checked per-thread cache for a
committed first byte; retain the full search when a descriptor starts in a hole.
The test covers sparse tails, half-open boundaries, invalid/overflowing ranges,
unmap/remap/protection changes and actual cache hits.

The isolated 200,000-query-per-reader benchmark changed from 2.826 to 0.939 ms
with one reader, 47.602 to 1.746 ms with four, and 124.068 to 1.804 ms with eight.
These are probe timings, not gameplay speedups.

Matched private Sky Garden tests use `underwater_aerial_garden`, identical save
state, a stationary camera, warm caches and no preparation-phase instrumentation.
The CPU-only candidate measured 32.78 FPS versus 32.11 in the repeated unmodified
Ported control. The first control had measured 30.38, so run variation exceeds
the small observed gain; the historical 25% whole-session difference and the
reported sub-10 FPS episode remain unattributed. GPU software thermal throttling
was observed. No large FPS recovery is claimed. A separately tested scalar-load
shader rewrite was not retained because its independent performance benefit was
not established.

Ported's existing launch capture now samples resources at 1 Hz through
`tools/diagnostics/runtime_resources.py`: CPU, RSS, high-water RSS, I/O/faults,
thread CPU placement and NVIDIA utilization/clocks/temperature/throttling.
CPU percentages use 100% per logical CPU; GPU telemetry is device-wide.
Missing data is recorded as unavailable. Sampling ends with the game and does
not attach a profiler. The launcher wrapper imports the installed helper from
its private `libexec` directory. Logs remain in Ported's `_CrashCapture` run
folder. Streaming's wrapper, installation and settings are untouched.

Run `python3 -m unittest discover -s tests -p test_runtime_resources.py` and the
three `virtual_memory_allocation*` CTests. Machine-local evidence, backups and
installation verification are under
`~/.local/state/kytyps5-ported-fps-fix-20261020/`.

Upstream was checked through `f186b357`. The `fa8216c4` per-thread LDS sizing
change is relevant to 23 recorded pixel permutations with live 32 KiB private
arrays in both builds. It is a separate possible improvement: raw shader-input
journal compatibility and the missing legacy allocation metadata need explicit
handling before porting. The stage-input null-check cleanup is translation-time
work and offers no established warmed-game FPS benefit. Neither update is part
of this candidate.
