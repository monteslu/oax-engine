# Rigid body physics: Erin Catto's Box3D (MIT, code/box3d, see
# code/box3d/README.md) and the engine module that owns Box3D worlds and
# hands them to gamecode (code/physics, syscall block 1200-1299 in
# qcommon/oax.h).
#
# Box3D is a static library of its own: C17, and -ffp-contract=off with no
# fast-math so a step gives the same bits on every build (x86-64 has no FMA
# at its baseline and wasm has none at all, but the flag keeps it that way
# if someone raises -march). Both builds run Box3D's SSE2 path: native
# x86-64 SSE2, and on the cart wasm SIMD128 (the cart also runs Box3D's
# worker threads, phys_tasks.c). The physics-determinism test proves the
# two give identical results for any worker count.

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
if(WASMCART)
    # Box3D's SSE2 path on wasm SIMD128 (Emscripten maps the SSE2
    # intrinsics it uses onto f32x4 operations with the same IEEE results)
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
