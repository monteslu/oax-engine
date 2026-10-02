# Box3D (vendored)

Erin Catto's Box3D 3D rigid body physics engine, MIT license (`LICENSE` in
this directory, kept as shipped). Upstream: https://github.com/erincatto/box3d

Pinned source: upstream commit `29bf523ce7bc4590aba9f17c9db791cdc5c4397e`
("Fixes 02 (#31)") plus one local change: `src/block_allocator.h` wraps
`#define B3_BLOCK_EXPONENT 8` in `#ifndef B3_BLOCK_EXPONENT`, so the block
size of Box3D's block allocator can be overridden at build time for
small-RAM targets (no behavior change at the default, which this engine
uses).

Copied as-is: `include/box3d/*.h`, `src/*.c`, `src/*.h`, `src/*.inl`. Not
copied: samples, tests, benchmarks, docs, the upstream CMake files and the
Visual Studio natvis. Apart from the change above nothing in these files is
modified; the engine's build (`cmake/physics.cmake`) compiles them as a
static library with `-ffp-contract=off` and no fast-math. Box3D picks its
SIMD path per target: x86-64 native builds use SSE2, wasm builds (the
wasmcart cart and plain Emscripten) use the same SSE2 path on wasm SIMD128
(`-msimd128 -msse2`), and arm64 native builds use NEON. Native builds and
the cart are multithreaded; a plain Emscripten build runs one worker. The
`physics-determinism` test shows that x86-64 native and the cart give
bit-identical simulations, and that the result does not depend on the
worker thread count. Bit-identical results between arm64 (NEON) and the
SSE2 builds are not verified.

The engine side that owns Box3D worlds is `code/physics` (GPL like the
rest of the engine).
