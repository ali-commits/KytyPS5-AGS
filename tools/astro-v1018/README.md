# Astro Bot v1.018 local configuration

This preserves the tested PPSA21567 v01.018.000 configuration for Jetsku U59
int19.1 (`28bc20f146a276479acf42d2e405fb46a4491b8e`). It does not contain a game,
saves, caches, downloads, or machine-specific paths. Configure those locally.

The profile selects 1920×1080 internal rendering, a 60 FPS cap, elapsed-time
simulation, its associated physics/rescue fixes, two supplied Havok guards,
real-time video, and intro skipping. Optional lighting, texture, and model-detail
reductions are disabled. R1+X toggles the timing patch. The 1/30-second timestep
limit deliberately slows simulation below 30 FPS to constrain physics steps.

The community patch source was supplied by the user in the UnrealGamingPC patch
bundle. `source-profile.json` preserves that source; its declared executable hash
matches the tested game. `prepare_profile.py` changes the selected options and
moves patch-private state from 0xf4e5440 (which overlaps this executable's import
strings) to an externally allocated page at 0x12004000, relocating its
RIP-relative references. It does not change the game's on-disk executable.

Generate with Python and Capstone 5.0.9, then run the tests:

```sh
uv run --with capstone==5.0.9 python tools/astro-v1018/prepare_profile.py
python3 -m unittest discover -s tools/astro-v1018 -p 'test_*.py'
python3 tools/astro-v1018/validate_profile.py --game /path/to/game
```

`validate_profile.py` pins both hashes, verifies original bytes and allowed cave
locations, and permits only the intended timestep-clamp overlap. Offline
validation proves placement, not full gameplay correctness. Retain save backups.

Build the Linux release with the fork's Linux workflow settings (the supplied
Windows PGO profile is not applicable):

```sh
cmake -S . -B _Build/local-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DKYTY_EMULATOR_IPO=ON \
  -DKYTY_BUILD_LAUNCHER=ON -DBUILD_TESTING=ON \
  -DKYTY_BUILD_ORIGIN=Fork -DKYTY_BUILD_REPOSITORY=Jetsku/KytyPS5 \
  -DKYTY_RELEASE_TAG=local-u59-int19.1 \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build _Build/local-release --target launcher kyty_emulator --parallel 6
cmake --install _Build/local-release --prefix "$PWD/_Build/local-release/install"
cp tools/u59-preset.json _Build/local-release/install/
```

Run the Linux release workflow's nine validation targets/tests before installing
as default. Stage the fallback Roboto fonts as described in
`.github/workflows/u59-linux.yml`.

`install_default.py --stage STAGE --prefix NEW_VERSIONED_PREFIX --game GAME
--runtime EXISTING_RUNTIME` installs for the current user. It reuses the tested
save/cache folders via symlinks, backs up replaced user launchers/settings, creates
`kytyps5` and `astro-bot` commands in `~/.local/bin`, and overrides the desktop entry.
The system package is retained; `/usr/bin/kytyps5` remains available for rollback.
A local INI avoids overwriting the system package's settings. Automatic update
checks are off for this local build.

For a direct test without installation, copy `config.example.json` to a local
file, fill in paths, then run `launch.py --config FILE --dry-run` followed by the
same command without `--dry-run`. A shared emulator entry point guards both GUI and direct game launches; help/version probes stay available. The direct launcher also enforces a single emulator instance,
records the settings and exit status, and preserves earlier logs. It keeps Intel
compatibility and the tested NVIDIA deferred-protection workaround enabled.
