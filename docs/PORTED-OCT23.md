# Jetsku int20 integration (2026-10-23)

This update adapts Jetsku's changes from `5c3b39f76de85a0dc3292176ee04e85825ab47fb`
(int19.4) through `b719d67ce6bcfd21d402bd415f3e4f68e4793534` (int20 release notes)
to local main, starting at `c352a3f114d32bc1e75c3e828fadd6a1fe66da20`.
Upstream: https://github.com/Jetsku/KytyPS5 . The local
`feat/kyty-client-host` branch is preserved separately; none of its 24 newer commits
are folded into this update. No additional worktree or comparison installation is created.

## Included

- `df50aed1`: sparse texture residency `auto`, disabled for device-local heaps at least
  15 GiB (16 GB class); restore supported draw-run acquisition/recall options and log
  important stability switches at startup.
- `2eccedfc`, `560086ca`: CP self-label wait journaling and bounded background graphics
  pipeline compilation. Only draws without memory writes, active occlusion, mesh stages,
  linear attachments or attachment clears may skip while their exact optimized pipeline
  builds. Required draws consume pending work instead of compiling a duplicate.
- `21af402b`: late boolean phi copies in SPIR-V to avoid an NVIDIA 580 compiler crash.
- `557f8974`, `b8dd3094`, `c2e65fce`: resolve identity-backed shader-plan references and
  remove dead references after specialization/DCE; parallel compile regression tooling.
- `ace0c2b0`, `788e16b1`: read-only memory-mapped host CJK fallback fonts for guest libFont.
- `440998c9`, `ec9943e6`: optional early Windows CUDA initialization, disabled by default,
  with guest-address-space floor kept above 1 TiB.
- `ad58d1e2`, `476b2db5`: optional absolute-vblank timeline and unlocked presentation;
  both remain off by default. Existing local late-vblank and frame-overlay accounting stays.
- `b6fbe267`, `88eadd95`, `ee45446a`: diagnostic guest probes/command-buffer watermark,
  label deferral controls and split labels. Guest CPU reads see completion-time labels;
  CP waits see pending values in command order. Preset selects split mode.
- `8cfe12b4`: no-exceptions-safe cancellation of unstarted pipeline jobs.

## Local adaptations and review fixes

- Preserve local fast-first prefetch, optimizer reservation cleanup, journal retargeting,
  vertex parameter alias recovery, and earlier shader execution-select ordering.
- Do not expose preset switches for upstream-only target-reuse/pipeline-swap implementations.
  This fork's acquisition switch is `KYTY_DRAW_RUN_ACQUIRE`.
- Adapt the render guard to local `load_clear_aspects` and local draw-run state layout.
- Import missing guest-probe/CB-watermark prerequisites. Actual guest probe handling is
  Windows-only; Linux builds use the upstream no-op path.
- A published prefetch/skip pipeline moves its journal payload once. Moving it twice cleared
  the vectors and left an unusable replay record.
- Split waits always overlay pending bytes before comparison, even if physical memory would
  pass. Subsequent partial CP writes mask individual label bytes, preserving untouched
  overlay and completion bytes instead of suppressing an entire 64-bit label.
- Pipeline-skip eligibility checks pending HTile clears before acquisition, where this fork
  materializes its load-clear aspects. The background pixel test caught and now covers this.
- Preparation without title/recipe metadata still exits with an error and never enters gameplay.
- Offline shader compile tests restore this fork's stored vertex alias layout; absent upstream
  input fields and audit-test includes are not imported.

## Already present or deliberately omitted

RangeSet node reuse (`95ca5409`), INSERTQ expectations, and portable Qt image plugin
filtering (`740b7088`) are already present locally. Keep the fork's implementations.
Windows PGO binary profiles (`f8e58efe`, `5decdcb6`, `3f2204cb`) and Windows release
workflows are not imported into this Linux Release/ThinLTO build. Upstream release-note
merges are represented by this integration note instead of overwriting local documentation.

## Validation

Release build uses Clang/LLD, ThinLTO, and no PGO. Evidence, logs and the installation
rollback archive are private under `~/.local/state/kytyps5-jetsku-port-20261023/`.

- Targeted CPU/GPU suite: 71/71 passed, covering pipeline prefetch/fast-first/journals,
  preparation metadata failure, program cache, draw reuse, command sequencing, VM,
  timestamp/recorder, preset, launcher preservation and new label/parallel regressions.
- Parallel shader test: 590 programs, eight threads and three rounds, including programs
  whose plans contain references made dead by specialization.
- Split-label regression exercises overlapping/clipped reads, guest-memory isolation,
  partial resets and a real WAIT_REG_MEM packet against stale physical memory.
- Independent review found and verified the fixes above; deferred byte publication was
  reviewed statically. Windows-only CUDA/probe behavior is not tested on Linux.
- Follow-up suite after the HTile fix: 51/51 passed, including background-pipeline pixel
  rendering and ordinary rasterization (73 distinct targeted tests across the two suites).
- Private journal migration replayed all 911 shader permutations with zero compile failures,
  built 724 retargeted graphics pipelines and prewarmed 127 compute pipelines. Fifteen
  historical graphics recipes had unpublished modules; the original build reports nine
  on the same input journals. Preparation therefore reports partial coverage (exit 3),
  not complete coverage. Installation and warm-repeat results are recorded in provenance.

No gameplay benchmark is run and no FPS improvement is claimed. Gameplay comparison
remains with the user. The manual shader-precompile launcher button remains removed.
