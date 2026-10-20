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
a time. Both use the same launcher, preset and initial copied game configuration,
saves, patches and trophies. The candidate has independent writable directories:

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
