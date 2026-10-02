# detmath

Deterministic double-precision `sin`, `cos`, `atan`, `atan2` and `acos` for
the QVM math traps (and the engine's own `AngleVectors`), so native and wasm
builds return the same bits. The `.c` files are unmodified copies of musl's
`src/math` (MIT, `COPYRIGHT.musl`), taken from the musl tree that emscripten
ships. `libm.h` is ours: musl's internal helper macros plus the renames to
`Q_det*`. A wasmcart's libc is musl, so these reproduce the cart's results.

`floor`, `ceil` and `sqrt` are exact in IEEE 754 and stay on the host libm.
