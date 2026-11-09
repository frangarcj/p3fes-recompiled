# Native asset preparation

These tools are reused from [Virtual-AK/fes-pc-recomp](https://github.com/Virtual-AK/fes-pc-recomp)
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
The same option also builds `p3fes_rw_viewer`, which uploads that neutral geometry
directly to raylib/OpenGL and renders an RMD without the emulated GS or an intermediate
glTF file. It is intentionally untextured while the PS2 raster path remains separate.

Do not commit extracted game assets or converted output.
