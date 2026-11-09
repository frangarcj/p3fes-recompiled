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
- `p3fes_tmx.py`: Atlus TMX texture decoding.

Do not commit extracted game assets or converted output.
