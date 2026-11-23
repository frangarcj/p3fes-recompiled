# vitaGL patches

The Vita build links [vitaGL](https://github.com/Rinnegatamante/vitaGL) at upstream commit
`c707ff00d9a372f0ba7056b4a4b723f5a00ba880` with these patches applied in name order. The release workflow
(`.github/workflows/release-vita.yml`) does that and checks that the patched sources hash to the git tree in
`VITAGL_TREE`.

By hand:

```sh
git clone https://github.com/Rinnegatamante/vitaGL && cd vitaGL
git checkout c707ff00d9a372f0ba7056b4a4b723f5a00ba880
git am /path/to/vitagl-patches/*.patch
make NO_SPLASHSCREEN=1 NO_TEX_COMBINER=1 HAVE_SHARK_LOG=1 USE_SCRATCH_MEMORY=1 LOG_ERRORS=1 \
     SHARED_RENDERTARGETS=2 DRAW_SPEEDHACK=2 PRIMITIVES_SPEEDHACK=1 INDICES_DRAW_SPEEDHACK=1 install
```

To change the set, regenerate it from a vitaGL checkout with
`git format-patch --no-signature --zero-commit -N -o <this directory> <base>..<head>` and update `VITAGL_COMMIT` and
`VITAGL_TREE` (`git rev-parse '<head>^{tree}'`) in the workflow.
