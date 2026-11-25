<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="p3fes/branding/logo-dark.png">
  <img src="p3fes/branding/logo-light.png" alt="Persona 3 FES Recompiled" width="85%">
</picture>

**An attempt at a static recompilation of *Shin Megami Tensei: Persona 3 FES* (NTSC-U `SLUS-21621`)**<br>
The game's MIPS R5900 code is statically recompiled to C++ and run on a reimplementation of the PS2 hardware. No emulator is involved.

![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-1b8cff)
![C++20](https://img.shields.io/badge/C%2B%2B-20-1b8cff?logo=cplusplus&logoColor=white)
![Based on](https://img.shields.io/badge/based%20on-PS2Recomp-17a35b)
![Renderer](https://img.shields.io/badge/renderer-PCSX2%20GS-17a35b)
![Status](https://img.shields.io/badge/status-experimental-d01f3c)

</div>

> [!IMPORTANT]
> This repository contains no game files: no ELF, IRX modules, `.CVM` archives or code generated from them.
> The PS Vita beta VPK on the [Releases](https://github.com/frangarcj/p3fes-recompiled/releases) page holds the
> recompiled executable code (a Vita app is one static binary), but no game data.
> You need **your own copy** of Persona 3 FES, NTSC-U, PS2 ID **`SLUS-21621`**, matching the
> [Redump record](http://redump.org/disc/4125/). The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_216.21` | `3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb` |

## Status

<img src="p3fes/status/progress.svg" alt="Status map: the executable's functions by module and the PS2 hardware, coloured by state, and the road to a playable port" width="100%">

<sub>Generated from <a href="p3fes/status/status.toml"><code>p3fes/status/status.toml</code></a> by <code>p3fes/scripts/status_chart.py</code>. 🟩 working · 🟦 partial · 🟥 pending.</sub>

**Where it stands:** the game boots from its own CVM archives, plays the opening movie, and goes through New Game (the
FES introduction, the opening anime, name entry) into the story: the dorm, school, day changes and Tartarus, loaded
from a memory card save. 3D runs at the console's 30 fps with VU1 on PCSX2's microVU recompiler; menus run at 60.
Music and voices play. Sound effects are still silent. A macOS test reached a companion's command menu and
confirmed an action in the Aigis tutorial encounter.

| Milestone | |
|---|:---:|
| Recompile all 14,640 functions of `SLUS_216.21` | ✅ |
| Boot: EE kernel, original IRX modules, CVM archives | ✅ |
| Memory card (8 MB `.ps2`, PCSX2 format) and controller | ✅ |
| Movies: video (IPU) and audio (ADX over SPU2 AutoDMA) | ✅ |
| PCSX2 GS renderer (OpenGL / software) | ✅ |
| Title screen and main menu (*The Journey* / *The Answer*) | ✅ |
| New Game: introduction, opening anime, name entry | ✅ |
| Story scenes, free movement, day changes | ✅ |
| VU1 on PCSX2's microVU recompiler | ✅ |
| 3D at 30 fps, menus at 60 (console frame pacing) | ✅ |
| Tartarus | 🔧 |
| Sound effects (SPU2 voices) | 🔧 |
| Battles | 🔧 |
| Widescreen, upscaling, 60 FPS | ⏳ |

## Gameplay options

Open **System → Config** from the command menu, or **Config** on the title screen after choosing a story.
Press **R1** (**E** on a keyboard) to turn to the gameplay page. Use Up/Down to select a row and Left/Right
to change it. Select **Confirm** and press Cross to save all pending settings. Circle cancels the edits.
Press **L1** (**Q**) to return to the original page.

| Option | Default | In play |
|---|---|---|
| Direct party control | Off | Give commands to companions from the next battle. Rush and forced actions still use the game's AI. |
| Scene fast forward | Off | Hold Triangle (**V** on a keyboard) during a scripted scene to run the dialogue, actors and camera at up to 4× with animated VHS tracking. Release to return to normal speed. Choices wait at normal speed. Native Vita movies use the video player’s 4× playback. |
| Record dialogue history | On | Press **Select** (**Tab** on a keyboard) during dialogue to open or close the history over the scene. Use **Up/Down** to browse. The history keeps the last 128 dialogue pages of the session. |

The history records displayed text and speaker names from the US release. Some special glyphs appear as question marks.
The original PS2 [Direct Commands mod](https://gamebanana.com/mods/426860) documents the battle mechanics used for party control.

## Goal

A **fully playable** Persona 3 FES, covering *The Journey* and *The Answer* from the opening movie to the credits,
as a native executable. Accuracy comes first, with PCSX2 as the reference. Once the game is playable, the port hopes
to add widescreen, higher internal resolution and 60 FPS with frame interpolation.

## How it works

```
SLUS_216.21 ──► function map (14,640) ──► ps2_recomp ──► C++ (one file per function)
                                                               │
   ps2xRuntime  EE kernel HLE · DMAC · GIF/GS · VIF1/VU1 · IPU ─┼─► native executable
   ps2xIOP      R3000A interpreter running the game's own IRX · SPU2 · SIO2
   pcsx2gs      PCSX2's GS (OpenGL / software) behind a small C API
```

<details>
<summary><b>EE: static recompilation</b></summary>

`ps2xRecomp` decodes every function in the map and emits C++ against an `R5900Context` (128-bit GPRs, HI/LO pairs,
FPU and VU0 state). Delay slots, 64-bit compares and MMI lane semantics are translated explicitly. Direct calls are
C++ calls. Indirect jumps go through a dispatcher that maps a guest PC to a compiled function or a mid-function entry.

| | |
|---|---|
| Functions recompiled | 14,435 |
| Bound to runtime HLE | 205 (EE kernel, SIF, CD, pad, memory card libraries) |
| Kept as guest code | Sony `libmpeg` / `libipu`, so the game's MPEG player runs on the emulated IPU |
| Indirect-jump fallback entries | 3,910 functions (453,264 entries) |
| Recovered jump tables | 14 |

</details>

<details>
<summary><b>Runtime and hardware</b></summary>

- **EE kernel (HLE):** threads, semaphores, event flags, alarms, interrupt handlers and VBlank/HBlank timing.
- **DMAC:** all channels, including scratchpad chain and interleave modes, which Sofdec's motion compensation uses.
- **IPU:** the MPEG-1/2 decoder adapted from PCSX2.
- **GS:** PCSX2's GS (`third_party/pcsx2-gs`, OpenGL or software renderer), fed through an observer on the GIF stream;
  the original CPU rasterizer and OpenGL backend remain as fallbacks (`PS2X_GS_RENDERER=builtin`).
- **VIF1/VU1:** VU1 microcode runs on PCSX2's microVU recompiler (`third_party/kzvu`); `PS2X_VU1=interp` selects the
  built-in interpreter.
- **IOP:** the game's original IRX modules (`SIO2MAN`, `PADMAN`, `MC2_S1`, `LIBSD`, `CRI_ADXI`, `SKSOUND`, …) run on an
  R3000A interpreter over an HLE IOP kernel. SPU2 handles voices and ADMA streaming, and SIO2 handles PCSX2-format
  memory cards.

| Middleware | Role | State |
|---|---|:---:|
| CRI Sofdec + Sony `libmpeg` | FMV (`.SFD`, MPEG-1 640×448) through the IPU | ✅ |
| CRI ADX (`CRI_ADXI.IRX`) | Music and movie audio over SPU2 AutoDMA | ✅ |
| CRI ROFS / CVM | Disc archives | ✅ |
| RenderWare 3.x | 2D/3D engine | 🔧 |
| ATLUS sound (`SKSOUND`/`MODHSYN`) | Effects and sequenced audio | 🔧 |

</details>

<details>
<summary><b>Repository layout</b></summary>

```
p3fes/                 Persona 3 FES config: p3fes.toml, function map, analyzer output, run.bat, scripts, status
ps2xRecomp/            Recompiler: R5900 decoder (rabbitizer) and C++ emitter
ps2xAnalyzer/          ELF analyzer that proposes stubs and entry points
ps2xRuntime/           EE runtime: kernel, DMAC, GS, VIF/VU, IPU, input, host window
ps2xIOP/               IOP: R3000A interpreter, HLE kernel and libraries, SPU2, SIO2
third_party/pcsx2/     Subset of PCSX2 at 2954ebe (GS, common, libraries)
third_party/pcsx2-gs/  PCSX2's GS as a shared library with a C API
third_party/kzvu/      PCSX2's microVU (VU1 recompiler) as a library, from KillzoneRecomp
third_party/pcsx2-vu/  Subset of PCSX2 at 646df006a used by kzvu
```

</details>

## PS Vita beta

1. You need a PS Vita or PS TV with HENkaku/Ensō and taiHEN, and about 6 GB free on `ux0:`.
2. Install the VPK from the latest [prerelease](https://github.com/frangarcj/p3fes-recompiled/releases) with VitaShell.
3. Open the **[data tool](https://frangarcj.github.io/p3fes-recompiled/)**, pick your `SLUS-21621` ISO and save the zip.
   The zip is built in your browser and nothing is uploaded. It holds the game files, the music converted to WAV and
   (optionally, ~10-15 min) the movies converted to H.264 for the Vita's decoder.
4. Extract the zip at the root of `ux0:` (it creates `ux0:data/p3fes/`) and start the game.

Updates only need the new VPK; saves (`ux0:data/p3fes/memcards/`) are kept. For bug reports attach
`ux0:data/p3fes/runtime.log`.

Releases are built by [`release-vita.yml`](.github/workflows/release-vita.yml) with
[`build_vita_release.sh`](p3fes/scripts/release/build_vita_release.sh): it recompiles `SLUS_216.21` (kept in a private
repository, SHA-1 checked, never cached or uploaded), builds the pinned VitaSDK, vitaShaRK and vitaGL (upstream plus
[these patches](p3fes/scripts/release/vitagl-patches/)), and publishes
only the VPK, `SHA256SUMS` and the notes. Start a release with *Run workflow* on `main` and a tag such as
`v0.1.0-beta.1` (that keeps the build caches; a pushed tag rebuilds from scratch). The data tool lives in
[`web/`](web/) and is deployed to GitHub Pages by [`pages.yml`](.github/workflows/pages.yml).

## Building

See [`p3fes/README.md`](p3fes/README.md). In short:

1. Build `ps2xRecomp`, copy `SLUS_216.21` from your `SLUS-21621` disc (check its SHA-1 above) next to `p3fes/p3fes.toml` and run `ps2_recomp p3fes.toml`.
2. Copy the generated C++ into `ps2xRuntime/src/runner/` and its two headers into `ps2xRuntime/include/`.
3. Build `ps2xRuntime` (Windows: llvm-mingw or MSVC).
4. Put the executable, `run.bat`, `SLUS_216.21`, `SYSTEM.CNF`, `LIB31\` and the `.CVM` files in one folder.

### PS Vita cross-build

With VitaSDK installed (the default toolchain path is `/usr/local/vitasdk` on the development setup used here):

```sh
VITASDK=/usr/local/vitasdk cmake -S . -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/usr/local/vitasdk/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DPS2X_ENABLE_FFMPEG=OFF
VITASDK=/usr/local/vitasdk cmake --build build-vita --target ps2xRuntime/ps2EntryRunner.vpk
```

Fresh Vita builds use the title id `P3FES0001` and expect the user's game files under
`ux0:data/p3fes/`, with `SLUS_216.21` as the boot ELF. Keep `DATA.CVM`, `BTL.CVM`, `BGM.CVM`,
`SYSTEM.CNF` and `LIB31/` there as well. These files are not packaged into the VPK and must not
be committed or redistributed.

The Vita target is a native engine backend: recompiled game logic feeds native
RenderWare resources and Vita graphics/audio/video services. Assets should be
prepared at load time, updated through explicit resource lifecycle operations,
and reused while drawing. Per-frame raster hashes, memory scans and emulated
GS texture uploads are transitional work to remove from that path.

The immediate validation target is memory-card screen, logos, opening movie and
interactive menu at full speed in the local `../vita3k` build. Hardware validation
comes after this gate. Local Vita3K runs on 2026-11-12 and 2026-11-13 completed the
FES opening (about 89.5 seconds, 2,671 video frames) and interactive menu. The newer
build also completed the original P3 opening (93.79 seconds, 2,801 frames).
Earlier short title/menu windows reached approximately 30 native presents/s, but
this does not establish the full menu speed gate. A later repeatable Load Game
probe (88 textures, 72 draws, no uploads or 3D meshes) measured 20.38 presents/s
for the validated build, 21.60 for the subsequent build and 21.80 after moving
branch diagnostics out of the dispatch hot path (12 five-second windows each).
Host thread priorities differed between sequential runs, so these numbers do not
establish an isolated optimization gain. The latest run completed the opening and
reached the same interactive menu; full speed remains unverified.
The opening uses SceAvPlayer and the title/menu
music uses streaming PCM through SceAudioOut. Ordinary textures use native resources
with explicit creation, mutation and destruction events, without per-frame content
fingerprints; the measured boot/menu path has no GS image uploads. Memory-card
background colour matches the GS reference, and consecutive static Atlus captures
are identical. Physical Vita, later story scenes and battles remain unverified for this
native backend. Keep the GS path as a separate visual reference when replacing
interfaces. See [native asset preparation](p3fes/scripts/native_assets/README.md).
The latest IOP idle-path build completed both openings again, preserved the
memory-card reference colours and produced six pixel-identical static Atlus
captures. Its earlier Load Game trial measured 28.55 native presents/s across
11 windows; later repeats above were slower. These historical measurements were
not controlled for the subsequently discovered host-priority changes.

A follow-up New Game probe on 2026-11-13 reached name entry and the first story
scene. The lobby environment and dialogue portraits are visible after preserving
camera transitions in draw order; the original frame camera was captured before
CameraBeginUpdate and remained null. The three textured, animated characters also
render after honoring the geometry prelight flag and rejecting null optional
attribute buffers: reading absent vertex colours from RAM address zero had made
their alpha zero. This scene runs at about 18 native presents/s, so it does not
pass the story speed gate. Further scene accuracy still needs comparison.
The probe also exposed native RenderWare entries that returned a value without
returning the guest PC to `$ra`: a scheduler checkpoint could repeatedly dispatch
the same draw until its command list exhausted memory. Those native returns are
now explicit; `ps2xRuntime/tests/native_renderware_return_test.cpp` covers both
direct entry and nested dispatch. Matrix/skin helper calls use an isolated guest
context and resume interior checkpoints to completion before consuming their results;
a regression test covers repeated checkpoints and preservation of caller state.
Native compressed-key interpolation and matrix multiplication have 129 and 3,072
differential cases against the recompiled routines, respectively; multiplication
also covers safe normal values beyond its fast-path range and exceptional fallbacks.
The general affine inverse has 512 comparisons against its actual recompiled callee.
A follow-up lobby probe measured about 19 native presents/s with animated characters;
this still fails the story speed gate. In that probe, interpolation and 35,046 skin
palettes used native math with zero guest fallbacks. All 32 palette comparisons,
spread across animation and scene placement, matched the original affine components
with zero error. Other scenes and inverse/hierarchy modes still require validation.
`P3FES_NATIVE_RW_MATH=0` selects the original animation/matrix/palette math for comparison.
`P3FES_GPU_SKIN=1` enables an experimental VitaGL Cg skinning path (default off).
It uploads bind-pose vertices, weights and bone indices once, and sends compact
palettes of up to 24 bones per draw batch. Shader limits, compilation, linking
and required uniforms are checked before enabling it. Unsupported dual/environment
passes, flat shading, oversized light sets and missing palettes retain CPU rendering.
With `P3FES_RESOURCE_DIAG=1`, `[native-skin]` reports GPU draws and CPU fallback reasons.
Host tests validate palette remapping, triangle order, weight handling and the
65,536-vertex boundary; they do not validate the Cg program or GPU rendering.
Vita3K compiled and linked the Cg program and drew the first lobby's characters
with zero skinning fallbacks. Visual inspection is not a pixel-level parity test.
One sequential same-SELF trial measured 16.85 GPU vs 17.40 CPU presents/s (12 windows
per arm, overlapping ranges); it demonstrates no speed gain. The option stays off.
Other materials, fog/alpha cases and physical Vita still require validation.
An IOP cached-idle fast path improved one matched Vita3K lobby A/B from 19.32 to
20.29 native presents/s (12 five-second windows per run, same executable and camera).
The 32 palette comparisons still matched with zero error. This scene remains below
30 fps; host priorities were not controlled in this historical comparison, and
it does not establish a portable speed gain or performance in other scenes.
A bounded graphics-frame diagnostic is available with
`-DPS2X_P3FES_FRAME_TRACE=ON` and `P3FES_FRAME_TRACE=1` in `runtime.cfg`.
It records one native frame after `P3FES_FRAME_TRACE_AFTER_SEC` (default 145),
matching `P3FES_FRAME_TRACE_DRAWS` commands (default 72, menu capture with no meshes).
The JSON at `ux0:data/p3fes/frame-trace.json` includes guest graphics routes,
command origins, textures, vertices, alpha/depth state and actual 2D GL pass counts.
The local Vita3K screenshot marker captures that frame. Timings are intrusive wall
measurements of the selected construction/presentation window, not GPU time or the
whole game update. `p3fes/scripts/frame_trace_report.py` resolves guest PCs using
the symbol map. The diagnostic is compiled out by default; reuse one JSON and PNG.

Projection tests check the near/far clip planes
for perspective and orthographic cameras. Mesh uploads compact vertices by material,
and unchanged static vertex data is reused without content hashes. Rendering and
speed still need validation beyond the boot/menu gate.

The physical-Vita package expects a taiHEN-enabled homebrew environment: the linked
vitaGL build imports taiHEN APIs. For a hardware measurement, set
`PS2X_LOG_FILE=ux0:data/p3fes/hardware-boot.log` and `P3FES_RESOURCE_DIAG=1` in
`runtime.cfg`, with no movie-test skip or controller script. Transfer the original
game data and prepared movies/music separately from the VPK, preserving existing
memory cards and saves. Memory diagnostics explicitly label invalid pool statistics
and unavailable vitaGL extensions rather than treating them as measurements.

<details>
<summary><b>Runtime options</b></summary>

| Variable | Effect |
|---|---|
| `PS2X_CALLBACK_STACK_TOP=0x100000` | Required: keeps interrupt/callback stacks clear of the main stack |
| `PS2X_GS_RENDERER=pcsx2` (default) / `pcsx2-sw` / `builtin` | PCSX2's GS with the OpenGL or software renderer (`pcsx2gs.so`/`.dll` next to the executable), or the built-in rasterizer |
| `PS2X_PCSX2_SCALE=2` | PCSX2 internal resolution multiplier |
| `PS2X_PCSX2_OPTIONS=` | PCSX2 image options, `key=value;…` (see `third_party/pcsx2-gs/include/pcsx2gs_api.h`) |
| `PS2X_VU1=interp` | Built-in VU1 interpreter instead of PCSX2's microVU recompiler |
| `PS2X_EE_CYCLE_SCALE=4` | Multiplier on the EE time charged by the recompiled code (1 = unscaled; 4 gives the console's 30 fps in 3D) |
| `PS2X_SHOW_SPEED=0` | Hide the game FPS / VPS readout in the window title |
| `PS2X_FRAME_CAP_FIELDS=1` | Let the game logic run at 60 updates per second (default 2: 30, like the console) |
| `PS2X_AUDIO_STRETCH=0` | Play the sound unstretched when the game runs below full speed (it runs dry instead) |
| `PS2X_GS_GPU=1` | Built-in OpenGL GS backend instead of the CPU rasterizer |
| `PS2X_GS_HW=1` | Experimental Celosia hardware GS (shared OpenGL context on macOS, vitaGL on Vita); CPU GS remains the default fallback |
| `PS2X_GS_HW_DIRECT=1` | With the experimental Celosia GS, present its shared GPU texture directly on macOS/Vita instead of readback/upload |
| `PS2X_GS_THREAD=1` | Run the GS backend on its own thread |
| `PS2X_GS_RASTER_THREADS=1..4` | Experimental: split large independent P3 PSMT4 triangle rows across CPU workers (off by default) |
| `PS2X_MEMCARD1=<path>` / `none` | Memory card for slot 1 (default `memcards/Mcd001.ps2`) |
| `PS2X_PAD_DEADZONE=0.15` | Analog stick deadzone |
| `PS2X_HIDE_DEBUG_UI=1` | Hide the developer overlay |
| `PS2X_AUDIO=0` | Disable host audio |
| `PS2X_AUDIO_DUMP=<file>` | Dump the SPU2 output (s16le stereo 48 kHz) |
| `PS2X_IOP_IDLE_FAST=0` | Disable the default cached-idle fast path for A/B checks; it advances only when the kernel cache and event/SPU clocks show no work through the target |
| `PS2X_PROFILE=<file>` | Linux sampling profiler; summarise with `p3fes/scripts/prof_report.py` |

Debug traces: `PS2X_SPU2_DIAG`, `PS2X_GS_DIAG`, `PS2X_IPU_TRACE`, `PS2X_DMA_LOG`, `PS2X_CD_LOG`, `PS2X_IOP_CD_LOG`,
`PS2X_RPC_LOG`, `PS2X_IOP_RPC_COUNTS`, `PS2X_SIFDMA_LOG`, `PS2X_IOP_MEMLOG`, `PS2X_IOP_TRACE`, `PS2X_EE_CYCLE_STATS`, `PS2X_GIF_TRACE`, `PS2X_PCSX2_SHOTS`,
`PS2X_PAD_SCRIPT` (with `PS2X_PAD_SCRIPT_CLOCK=fields`), `PS2X_EE_TRACE_CALLS`, `PS2X_LOG_SUSPEND`.
`PS2X_BRANCH_RING=1` enables the last-256-branches diagnostic history; recording
is off by default to avoid an atomic increment on every guest control transfer.

</details>

## Credits

- Logo lettering: [Faktos](https://maylett.net/cmd/faktos.html) and [BM Space](https://www.dafont.com/bm-space.font) (free font).
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the recompiler and runtime this port is built on.
- [god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp) by KIexster: SPU2, SIO2, IOP and VU1 work and many
  EE/GS fixes, merged here, plus the status map design.
- Taylor N. Albarnaz / LightVelox's Shadow of the Colossus fork of PS2Recomp: the OpenGL GS backend (via god-of-war-recomp).
- [Celosia](https://github.com/HenryKun55/celosia) by HenryKun55: the Vita SDL2/vitaGL host shim and
  reference architecture for sharing a PS2Recomp runtime between Apple Silicon and PS Vita.
- [librw-vita](https://github.com/Rinnegatamante/librw-vita) by aap/Rinnegatamante contributors: the MIT-licensed
  RenderWare-to-OpenGL/Vita camera and rendering reference used by the progressive native path.
- [black-recomp](https://github.com/SirBraga/black-recomp) by SirBraga: the conservative row-parallel CPU GS
  raster-worker pattern reused for large independent Persona 3 post-process triangles.
- [urban-reign-recomp](https://github.com/thiagordsfx/urban-reign-recomp) by thiagordsfx: the PCSX2 GS bridge design.
- [PCSX2](https://github.com/PCSX2/pcsx2): the IPU, the GS and microVU (VU1 recompiler), and the accuracy reference.
- [KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp) by drakolordx7: kzvu, PCSX2's microVU packaged for
  PS2Recomp (`third_party/kzvu`, ported here to clang and MinGW), and the IOP heap reuse fix.
- [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp): SPU2, IPU and GPU GS reference.
- [ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp) by Samir Nuri and exoteric33: the sound
  time-stretcher (`ps2xRuntime/src/lib/audio/stretch.*`, MIT) that keeps audio smooth below full speed.
- The [snaku Persona 3 FES decompilation](https://github.com/snaku/Persona3-FES-Decompilation), for function names.
- The [Persona 4 decompilation](https://github.com/Raikaru/Persona4-Decompilation) by Raikaru: names for shared
  engine, RenderWare and CRI functions, transferred by `p3fes/scripts/p4_names.py` (`p3fes/names/p4_names.csv`).

*Persona 3 FES* is © ATLUS / SEGA. This is an unofficial, non-commercial fan project, not affiliated with or endorsed
by ATLUS or SEGA. Licensed under **GPL-3.0** (PS2Recomp, PCSX2 and god-of-war-recomp code is GPL); distributing a
build means providing its source.
