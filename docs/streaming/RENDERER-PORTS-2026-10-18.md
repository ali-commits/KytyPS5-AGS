# Renderer ports — 18 October 2026

This release adapts three upstream renderer fixes onto the native streaming fork, preserving the current launcher and its native Qt theme, icon/list views, Streaming tab, experimental audio controls and cheat handling.

| Upstream commit | Adaptation |
|---|---|
| `01ce973882b593fd13dbb225fd4a55dbda491669` | Metadata bookkeeping no longer suppresses arbitrary compute stores when a metadata address is reused as ordinary memory. |
| `4059179e9d51d907f02bde193dbd46d86489d739` | Proven uniform compute fills can update mapped guest buffers through the existing buffer cache; ambiguous image aliases and pending publications retain normal dispatch. |
| `fe3857a1715551e4f554b2ec29cc1ca3286287a1` | CMASK fast clears support multisampled color targets while retaining this fork's per-layer metadata and ownership tracking. Fast-clear elimination admits CMASK multisampling while preserving DCC restrictions; VRS metadata remains excluded. |

The fork uses an explicit conservative specialization-memory dependency marker rather than importing upstream's different SRT snapshot machinery. A shader with such a dependency keeps normal dispatch. This may decline an optimization; existing SRT replay and invalidation remain intact.

Baseline testing also exposed duplicate shutdown signals from the launch wrapper and its guardian. The guardian now broadcasts graceful shutdown once, preventing reentrant delayed capture cleanup. Process ownership and the existing forced-stop deadline remain enforced.

## Verification

Targeted regressions demonstrate fail-before/pass-after behavior for lost metadata stores, CPU-owned proven fills, MSAA CMASK discovery and fast-clear elimination. The new MSAA cases also pass Vulkan validation with the recorder off, queued and inline (zero errors or warnings). Three focused CMASK validation entries are registered in CTest for future runs.

Release verification additionally compiles all native/launcher/test targets, runs the full registered CTest suite and launcher preservation checks, and runs an isolated Astro Bot 01.018 HEVC streaming session using the current cheat selections. The installed package records complete logs and provenance under `verification/oct18-renderer`. The network smoke establishes transport and shutdown health; it does not establish controller feel or frame pacing on physical clients.

## Installation and comparison baseline

The updated dedicated command is `kytyps5-streaming`. Installation uses a fresh versioned prefix and private config, migrates paired clients and all launch/audio/cheat preferences, and independently copies saves. Previous packages and rollback wrappers are retained. Current enabled cheat selections (15 mods, 42 validated writes) are preserved; this release does not reset them to the earlier smoke-test preset.

The immediate pre-port build is `bfce869405a5494d80a9e19f715b4ecabaff10cb` at `~/.local/opt/kytyps5-streaming/20261018-upstream-ui`; its configuration, saves, patches and byte hashes were preserved at `~/.local/state/kytyps5-ab/before-three-ports-20261018`. Earlier native streaming packages are also retained for the requested original-versus-final performance/controller comparison, which follows completion of these ports.
