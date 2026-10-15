# Pipeline look-ahead with fast-first fallback

`KYTY_PIPELINE_PREFETCH_FAST_FIRST=1` allows graphics pipeline prefetching and
fast-first compilation to coexist. It is experimental and defaults to off.
Enable it together with `KYTY_PIPELINE_PREFETCH=1` and
`KYTY_PIPELINE_FAST_FIRST=1` (or the existing vendor-qualified fast-first setting).

Draw preparation can request an exact graphics pipeline before the ordered draw
needs it. The existing bounded prefetch pool builds that pipeline optimized on a
worker. The draw consumes the matching result and waits if it is not finished.
When no earlier request exists, it uses the ordinary fast-first path; it does not
submit an optimized build merely to wait for it synchronously. All draws still
execute with their exact pipeline.

The pool retains its existing limits: two workers by default, configurable with
`KYTY_PIPELINE_PREFETCH_THREADS` (1–4), and at most 128 pending results. Pipeline
libraries retain their existing separate path. Turning the live
`KYTY_PIPELINE_PREFETCH` switch off stops new requests while allowing existing
jobs to be consumed.

Prefetched builds record their content keys in the known-pipeline set. Consumed
results carry their serialized pipeline state and shader identities to the
ordered publication path, which records them in the pipeline journal. The
prefetch diagnostic reports how many consumed results were journaled. Shader
modules and layouts remain alive until workers join and pending results are
released.

This moves work earlier only when preparation reaches a pipeline before its
first draw. It cannot predict an unseen shader/state combination, and a short
look-ahead window may hide only part of a long driver compile. Background work
also competes for CPU and driver resources. Gameplay comparisons must examine
actual draw wait time, not just worker compile time or average FPS.

## Validation

The GPU tests check literal blended-pixel results, serial cold misses with the
prefetch switch still on, worker-result consumption, live disabling, draw-run
reuse, and pipeline-journal record/replay. Separate queue, snapshot and
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
