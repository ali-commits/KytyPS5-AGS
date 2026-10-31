# Jetsku int21 integration (2026-10-25)

This port integrates the applicable source delta from Jetsku int20
`b719d67ce6bcfd21d402bd415f3e4f68e4793534` through int21
`744705ee1df4b0acb1af382ff5c32ca4a20aa5f7` into local main, starting at
`134f665926c04d418b2e7478310caf246c9400c4`.

Upstream: https://github.com/Jetsku/KytyPS5/commit/744705ee1df4b0acb1af382ff5c32ca4a20aa5f7 .
Safety ref: `safety/pre-jetsku-port-main-20261025`.

## Ported changes

- Shader lowering and resource tracking: packed D16 buffer loads/stores, F16 interpolation,
  byte/halfword accesses, uniform-buffer support, native float min/max option, clipping and
  stage LDS metadata, tessellation reflection and optional split-wave execution, plus
  smaller ALU/control-flow sequences and reduced debug naming in release shaders.
- Renderer: conditional GPU metadata clears, GPU-driven indirect draws, buffer re-resolution
  after joins, stencil-target separation, invalid color-target handling, optional small tiled
  image readback and read-only depth/stencil store behavior.
- Synchronization: queued upload copies drained/taken over before writes, deferred protection
  watcher fixes, recorder-side stream copies, command-processor safe-render waits, recurrent
  target restrictions for pipeline skipping and device-fault diagnostics.
- Runtime: fixed-chunk LRU storage, optional mapping-region epochs and helper spin gates,
  nonrecursive pool decommit, safer guest file writes, APR file/readahead improvements,
  filesystem/network readiness and guest service compatibility fixes.
- Audio/input: PS5 discrete ATRAC9 multichannel handling, NGS2 streamed PCM/custom racks,
  AudioOut2 context isolation, controller state coalescing and additional pad/VR support.
- Astro Bot 01.018: exact-version and instruction-table-matched built-in command-buffer
  enlargement (`KYTY_ASTRO_CB_SCALE`, defaults to 2; 1 disables).
- Optional OpenXR headset/controller support, with a pinned static loader; ordinary launches
  do not activate VR. Headset hardware is not part of this machine's validation.
- Launcher: ZAR game-folder compression, icon artwork, system/dark/light theme selection,
  tiled readback option and translated strings. Local and streaming launches continue to
  share one argument builder. Existing manual shader preparation UI remains removed.

## Adaptations and review fixes

- Retain local streaming capture, remote controller/motion/output ownership, Linux mapping
  probes, vertex parameter aliases, split-label byte masks and stale-wait fix, HTile skip-clear
  protection, fast-first prefetch journal ownership and fail-closed preparation mode.
- Retain the fork's existing CMASK MSAA behavior and unconditional buffer-range/cache-
  invalidation fixes. Do not import unrelated absent lane-local/branchless/GuestLoad/V-cache
  prerequisites. Windows TEB and other guarded options remain platform-specific.
- Retain the native float min/max option in code but disable `KYTY_NATIVE_FMINMAX` in this
  preset: enabling it on the RTX 3090 fails the MED3 NaN/denormal accuracy regression.
- Keep the existing boolean `--perf-overlay <true|false>` command interface and deduplicate
  already-ported keyboard-player/multiplayer functionality.
- Shader review corrections: F16-only smooth inputs retain their barycentric interface;
  split waves do not use an unproven half-local read-first shortcut; tessellation reflection
  keys the actual shader bytes and static stage keys include reflected patch layouts.
- Tiled targets enrolled for CPU readback cannot be skipped as visual-only draws. The same
  eligibility predicate drives enrollment and skipping for both color and depth.
- Draw-index indirect commands preserve zero-count SGPRs and propagate the executed last
  index to subsequent threaded parser snapshots through a lockstep result.
- Opt-in command spinning retains its atomic state through notification. Mapping-region
  cache invalidation covers Linux map/unmap, sparse writes and region boundaries.
- AudioOut2 submissions have context-owned shared lifetime; completed/destroyed contexts do
  not retain PCM buffers forever. Archive output creation is exclusive, validates source/
  destination aliases and directory cycles, and cannot truncate an existing destination.

## Cache compatibility

New shader input layouts require explicit migration. Same-device int20 journals using the
frozen `10152 / 224 / 96` byte layouts are converted field by field, with cleared pointers,
validated unchanged static keys and retained permutation identities. A verified replacement
is installed atomically after preserving the exact original as `.int20.bak`. Unknown layouts,
wrong-device journals and corrupt inputs are preserved instead of silently overwritten.

Old entries whose new LDS/tessellation metadata cannot be recovered safely remain explicit
unavailable entries. Preparation counts these omissions; a warm run must not claim complete
coverage merely because it skipped them. Current preset journal-retarget behavior is retained;
private preparation explicitly enables retargeting when refreshing the driver's cache.

Windows PGO profiles and release automation are excluded from this Linux Release/ThinLTO
build. Release-note history is represented here rather than overwriting this fork's notes.

## Validation record

Build/test logs, private cache migration/preparation, installation hashes and the rollback
archive are kept in `~/.local/state/kytyps5-jetsku-port-20261025/`.

Validation passed for shader codegen/GPU fixtures, journal migration, indirect-index state in
all CP modes, tiled-readback draws, archive preservation, controller/remote routing and
coalescing, AudioOut2/NGS2, VM/cache invalidation, pipeline replay and launcher preservation.
Final outcomes are recorded in this directory and in the installed provenance.

The final validation run passed all 156 named tests (139.25 seconds). The full GPU shader suite passed, and all 16 preset-mode
checks passed exercising uniform buffers with the accuracy-preserving float min/max path.
The native-min/max preset experiment is intentionally recorded as a failed accuracy check
and remains disabled. Shader migration tests include safe recapture of unavailable records
and indexed matching so large histories do not introduce quadratic startup work.

Private preparation preserves 605 of 969 recorded shader permutations; 364 lack required
legacy metadata. All 5,452 old graphics recipes remain unavailable to retarget under the
new interfaces. The migrated shaders compile without failures and 128 compute pipelines
prewarm, but graphics preparation is incomplete. First visits after this update can require
new shader/pipeline compilation. The old journal and complete installation rollback remain
backed up. These numbers describe this machine's captured history, not general game coverage.

This update does not establish that the reported Bathhouse Battle stop or Bluetooth transport
errors are fixed. Upstream int21 still documents rare GPU device loss in that level. Gameplay
comparison remains with the user; no FPS improvement is claimed by the port itself.
