#!/usr/bin/env bash
# gate.sh: the full gate. Builds the native client and the wasmcart cart,
# packs the cart with OpenArena content, and runs the romdev tests.
#
#   misc/ci/gate.sh [--update] [test-name ...]
#
# Needs: cmake, a C compiler, emsdk (EMSDK or emcc on PATH), node, unzip,
# OpenArena content (OA_BASEOA or a distro install), OA QVMs (OA_QVM_DIR, a
# directory containing vm/*.qvm; defaults to a sibling oa-gamecode build),
# and a romdev server (ROMDEV_URL, default http://127.0.0.1:7331).
#
# Exit codes follow tests/romdev/run.mjs: 0 pass, 1 test failure,
# 3 infrastructure (romdev unreachable). A build failure exits 2.

set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"
JOBS="${JOBS:-$(nproc)}"

if ! command -v emcc >/dev/null 2>&1; then
  if [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
    # shellcheck disable=SC1091
    source "$EMSDK/emsdk_env.sh" >/dev/null
  else
    echo "gate: emcc not found; set EMSDK to an emsdk checkout" >&2
    exit 2
  fi
fi

if [ -z "${OA_QVM_DIR:-}" ]; then
  OA_QVM_DIR="$(ls -d "$ROOT"/../oa-gamecode/build/release-*/oax 2>/dev/null | head -1 || true)"
fi
if [ -z "$OA_QVM_DIR" ] || [ ! -d "$OA_QVM_DIR/vm" ]; then
  echo "gate: no OpenArena QVMs; build OpenArena/gamecode and set OA_QVM_DIR" >&2
  exit 2
fi

echo "== native build"
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
cmake --build build-native -j"$JOBS" >build-native.log 2>&1 || { tail -30 build-native.log; exit 2; }

echo "== cart build"
emcmake cmake -S . -B build-cart -DWASMCART=ON -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
cmake --build build-cart -j"$JOBS" >build-cart.log 2>&1 || { tail -30 build-cart.log; exit 2; }

echo "== pack"
node misc/wasmcart/pack-cart.mjs --wasm build-cart/Release/ioquake3.wasm --qvm "$OA_QVM_DIR" --out build-cart/cart | tail -1

echo "== romdev tests"
node tests/romdev/run.mjs "$@"
