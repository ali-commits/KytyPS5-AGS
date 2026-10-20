# KytyPS5 Ported2 — Jetsku PR evaluation, October 20

Ported2 starts from `0948e1272930abfe1bea90e7e23708a09af974b0`, the Ported
build the user confirmed played the first Astro Bot stage without issues.
It is a separate comparison installation on branch `ports/jetsku-ported2`.
The existing Ported, Streaming and main branch references are retained.

## Included change

[Jetsku PR #63](https://github.com/Jetsku/KytyPS5/pull/63), by
Majed-allababedi, head `c9b54080d42ca0eaddbbe5bab6714bbb53e67c1f`:
`RangeSet::Add` now keeps an existing map node when a touching or overlapping
addition extends its right endpoint without changing its start. It deletes
only successor intervals absorbed by that extension. This removes a node
allocation/free pair from each eligible merge in GPU memory range tracking.

The production change is the upstream eleven-line patch. The port also adds
its standalone allocation, interval semantics and randomized byte-model tests
to CTest and `kyty_tests`. The test allocator aborts on allocation failure when
building without C++ exceptions, as required by this Linux build.

The PR's unrelated INSERTQ expectation fixes are already present here.
Its Windows workflow changes are outside this Linux candidate.
Shader generation, rendering order, memory synchronization and presentation
timing are unchanged.

## Review of the other open PRs

| PR | Decision |
| --- | --- |
| [#65 — frame timing](https://github.com/Jetsku/KytyPS5/pull/65) | Deferred. Its Immediate-mode loop can complete multiple guest flips in one eligible virtual vblank. Streaming capture can discard the additional frames while guest work increases. Its future-only deadline policy can also skip a full period when work slightly exceeds the budget. A timer-only adaptation needs separate timing tests and a demonstrated pacing problem. |
| [#64 — Windows CI](https://github.com/Jetsku/KytyPS5/pull/64) | No effect on this Linux runtime. |
| [#58 — early CUDA initialization](https://github.com/Jetsku/KytyPS5/pull/58) | Windows `nvcuda.dll` / driver Smooth Motion support, not this Linux RTX 3090 path. |
| [#51 — Qt packaging](https://github.com/Jetsku/KytyPS5/pull/51) | Already present. |
| [#50 — INSERTQ tests](https://github.com/Jetsku/KytyPS5/pull/50) | Already present. |
| [#49 — NVIDIA bool-phi workaround](https://github.com/Jetsku/KytyPS5/pull/49) | Deferred. Its demonstrated fix targets Astro's Playroom with driver 580.178.04, while this machine uses 615.78.08 and the user's current game is Astro Bot. It changes shader output and invalidates the program cache; no matching crash has been reproduced here. Worth a separate compatibility experiment if that issue occurs. |
| [#45 — CJK fonts](https://github.com/Jetsku/KytyPS5/pull/45) | Deferred. Our overlay already has locale-specific font fallbacks. Guest `libFont` CJK/TTC support could still help missing in-game glyphs, but the PR is unbuilt/untested and has a known writing-layout limitation. |
| [#20 — non-DualSense haptics](https://github.com/Jetsku/KytyPS5/pull/20) | Already implemented, with local gain handling and device-switch cleanup. |

## Validation and limits

The new allocation tests fail against the old implementation and pass after
the port: right-growing merges allocate zero replacement nodes, while absorbed
successors are still released. Both versions pass the independent semantics
model: 60,000 randomized mutations and 240,000 queries.

The Linux Release native and test targets build successfully. All six selected
CTests pass: `range_set`, `memory_tracker`, `page_manager`, and the normal,
clamp-memo verification and clamp-memo disabled virtual-memory suites.

This proves an allocation reduction, not an increase in gameplay FPS. Keep
the same route and settings when comparing Ported with Ported2.

Build/test/install and bounded Sky Garden smoke evidence is recorded outside
the repository at
`~/.local/state/kytyps5-ported2-20261020/`.

## Separate installation

- App-menu entry: **KytyPS5 Ported2**
- Command: `kytyps5-ported2`
- Prefix: `~/.local/opt/kytyps5-ported2/20261020-jetsku`
- Settings/profile: `~/.config/kytyps5-ported2/20261020-jetsku`

Ported2 begins with independent copies of current Ported saves, patches,
trophies, settings, pairing profile and warm shader caches. It retains the
existing precompile button and per-run CPU/RAM/GPU capture. Runtime logs are
written to its own `_CrashCapture` directory. Shader codegen remains
`86b7a2d2f817d103`; no shader cache reset is required by this port.
