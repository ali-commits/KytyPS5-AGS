# Astro Bot PPSA21567 v01.018 widescreen research (UNTESTED)

Status: RESEARCH ONLY. None of this has been applied or launched. Do not enable against a real
game without the morning experiment described in the custom-resolution spec (retired with the
AGS streaming split) section 5. Keep save backups.

The host already presents a correct 16:9 image on the 20:9 phone via the shipped `fit` aspect
mode. A native 20:9 render is a bonus that needs live validation.

## Offset model

Patch (loaded-image) offset -> eboot file offset: `file = patch + 0x3b070` (verified against the
resolution-load and decouple-cave byte patterns).

## Confirmed resolution-mode selector

Patch `0xc62a97` (file `0xc9db07`), original bytes `8b 83 f0 23 00 00`
(`mov eax, [rbx+0x23f0]`). The community cheats overwrite it with `b8 0X 00 00 00 90`
(`mov eax, imm32 ; nop`) to force a mode index. Known indices (all 16:9):

| index | size       |
|-------|------------|
| 0     | 1920x1080  |
| 1     | 2432x1368  |
| 3     | 3328x1872  |
| 4     | 3840x2160  |

The index is consumed by virtual calls at `0xc62845` (`call [rax+0x38]`) and `0xc62852`
(`call [rax+0x30]`), where `rax = *[rbx+0x28]` (an object vtable). Those functions turn the index
into the actual buffer width/height and issue the VideoOut/swap-chain size request. The concrete
dimensions are NOT a flat table in the data segment (a scan for the 16:9 width/height int pairs
found them only as scattered UI/config defaults, not a dense indexed array).

## Why no drop-in widescreen cheat yet

1. The per-index dimensions are computed inside the virtual "apply mode" functions, not read from
   an editable table, so forcing 2400x1080 means either editing those functions or redirecting the
   size request through a cave (as the FPS cheats do for timing).
2. The executable contains NO 20:9 aspect constant (`e4380e40` absent; 16:9 `398ee33f` and 21:9
   `55551540` present). A 16:9 render stretched to 20:9 without fixing the camera projection would
   distort the world. The projection aspect source must be found first.

## Proposed morning experiment (fill in the bytes after the live trace)

1. Resolve the vtable at `*[rbx+0x28]` and disassemble `+0x38` / `+0x30`. Record where each writes
   the render width/height (watch for a `sceVideoOutSetBufferAttribute2` call and the width/height
   it is given).
2. Decide the mechanism:
   - If the dimensions are per-index immediates/table entries: overwrite an UNUSED index's entry
     with 2400 x 1080 (leave 0/1/3/4 intact), then force that index at `0xc62a97`.
   - If they are computed from a base x scale: redirect via a cave that writes 2400 x 1080 into the
     buffer-attribute request, mirroring the FPS cave pattern (`0x12002400` style external page).
3. Locate the projection aspect: search near the camera-matrix build for a divide by the render
   width or a multiply by a 16:9 constant. If aspect = width/height, step 2 alone widens the FOV
   correctly. If aspect is a constant, add a paired cave that supplies `20/9 = 2.2222f`
   (`e4380e40`) to the projection.
4. Author the cheat(s) in the same JSON shape as `tools/astro-v1018/source-profile.json`, each
   `enabled:false` and documented with offset, original bytes (`off`), patched bytes (`on`) and
   reasoning. Run `tools/astro-v1018/validate_profile.py --game <path>` to pin the hashes and
   verify the original bytes before any launch.

## Template entries (bytes are PLACEHOLDERS -- do not apply)

```json
{
  "enabled": false,
  "memory": [
    { "off": "8b83f0230000", "offset": "0xc62a97", "on": "b8<II>000090",
      "_note": "force an unused resolution-mode index whose entry was repointed to 2400x1080" }
  ],
  "name": "[UNTESTED] Render 2400x1080 (20:9)"
},
{
  "enabled": false,
  "memory": [
    { "off": "<ORIG>", "offset": "0x<projection-aspect-site>", "on": "<inject 20:9 e4380e40>",
      "_note": "only if projection aspect is a constant, not width/height-derived" }
  ],
  "name": "[UNTESTED] Widescreen 20:9 projection"
}
```
