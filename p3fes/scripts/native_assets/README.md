# Native asset preparation

The extraction and model preparation tools are reused from [Virtual-AK/fes-pc-recomp](https://github.com/Virtual-AK/fes-pc-recomp)
at commit `4209a2a8e3e9ab9d85f37a6a3866df83434afd6c` under the MIT license in
`LICENSE.virtual-ak`.

They are intentionally kept as an offline preparation layer: Persona 3 FES assets stay on the
user's machine, while the native renderer can consume converted/decoded geometry and textures
without reimplementing the Atlus RenderWare formats in the runtime.

Included now:

- `p3fes_unpack.py`: ISO/CVM/container extraction helpers.
- `p3fes_rmd.py`: RMD/RWS RenderWare scene, model, skin and PS2-native texture decoding.
- `p3fes_gltf.py`: glTF export for meshes, skeletons, skin weights and animations.
- `p3fes_tmx.py`: Atlus TMX texture decoding.
- `prepare_model.py`: extract one RMD directly from an existing CVM and prepare it as glTF
  without unpacking the whole archive.
- `prepare_native_model.py`: extract one RMD directly from a CVM and write one compact
  `.p3n` runtime package containing simple mesh/skin/material data plus decoded RGBA
  textures. This follows the same broad strategy as RECV Vita: console-specific asset
  decoding happens on the user's PC/Mac, not on the handheld.
- `prepare_movies.py`: list SFD movies in a CVM or convert selected movies to H.264 MP4
  for native playback. It extracts only the chosen SFD to a temporary file and records
  output metadata and a SHA-256 hash in `manifest.json`.
- `prepare_bgm.py`: list ADX music in a CVM or prepare selected tracks as native
  streaming PCM WAV files, preserving their loop points.

### Native movie preparation

The movie converter defaults to listing only; it does not transcode the archive unless
you request a movie or `--all`. Run from the repository root:

```sh
python3 p3fes/scripts/native_assets/prepare_movies.py game-mac/DATA.CVM --list
python3 p3fes/scripts/native_assets/prepare_movies.py game-mac/DATA.CVM \
  --movie /SFD/P3CTOP2.SFD --movie /SFD/P3CTOP.SFD --out game-mac/native_movies
```

Repeat `--movie PATH` to prepare specific files. `--all` converts every SFD and should
only be used when that storage and conversion time are wanted. The output keeps the
source stem exactly (`P3CTOP.mp4`) so a case-insensitive runtime hook can map the
original `.SFD` path to the converted movie. Video is H.264 Baseline, level 3.1,
`yuv420p`, capped at 640×448 without upscaling and kept at its source frame rate. A
source movie with audio requires ffmpeg support for ADX decoding and AAC encoding; if
either is unavailable, conversion fails and does not publish a silent video. `ffprobe`
must be available alongside ffmpeg or on `PATH`. The converter extracts and hashes the
source SFD, then reuses an output only when its source hash and recipe version match,
its output SHA-256 matches the manifest, and ffprobe validates its stream metadata.
Fresh output is written to a temporary MP4 in the destination directory and atomically
renamed into place only after ffprobe validation. `manifest.json` records the original CVM path, output stem, duration, frame
rate, output dimensions, and output SHA-256. Its `source` and `output` sections preserve
ffprobe container, video codec/profile/pixel format/level/dimensions/rates, and audio
codec/profile/sample rate/channel layout for auditing the conversion.

The converter reads the user's CVM and writes only to the selected output directory.
The generated MP4s and manifest are derived from game data; do not commit, publish,
or distribute them.

The Vita runtime maps the movie requested by the original Sofdec task to
`ux0:data/p3fes/native_movies/<UPPERCASE_STEM>.mp4`. The opening requested first
by the US executable is movie 35, `P3CTOP2.SFD`; `P3CTOP.SFD` is a separate clip.
The native route uses SceAvPlayer video frames directly as GXM YUV textures and
outputs its decoded PCM through SceAudioOut. The original task waits for playback
to finish; Start, Cross or Circle skips it. An absent or failed native movie uses
the existing guest decoder. Set `P3FES_NATIVE_MOVIES=0` to disable native playback.

Vita launchers can read runtime switches from `ux0:data/p3fes/runtime.cfg`:

```ini
PS2X_LOG_FILE=ux0:data/p3fes/boot-menu.log
PS2X_PERF_DIAG=frames
PS2X_PERF_FRAME_PC=0x001983A0
P3FES_NATIVE_MOVIES=1
```

For visual checks with the local Vita3K build, `P3FES_VITA3K_CAPTURE=1` requests
screenshots of selected native UI frames. Disable captures and verbose RW
diagnostics when measuring speed. `PS2X_PERF_DIAG=1` instruments individual
runtime operations and adds substantial overhead under emulation; use `frames`
for the boot speed check. The acceptance sequence is memory-card screen, logos,
opening movie and an interactive menu. Check actual game/VBlank progression,
movie duration and audio/video synchronisation: host loop or window FPS alone
does not establish full speed.

`P3FES_RESOURCE_DIAG=1` reports native presents, uploads, live textures and retired
resources every five seconds. Ordinary RenderWare textures are identified by a
resource generation and mutation revision. Loading or unlocking changes the
revision; drawing reuses the prepared GPU resource without hashing or scanning its
pixels. Submitted frames retain their texture definitions until rendering has
finished, including when the game destroys or reuses a raster in the meantime.

For short renderer iterations only, `P3FES_MOVIE_TEST_SKIP_SECONDS=1` ends a movie
after its first second. Remove that switch for the acceptance run so the entire
opening, its audio and the handoff to the title are exercised. Optional
`PS2X_DMA_ORIGIN_DIAG=1` and `P3FES_GS_UPLOAD_DIAG=1` trace remaining legacy
transfers; disable both when measuring performance.

#### Format compatibility checks

The original opening `P3CTOP2.SFD` contains MPEG-1 video at 640×448 and
30000/1001 fps with stereo ADX at 48 kHz. `P3CTOP.SFD` uses MPEG-2 video;
`GAME_START.SFD` uses MPEG-1. All three use ADX audio. These codecs differ from
the H.264/MPEG-4 and AAC combinations in Sony's
[published Vita video formats](https://manuals.playstation.net/document/gb/psvita/videos/filetypes.html).
Remuxing those SFDs alone does not make their video/audio codecs compatible.
Their transcode is performed offline, preserving source resolution and frame
rate at these sizes. The manifest records probed source and output codecs.

The decoded native movie stays in AVPlayer's YUV format and is sampled directly
by GXM. It does not pass through the PS2 IPU/GS or a CPU YUV-to-RGBA conversion.
For ordinary textures, GXM supports P4/P8 palettes, but PS2 RenderWare's index
layout, PAL8 palette permutation and GS alpha range need adaptation. The
current renderer accepts RGBA, so its palette expansion remains an optimization
opportunity; GXM support for palettes does not make the PS2 byte layout directly
interchangeable with GXM's layout.

### Native music preparation

Prepare only the selected tracks from the user's `BGM.CVM`:

```sh
python3 p3fes/scripts/native_assets/prepare_bgm.py game-mac/BGM.CVM --list
python3 p3fes/scripts/native_assets/prepare_bgm.py game-mac/BGM.CVM \
  --track 115.ADX --track 54.ADX --out game-mac/native_bgm
```

For the first US New Game story probe, prepare its two opening clips and the
name-entry/first-field music as well:

```sh
python3 p3fes/scripts/native_assets/prepare_movies.py game-mac/DATA.CVM \
  --movie P3CT001_01.SFD --movie P3CT001_02.SFD --out game-mac/native_movies
python3 p3fes/scripts/native_assets/prepare_bgm.py game-mac/BGM.CVM \
  --track 79.ADX --track 44.ADX --out game-mac/native_bgm
```

These are the assets observed after choosing The Journey / New Game. They extend
the prepared resources beyond the menu gate; their presence does not prove that
story rendering or physical-Vita performance passes validation.

The tested US title requests BGM 115 (`115.ADX`), then BGM 54 (`54.ADX`) for the
interactive menu. The converter decodes ADX to PCM16 stereo at 48 kHz for SceAudioOut. It trims ADX
block padding to the source sample count and preserves version 3/4 ADX loop
points in the WAV `smpl` chunk, rescaling sample positions when needed. Source
ADX loop end positions are exclusive; WAV loop end positions are inclusive.
The header interpretation follows [vgmstream's ADX reader](https://github.com/vgmstream/vgmstream/blob/master/src/meta/adx.c).
Outputs are validated and published by atomic rename; the manifest permits
reusing unchanged prepared files without transcoding them again.

The runtime resolves the game's BGM ID to its original filename, then reads
`ux0:data/p3fes/native_bgm/<UPPERCASE_STEM>.wav` in small blocks. It keeps the
game's BGM queries, pause and volume operations, and leaves other audio handles
on their original path. Missing or unsupported WAV files use the original audio
path. Set `P3FES_NATIVE_BGM=0` for an A/B check. Prepared WAVs and manifests are
derived game assets and must not be committed or distributed.

The boot/menu run exercises play, direct stop and track replacement. Loop wrapping
is covered separately by the synthetic WAV streaming test.
`StopBgmFade` currently interprets its signed 16-bit duration as 60 Hz update ticks;
that timing assumption still needs verification outside this validation sequence.

For a quick native-render sanity check, build the desktop target `p3fes_native_viewer`,
prepare one model with `prepare_model.py DATA.CVM /PATH/MODEL.RMD -o OUT`, and pass
the generated `.gltf` to the viewer. The viewer is deliberately separate from the
emulated GS so it can be used to compare a native asset path against the existing
correctness fallback.

For direct RenderWare compatibility checks, configure with `-DP3FES_BUILD_RW_PROBE=ON`
and build `p3fes_rw_probe`. The probe fetches [aap/librw](https://github.com/aap/librw)
at commit `18532c2e13efbc1aa43f0b00b4459831f9414de0` (MIT), uses its parser-only `NULL`
platform, and converts the geometry/material/skin data to a small GPU-neutral mesh
representation. The conversion follows the approach used by
[Coney](https://github.com/Ryubara/coney) at commit
`eb07fc0f9d69e62c7aeee6307be5c2ef162f4ee6` (GPL-3.0-or-later). PS2 texture decoding
stays on the existing offline Virtual-AK path for now; the probe deliberately does not
load the RMD texture dictionary. It is diagnostic tooling, not a runtime dependency.
The same option also builds `p3fes_rw_viewer`. Its RMD path now talks to the small
platform-neutral interface in `p3fes/native_renderer/` (inspired by Celosia's
`native_renderer` boundary), rather than to raylib directly. The current desktop backend
uses raylib/OpenGL; a Vita backend can implement the same upload/draw contract on vitaGL.
The viewer therefore renders an RMD without the emulated GS or an intermediate glTF file.
It is intentionally untextured while the PS2 raster path remains separate.

`p3fes_rw_viewer` also accepts a `.p3n` package. That path does not parse RenderWare at
runtime: `p3fes/native_renderer/native_model.*` validates and loads the already-prepared
vertices, indices, submeshes, skin weights and RGBA textures. The texture payload is
uploaded once by the native renderer and bound per RenderWare material/submesh by its
logical texture name. Direct `.RMD` viewing remains intentionally texture-free because
the target-oriented path is the preprocessed `.p3n` package.

`.p3n` files are derived from the user's game data. Do not commit, publish, or distribute
them, extracted game assets, or converted output.
