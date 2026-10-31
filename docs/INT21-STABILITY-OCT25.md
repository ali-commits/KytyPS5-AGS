# int21 gameplay stability investigation — 2026-10-25

Baseline: main `4fea7be47b9a8eb3a6683e37fe99f81977f4fc05`, installed native
SHA-256 `848f313c0fb16a697a0a62798dc4559c9dc1cec578f2eceb58cefcee0ee8af43`.
User identified these three runs, in order:

| Capture (UTC) | Level | Skipped draws | Background pipeline build time | Synchronous fast-first build time |
| --- | --- | ---: | ---: | ---: |
| `20261025T022004.952312Z` | Free Big Brother | 73,544 | 79.52 s | 17.22 s |
| `20261025T022959.882800Z` | Trapped in Time | 4,446 | 44.50 s | 9.67 s |
| `20261025T023816.736803Z` | Trapped in Time | 488 | 8.67 s | 1.78 s |

Build times are cumulative counters, including concurrent background work; they are not
individual frame times. These runs did not record individual flips. Program-cache saves
occur on a separate saver thread, so their elapsed write times do not establish equivalent
render-thread stalls.

## Confirmed findings

The pipeline-skip path deliberately returns without drawing until a new pipeline is ready.
Its recurrent-target check does not guarantee every omitted object will be drawn again.
This explains the first-run disappearing/flickering elements and their later appearance.
The int21 shader interfaces invalidated old graphics recipes; all 640 graphics keys created
in the first run were absent at startup. First-use compilation also explains substantial
hitching, with markedly less compilation on each later run.

Free Big Brother stops rendering at 10:29:31 MYT, coinciding with NVIDIA Xid 32 attributed to
PID 96090. The upload-DMA implementation documents this exact level, `big_hand_pirate_island`,
as a reproducer of late host copies seeing future guest data. Overlap-only write draining
does not order a nonoverlapping completion label before the guest reuses an upload source.
Upstream commit `8912166d` documents the stricter `labels` mode closing residual failures
under delayed-worker testing. This is a strong candidate for the observed GPU failure;
the captured kernel error alone does not identify the offending command.

Trapped in Time becomes idle at 10:37:32 and 10:43:59 MYT. The last partial core was requested
by an external SIGABRT at 10:44:11, then interrupted by SIGKILL at 10:44:16. It is not evidence
of a spontaneous native abort. It shows a trigger-effect thread waiting for SDL's joystick
mutex and an audio submission waiting for the audio mutex, but lacks the native stacks and
heap pages needed to recover the owners and prove a lock cycle. No OOM-killer evidence was
found. NVMe1 timeout messages overlap the second run, but the game and installation are on
NVMe0; a causal link is not established.

## Candidate fixes and diagnostics

- Ship `KYTY_UPLOAD_DMA_WRITE_DRAIN=labels`: queued upload sources are copied before a
  guest-visible write can release the guest to reuse them.
- Disable `KYTY_STREAM_COPY_DEFER`: the new recorder-copy path has the equivalent
  nonoverlapping-label gap. Keep copies at their original CP ordering point.
- Disable `KYTY_PIPELINE_SKIP`: missing pipelines no longer intentionally erase objects.
  Unseen pipelines may still cause first-use stalls; this does not promise universal warm
  coverage or unchanged average FPS.
- Retain the current shader interfaces and newly captured caches. A private UBO-off
  experiment recovered fewer graphics recipes, so disabling UBO is not a cache fix.
- Enable the existing watchdog on this RTX 3090. The wrapper records per-run flips and,
  after a sustained stall report, captures one bounded GDB thread-stack snapshot, then
  detaches. It authorizes only the wrapper and its debugger descendants with Linux
  `PR_SET_PTRACER`; no system ptrace setting changes. Normal gameplay runs without GDB.
  Existing resource capture is retained. Debugger absence/startup failure does not abort
  the game. A debugger timeout also leaves the child alive and untraced in the local test.

The current-cache preparation replays 818 shader permutations, resolves 940 graphics recipes
(929 already known, 11 additionally built), and prewarms 128 compute pipelines. It reports
201 unavailable shader permutations and 5,452 legacy graphics recipes that still cannot be
retargeted. Preserve these records and report the incomplete coverage; do not delete caches
or claim complete precompilation. Shader codegen identity is unchanged by this fix.

## Validation and limits

The new `upload_dma_preset_label_order` GPU test holds the DMA worker, queues an upload,
publishes a nonoverlapping label, then overwrites the source as the next frame would. It
fails with the previous shipped policy and passes with `labels`, checking the actual uploaded
GPU bytes. CTest obtains the policy from the shipped JSON rather than hardcoding the fix.

All 26 targeted rendering, upload, pipeline, controller/audio and watchdog tests passed.
Python capture/package tests, real debugger attach/detach and timeout tests, a whole-wrapper
capture test, and simulated diagnostic-start failure passed. Launcher startup is checked
with private settings. Evidence, original caches/binaries and protected-state hashes are in
`~/.local/state/kytyps5-int21-regression-20261025/`.

Neither complete level was replayed during this validation. The Trapped in Time freeze is
not claimed fixed: the automatic native capture is intended to resolve the missing evidence
if it recurs. Saves, patches, trophies, controller pairing, graphics settings and other agents'
worktrees must remain untouched during installation.

## Live-freeze root cause found after the first candidate

Run `20261025T030849.727482Z` on `88a597e4` reproduced repeated long freezes at
the stage entrance. Automatic capture completed successfully, and additional live
debugger observations established the command processor was sleeping in
`UploadDma::ClaimAndCopy`, in `FUTEX_WAIT_PRIVATE` for ticket state 1, while the
same word already contained completed state 2. The upload worker was idle.
Interrupting the wait with the debugger briefly restored progress before another
ticket stalled. The window/input loop remained responsive; no new NVIDIA Xid was
recorded for this run. The user subsequently closed the game.

The installed `gcc-libs 16.2.1+r23+gd564253eb6c8-1` has GCC libstdc++ bug
[PR127757](https://gcc.gnu.org/PR127757). Its notifying implementation can observe
zero waiters while the waiter still sees the previous publication value, because
it lacks the necessary store/load fence. The installed binary disassembly and its
matching source lack the fence. GCC fixed this upstream in
[a971455d8f2b](https://github.com/gcc-mirror/gcc/commit/a971455d8f2b672d87297226f68e3a4f2019ec28).

The emulator-side workaround preserves the release store and places a sequentially
consistent fence before notifying ticket waiters. It uses the same ordering remedy
as upstream, without updating system packages, changing shader codegen, discarding
caches or redesigning ticket ownership. Recorder bell increments already use atomic
read-modify-write operations; this change targets the observed plain completion store.

An independent host regression exercises one million publications with cache
contention, randomized timing and a bounded progress monitor. Its unfenced control
reproduced a stalled receiver after publication; the production fenced path passed.
A private run of GCC's original reproducer independently failed without the fence
and passed with it. The progress monitor uses a generous five-second threshold;
this remains a timing-dependent stress test, not an exhaustive concurrency proof.
Failure cleanup also changes the publication value before joining the receiver.

Live stacks, actual futex operands, completed ticket contents, syscall trace,
runtime versions and reproducer outputs are retained under
`~/.local/state/kytyps5-live-freeze-20261025/`. The earlier uncertainty about the
Trapped in Time freeze is superseded for this captured upload-completion failure;
other historical hangs are not automatically assigned the same cause.
