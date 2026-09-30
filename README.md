# kapy-planegcs

The modified build of **planegcs** (FreeCAD's 2D geometric constraint solver, via
`@salusoft89/planegcs`) that [Kapy CAD](https://kapycad.com) ships as WebAssembly
(`planegcs.wasm` and `planegcs_capi.wasm`).

This repository publishes the complete modified sources and the build recipe, as
required by planegcs's licence (LGPL-2.0-or-later, see `build/LICENSE`):

- **Sources:** `build/planegcs/` (our vendored fork), with our modifications listed in `BUILD.md`
  (heap-determinism patch, zero-initialised allocations, sequence-ordered constraints,
  memory growth).
- **C API:** `build/capi/` (`gcs_capi.cpp`, the export list and constraint kinds).
- **Build recipe:** `build/build-native.sh` (reproducible, `--twice` checks identical bytes),
  `build/Dockerfile`, `build/build-bindings/`.

The Kapy CAD application loads these modules as separate, replaceable files; they can be
rebuilt from this repository and swapped in.
