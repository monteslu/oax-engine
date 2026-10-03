# oax engine
# Copyright (C) 2026 Luis Montes
#
# This file is part of the oax engine, a fork of ioquake3.
# It is free software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version. The combined engine is distributed under GPLv3 (see
# COPYING-GPLv3.txt).
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details.

# Rigid body physics: Erin Catto's Box3D (MIT, code/box3d, see
# code/box3d/README.md) and the engine module that owns Box3D worlds and
# hands them to gamecode (code/physics, syscall block 1200-1299 in
# qcommon/oax.h).
#
# Box3D is a static library of its own: C17, and -ffp-contract=off with no
# fast-math so a step gives the same bits on every build (x86-64 has no FMA
# at its baseline and wasm has none at all, but the flag keeps it that way
# if someone raises -march). Box3D picks its SIMD path per target: x86-64
# native builds use SSE2; wasm builds (the wasmcart cart and plain
# Emscripten) use the same SSE2 path on wasm SIMD128; arm64 native builds
# use NEON. The physics-determinism test shows x86-64 native and the cart
# give identical results for any worker count (the cart also runs Box3D's
# worker threads, phys_tasks.c). Bit-identical results between the NEON
# path and the SSE2 path are not verified.

include_guard(GLOBAL)

set(BOX3D_DIR ${SOURCE_DIR}/box3d)

file(GLOB BOX3D_SOURCES ${BOX3D_DIR}/src/*.c)

add_library(box3d STATIC ${BOX3D_SOURCES})
set_target_properties(box3d PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    INTERPROCEDURAL_OPTIMIZATION OFF)
target_include_directories(box3d PUBLIC ${BOX3D_DIR}/include PRIVATE ${BOX3D_DIR}/src)
target_compile_definitions(box3d PRIVATE NDEBUG)
if(MSVC)
    target_compile_options(box3d PRIVATE /fp:precise /w)
else()
    target_compile_options(box3d PRIVATE -ffp-contract=off -fno-fast-math -w)
endif()
if(WASMCART OR EMSCRIPTEN)
    # Box3D's SSE2 path on wasm SIMD128 (Emscripten maps the SSE2
    # intrinsics it uses onto f32x4 operations with the same IEEE results).
    # Any wasm build needs it: core.h selects SSE2 for wasm, and
    # emmintrin.h refuses to compile without -msse2.
    target_compile_options(box3d PRIVATE -msimd128 -msse2)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(box3d PUBLIC Threads::Threads m)
endif()

# the engine side: shared by the server (game worlds) and the client
# (cgame worlds); both binaries link Box3D
set(PHYSICS_SOURCES
    ${SOURCE_DIR}/physics/phys_main.c
    ${SOURCE_DIR}/physics/phys_bsp.c
    ${SOURCE_DIR}/physics/phys_ragdoll.c
    ${SOURCE_DIR}/physics/phys_syscalls.c
    ${SOURCE_DIR}/physics/phys_tasks.c
    ${SOURCE_DIR}/physics/phys_vehicle.c
    ${SOURCE_DIR}/physics/phys_droptest.c
)
set_source_files_properties(${PHYSICS_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-ffp-contract=off;-std=gnu17"
    INCLUDE_DIRECTORIES "${BOX3D_DIR}/include")

if(WASMCART AND WASMCART_THREADS)
    # wasi_thread_start: sets the new thread's stack pointer before any C
    # runs on it (phys_tasks.c)
    enable_language(ASM)
    list(APPEND PHYSICS_SOURCES ${SOURCE_DIR}/physics/wc_thread_start.S)
endif()

list(APPEND SERVER_LIBRARIES box3d)
list(APPEND CLIENT_LIBRARIES box3d)
