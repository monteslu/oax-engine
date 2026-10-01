# wasmcart cart build: the client as a standalone wasm module a wasmcart host
# drives one frame at a time. Emscripten is the compiler; none of its JS
# runtime, SDL port or virtual filesystem is used. The platform backend is
# code/wasmcart (in place of code/sys + code/sdl).

if(NOT WASMCART)
    return()
endif()

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "WASMCART=ON needs the Emscripten toolchain (emcmake cmake ...)")
endif()

set(CMAKE_EXECUTABLE_SUFFIX ".wasm")

set(BUILD_SERVER OFF CACHE INTERNAL "")
set(BUILD_RENDERER_GL1 OFF CACHE INTERNAL "")
set(BUILD_RENDERER_GL2 ON CACHE INTERNAL "")
set(USE_RENDERER_DLOPEN OFF CACHE INTERNAL "")
set(BUILD_GAME_LIBRARIES OFF CACHE INTERNAL "")
set(BUILD_GAME_QVMS OFF CACHE INTERNAL "")
set(USE_HTTP OFF CACHE INTERNAL "")
set(USE_OPENAL OFF CACHE INTERNAL "")
set(USE_OPENAL_DLOPEN OFF CACHE INTERNAL "")
set(USE_VOIP OFF CACHE INTERNAL "")
set(USE_MUMBLE OFF CACHE INTERNAL "")
set(USE_FREETYPE OFF CACHE INTERNAL "")

# The libraries Emscripten ships are not LTO objects
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)

add_compile_definitions(WASMCART)

# ERR_DROP unwinds Com_Frame with longjmp; wasm exception handling carries it
list(APPEND CLIENT_COMPILE_OPTIONS -sSUPPORT_LONGJMP=wasm)
list(APPEND RENDERER_COMPILE_OPTIONS -sSUPPORT_LONGJMP=wasm)

list(APPEND CLIENT_LINK_OPTIONS
    -sSTANDALONE_WASM=1
    -sSUPPORT_LONGJMP=wasm
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=512MB
    -sMAXIMUM_MEMORY=4GB
    -sSTACK_SIZE=8MB
    -sERROR_ON_UNDEFINED_SYMBOLS=1
    -sFILESYSTEM=0
    --no-entry
)

set(WASMCART_SYSTEM_SOURCES
    ${SOURCE_DIR}/wasmcart/wc_sys.c
    ${SOURCE_DIR}/wasmcart/wc_vfs.c
    ${SOURCE_DIR}/wasmcart/wc_debug.c
    ${SOURCE_DIR}/wasmcart/wc_debug_sv.c
)

set(WASMCART_CLIENT_SOURCES
    ${SOURCE_DIR}/wasmcart/wc_input.c
    ${SOURCE_DIR}/wasmcart/wc_snd.c
)

set(WASMCART_RENDERER_SOURCES
    ${SOURCE_DIR}/wasmcart/wc_glimp.c
)
