# Pipeline look-ahead with fast-first compilation

`KYTY_PIPELINE_PREFETCH_FAST_FIRST=1` allows graphics pipeline prefetching and
fast-first compilation to coexist. It is experimental and defaults to off.
Enable it together with `KYTY_PIPELINE_PREFETCH=1` and
`KYTY_PIPELINE_FAST_FIRST=1` (or the existing vendor-qualified fast-first setting).

Draw preparation can request an exact graphics pipeline before the ordered draw
needs it. With fast-first enabled, the bounded prefetch pool uses the same policy
as a cold draw: probe a known optimized entry, or build the exact pipeline with
optimization disabled. The draw waits for and publishes that usable result, then
queues optimization in the background. An early prediction therefore cannot
bypass fast-first and force the draw to wait for full optimization. A cold miss
without a prediction uses the ordinary fast-first path. All draws still execute
with their exact pipeline.

Fast graphics and compute builds share a transient pipeline cache so new state
variants can reuse shader work within the process. It is separate from the saved
optimized cache and is destroyed at shutdown. Only optimized builds populate the
persistent cache and its known-key set. Fast-first queue limits and unsupported
create-info fallbacks still apply; a driver can also spend substantial time on a
first shader even when optimization is disabled.

The pool retains its existing limits: two workers by default, configurable with
`KYTY_PIPELINE_PREFETCH_THREADS` (1–4), and at most 128 pending results. Pipeline
libraries retain their existing separate path. Turning the live
`KYTY_PIPELINE_PREFETCH` switch off stops new requests while allowing existing
jobs to be consumed.

Consumed results carry their serialized pipeline state and shader identities to
the ordered publication path, which records them in the pipeline journal. The
prefetch diagnostic reports how many consumed results were journaled. Shader
modules and layouts remain alive until workers join and pending results are
released. An unused fast prediction releases its optimizer reservation and its
pipeline/layout resources before the optimizer stops; it does not schedule an
optimized replacement for a pipeline that no draw published.

This moves work earlier only when preparation reaches a pipeline before its
first draw. It cannot predict an unseen shader/state combination, and a short
look-ahead window may hide only part of a long driver compile. Background work
also competes for CPU and driver resources. Gameplay comparisons must examine
actual draw wait time, not just worker compile time or average FPS.

## Validation

The GPU tests check literal blended-pixel results, serial cold misses with the
prefetch switch still on, worker-result consumption, live disabling, draw-run
reuse, pipeline-journal record/replay, new predicted state variants retaining
fast-first with exact pixels, and cleanup of unused fast predictions. Separate queue, snapshot and
fast-first tests retain their existing coverage. Run the relevant tests after
building `shader_recompiler_compute_tests`, `pipeline_compile_queue_tests`,
`pipeline_snapshot_tests` and `pipeline_fast_first_tests`:

```sh
ctest --test-dir <build> --output-on-failure \
  -R '^pipeline_(prefetch|fast_first|compile_queue|snapshot|journal)'
```

Real-game validation should use the same executable, game/profile, saved
position and cache seed for control and candidate, changing only the combined
mode setting. Keep existing caches backed up. Record pipeline requests/uses,
residual draw waits, compilation, GPU clocks and temperature, visual behavior,
and area-entry stalls. No gameplay improvement is assumed from test success.
