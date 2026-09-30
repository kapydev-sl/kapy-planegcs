# Vendored planegcs (Kapy fork)

Our own build of [`Salusoft89/planegcs`](https://github.com/Salusoft89/planegcs)
— FreeCAD's 2D geometric constraint solver (GCS) compiled to WebAssembly.

We vendor + build it ourselves (instead of consuming the prebuilt
`@salusoft89/planegcs` npm WASM) so we can **expose extra `GcsSystem`
methods** the npm build doesn't, and re-sync FreeCAD on our own schedule.

The TypeScript wrapper (`GcsWrapper`, the sketch layer, enums, types) is
still consumed from the `@salusoft89/planegcs` npm package — we only
replace the compiled `.wasm`.

## What we added

`dist/planegcs.wasm` is built from `build/` with two patches on top of
upstream `v1.1.7` (commit `5fd4468`), and `dist/planegcs_capi.wasm` (the C API,
below) sits beside it:

- **`GcsSystem::get_dependent_params()`** — returns the indices (into
  `p_params`) of the parameters the solver found _dependent_
  (not fully constrained). A sketch primitive is fully constrained iff
  none of its params appear here. This is the same QR-diagnosis FreeCAD's
  Sketcher uses to paint fully-constrained geometry. The patch lives in
  `build/build-bindings/templates/gcs_system.cpp.njk` (search
  `get_dependent_params`).

- **A solve that does not depend on the heap.** FreeCAD's GCS ordered a
  subsystem's parameters by their ADDRESS, and every parameter is its own
  `new double`, so the same sketch solved differently depending on what the
  module had allocated before it — last-bit noise on a well-constrained
  sketch, a whole different translation on an under-constrained one
  (`board-cutout` with `board=nano`: kBL.y −19.801 or −20.54). The editor
  keeps one module for the whole session, so a document could solve
  differently after another one than opened alone. Three sites, each marked
  `Kapy patch` in the source:
    - `SubSystem.cpp: SubSystem::initialize` — the parameter list (the
      Jacobian's column order) keeps the caller's order instead of
      intersecting two `std::set<double*>`;
    - `SubSystem.cpp: SubSystem::redirectParams` (+ `pmapOrder` in
      `SubSystem.h`) — the slots are seeded walking the parameters in the
      caller's order, not the address-keyed `pmap`. Several parameters an
      equality reduced share a slot and the last one written seeds it; that
      was the one at the highest address, and is now the last in the
      caller's order — what a freshly booted module, which allocates in
      index order, used to do. Seeding from the KEPT parameter instead was
      tried and rejected: it made `branchFlipLoss` flip a well-constrained
      sliver sketch the old solver flipped on none of the 41 heaps probed;
    - `GCS.cpp: System::solve(SubSystem*, SubSystem*, …)` — the two-subsystem
      union keeps first-seen order instead of sorting both lists by address.

    The pointer-keyed maps and sets that remain are lookups and are never
    iterated in an order that reaches the numbers. The gate is
    `tests/unit/workers/solver/heapDeterminism.planegcs.test.ts`: the same input
    on a fresh module, on modules with N idle systems alive, and after the whole
    corpus has been solved, must agree bit for bit.

## The C API (`dist/planegcs_capi.wasm`, v3.3 wave S1)

The second artifact of the same build: the solver behind about twenty
`extern "C"` functions (`gcs_new`, `gcs_push_param`, `gcs_geom`,
`gcs_constraint`, `gcs_solve`, `gcs_apply`, `gcs_dof`, `gcs_diag_count` /
`gcs_diag_at`, `gcs_last_error`, ...) instead of embind. It is what the Rust
core will call (D163, D168): no JS wrapper above it, scalars only across the
boundary, every export `noexcept`, and every failure a `-1` with a code in
`gcs_last_error` (a bad index, kind, slot or list; nothing is thrown or
aborted outward). `gcs_geom(kind, a0..a8)` and `gcs_constraint(kind, ...)` are
generic: the constraint kinds are one table, `build/capi/constraint_kinds.def`,
that the C++ dispatch, the slot validation and the test shim all read.

The embind build stays until the app has moved off it (S8, D168). Both come out
of one `build-native.sh` run and one set of sources; `planegcs.wasm` is
byte-identical to what it was before the C API existed.

What the C API build has that the embind one does not:

- **A heap that grows** (`-s ALLOW_MEMORY_GROWTH=1`, D169). The embind build
  has a fixed 16 MB and a chain of about 400 points aborts it with OOM, killing
  the instance. `synthetic/oom-chain` (700 points) solves through the C API.
- **Export names that survive -O3.** At -O3 emcc 3.1.45 renames every wasm
  export to a letter and only the JS glue knows the map.
  `MINIFY_WASM_EXPORT_NAMES` is an internal setting emcc refuses on the command
  line; linking `library_exports.js` (`-lexports.js`) is one of the two
  documented things that turn it off. So `gcs_*` is readable out of the
  binary, by Rust or by a test (`tests/unit/workers/solver/capi.planegcs.test.ts`
  checks every name in `build/capi/gcs_capi.exports` is exported).
- **An answer that is a function of the sketch alone** (the next section).

### Why the C API build is not just the embind build with a different front

planegcs still depended on the heap after the patch above. Solving the same
sketch after a few unrelated systems (`heapShift` in
`tests/unit/helpers/solverOracle/capture.ts`) changes the answer of 35 of the
602 sketches of the S0 oracle, and it does so in the **embind build too**: the
frozen result of S0 is what one particular layout gave. Three causes, three
fixes, all in the C API build only (the embind `.wasm` stays what it was):

- **Memory planegcs reads before writing.** A fresh wasm memory is zero, so the
  first system solved in an instance saw zeros; later ones saw the leftovers of
  earlier ones. `build/capi/gcs_capi_alloc.cpp` replaces `operator new` with a
  zeroing one, `EIGEN_INITIALIZE_MATRICES_BY_ZERO` zeroes Eigen's matrices, and
  `-ftrivial-auto-var-init=zero` zeroes the stack locals. This is what moved
  every sketch to a stable answer.
- **Containers ordered by a `Constraint*` address** (the redundant-constraint
  tie-break of the diagnosis, and the maps and sets it builds). `KAPY_SEQ_ORDER`
  (in `Constraints.h`) gives every `Constraint` a sequence number at
  construction and orders `SET_pC` / `MAP_pC` by it; those containers are
  `std::less<Constraint*>` again when the macro is off. Only the library the C
  API links (`LibsModuleSeq` in `CMakeLists.txt`) defines it.
- **Containers ordered by a `double*` address.** The C API hands out parameters
  from chunks of 16384 doubles in push order (`Sys::newParam`), so address order
  is index order inside a chunk. A system with more than 16384 parameters
  starts a second chunk, whose place relative to the first is the allocator's:
  such a system is still correct, but not covered by the property above.

The gate: `tests/unit/e2e/solverOracle.capi.planegcs.test.ts` solves all 602
sketches through the C API, each once on a clean heap and once after 13
unrelated systems, and they must agree bit for bit. Against the S0 freeze,
582 sketches are identical in every hash and 20 are not (their digests are in
`solverOracle.capi.frozen.json`); in all 20 the committed solves are still the
frozen bits, and the difference is a drag frame, in 11 of them a frame the
embind build reported as failed (a conflicting constraint) that now converges.
`tests/unit/workers/solver/heapDeterminism.planegcs.test.ts` runs over both
builds.

## Files we modified

Everything else under `build/` is upstream (Salusoft89 `v1.1.7`, FreeCAD's GCS)
byte for byte. This is the whole list of what is ours, for the public
`kapy-planegcs` repository and for the LGPL notice (`LICENSES.md`):

Modified upstream files:

- `build/build-bindings/templates/gcs_system.cpp.njk` — `get_dependent_params`.
- `build/planegcs/SubSystem.cpp`, `build/planegcs/SubSystem.h` — the heap
  determinism patch (parameter order, `pmapOrder`); `SubSystem.h` also has the
  `MAP_pC` type of the sequence-ordered `c2p` (C API build).
- `build/planegcs/GCS.cpp` — the two-subsystem union order; `SET_pC` /
  `MAP_pC` in place of the `Constraint*`-keyed containers.
- `build/planegcs/GCS.h` — the same container types.
- `build/planegcs/Constraints.h`, `build/planegcs/Constraints.cpp` — the
  sequence number and `ConstraintLess` (`KAPY_SEQ_ORDER`).
- `build/planegcs/CMakeLists.txt` — the DSE flag, the `planegcs-capi` target,
  `LibsModuleSeq` and the C API link line.

Files that are ours entirely:

- `build/capi/gcs_capi.h`, `build/capi/gcs_capi.cpp`,
  `build/capi/gcs_capi_constraints.cpp`, `build/capi/constraint_kinds.def`,
  `build/capi/gcs_capi.exports`, `build/capi/gcs_capi_alloc.cpp` — the C API.
- `build/build-native.sh` — the reproducible native build of both artifacts.
- `dist/planegcs.wasm`, `dist/planegcs_capi.wasm`, `dist/planegcs_capi.js` —
  their output.

Re-syncing FreeCAD with `update_freecad.sh` overwrites `SubSystem.cpp` and
`GCS.cpp`: re-apply the `Kapy patch` blocks and run the gate.

The emscripten JS glue is byte-identical to upstream's, and embind
registers functions at runtime, so swapping only the `.wasm` is enough —
the npm `init_planegcs_module` glue loads it and the extra method appears
on the `GcsSystem` instance. The matching TS type is declared in
`src/workers/solver/planegcs.augment.d.ts`.

## Consumed by

- `scripts/build/workers.mjs` copies `dist/planegcs.wasm` into
  `public/workers/planegcs.wasm`.
- CDN deploys (`WORKERS_WASM_BASE_URL` set) must upload **this**
  `dist/planegcs.wasm` to the bucket — see `docs/deploy-ovh.md`.

## Rebuilding without Docker (the reproducible path)

`build/build-native.sh` builds both artifacts from pinned inputs and prints
each wasm's sha256; `--publish` copies them over `dist/`; `--twice` builds from
scratch two times and fails unless every wasm is byte for byte the same. Everything it needs lives under a
directory you name, nothing is installed globally:

```bash
# once: emsdk 3.1.45 and a Node 20 somewhere outside the system disk
git clone https://github.com/emscripten-core/emsdk.git "$EMSDK"
"$EMSDK/emsdk" install 3.1.45 && "$EMSDK/emsdk" activate 3.1.45

TOOLCHAIN=/some/dir EMSDK="$EMSDK" NODE20=/path/to/node20/bin \
    services/planegcs/build/build-native.sh --publish
```

| Input    | Pin                                                                      |
| -------- | ------------------------------------------------------------------------ |
| emcc     | **3.1.45** (the version the Dockerfile names)                            |
| Eigen    | 3.4.0 upstream tag tarball, sha256 `8586084f…1c72`                       |
| Boost    | Ubuntu jammy `libboost1.74-dev_1.74.0-14ubuntu3`, sha256 `4d9c90e4…7a78` |
| bindings | generated by `npm ci && npm run build:bindings` under Node 20            |

What this taught, measured on 2026-09-23:

- **The emsdk 3.1.45 clang is not deterministic on `GCS.cpp`.** The same
  sources and flags gave four different `GCS.cpp.o` in six compiles: dead-store
  elimination's partial-overwrite tracking shortens an `Eigen::SparseMatrix`
  zeroing memset differently from run to run. The artifact committed before
  the heap patch (`aaa0173f…`) is one of those draws — it came out of two of
  roughly eight identical builds. `CMakeLists.txt` now passes
  `-mllvm -enable-dse-partial-overwrite-tracking=false`, and with it twelve
  compiles in a row are identical and so is every full build.
- **Eigen and Boost matter.** Upstream Boost 1.74 headers differ from
  Ubuntu's and give another wasm; Boost 1.71 and Eigen 3.3.7 do not compile
  under C++20 at all.

## Rebuilding with Docker

The build is Dockerized + reproducible. The host steps need **Node 20**
(the pinned `tree-sitter-cpp` native addon does not compile on Node ≥22).

```bash
cd vendor/planegcs/build

# 1. Generate the embind C++ bindings + TS (host, Node 20).
docker run --rm -v "$PWD":/work -w /work node:20 \
  bash -lc "npm install && npm run build:bindings"

# 2. Build the emscripten toolchain image (emsdk 3.1.45 + Eigen + Boost).
docker build -t gcs_builder .

# 3. Compile to WASM (runs cmake + make inside the image).
docker run --rm -v "$PWD/planegcs":/src gcs_builder \
  sh -c 'emcmake cmake . -DCMAKE_BUILD_TYPE=Release && emmake make -j "$(nproc)"'

# 4. Publish the artifact.
cp planegcs/bin/planegcs.wasm ../dist/planegcs.wasm
cp planegcs/bin/planegcs.js   ../dist/planegcs.js
```

To re-sync FreeCAD's GCS source, run `planegcs/update_freecad.sh` inside
the Node 20 container, re-apply the patch if the template moved, then
rebuild.

Generated files under `build/` (`planegcs/bindings.cpp`,
`planegcs_dist/*.ts`) are reproduced by step 1; CMake artifacts
(`CMakeCache.txt`, `CMakeFiles/`, `Makefile`, `bin/`) are build outputs.

Upstream is LGPL-2.1 — see `build/LICENSE`.
