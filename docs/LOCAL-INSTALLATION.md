# Local KytyPS5 installation

The sole installed version is based on the user-validated Ported2 runtime,
including the RangeSet optimization from Jetsku PR #63 and the earlier Ported
performance fixes. The source is promoted directly to local `main`.

- Application-menu entry: **KytyPS5**
- Desktop shortcut: `~/Desktop/KytyPS5.desktop`
- Command: `kytyps5`
- Program directory: `~/.local/opt/kytyps5`
- Settings and pairing profile: `~/.config/kytyps5`
- Saves, patches, trophies and warm caches: their existing `_SaveData`,
  `_Patches`, `_Trophies` and `_PipelineCache` folders under the program directory
- Per-run logs and CPU/RAM/GPU capture: `_CrashCapture` under the program directory

The manual **Precompile shaders** launcher button and its dialog are removed.
Automatic shader/pipeline replay, performance settings, controller support and
streaming functionality are preserved. Shader codegen is unchanged, so the
latest Ported2 caches remain compatible.

Ported2's latest saves, settings, trophies, patches and pairings are migrated
to the generic paths. The former Streaming, Ported and Ported2 installation
directories, version-specific commands and menu entries are removed after
the new installation passes verification.

Archived user data and migration evidence are stored privately in
`~/.local/state/kytyps5-consolidate-20261020/`. These archives contain settings,
saves and diagnostic history, not additional installed emulator versions.
