# id Tech 4 (DOOM-3) script VM, ported into the engine as C++ (code/idscript)
# with the idLib subset it needs (code/idlib_lite). Linked into every binary
# that has the server; the game module drives it through the G_OAX_SCRIPT_*
# syscalls (code/server/sv_script_oax.c).
#
# Built without exceptions or RTTI: compile and runtime errors leave through
# longjmp (see idlib_lite.h). -ffp-contract=off keeps float results the same
# on every build (no fused multiply-add on one and not the other).

include_guard(GLOBAL)

set(IDLIB_LITE_SOURCES
    ${SOURCE_DIR}/idlib_lite/Lib_lite.cpp
    ${SOURCE_DIR}/idlib_lite/Str.cpp
    ${SOURCE_DIR}/idlib_lite/Token.cpp
    ${SOURCE_DIR}/idlib_lite/Lexer.cpp
    ${SOURCE_DIR}/idlib_lite/Parser.cpp
    ${SOURCE_DIR}/idlib_lite/HashIndex.cpp
)

set(IDSCRIPT_CXX_SOURCES
    ${SOURCE_DIR}/idscript/Script_Event.cpp
    ${SOURCE_DIR}/idscript/Script_Program.cpp
    ${SOURCE_DIR}/idscript/Script_Compiler.cpp
    ${SOURCE_DIR}/idscript/Script_Interpreter.cpp
    ${SOURCE_DIR}/idscript/Script_Thread.cpp
    ${SOURCE_DIR}/idscript/oax_script.cpp
)

set(IDSCRIPT_SOURCES
    ${IDLIB_LITE_SOURCES}
    ${IDSCRIPT_CXX_SOURCES}
    ${SOURCE_DIR}/idlib_lite/idlib_bridge.c
)

set_source_files_properties(${IDLIB_LITE_SOURCES} ${IDSCRIPT_CXX_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-rtti;-ffp-contract=off;-std=gnu++11;-w"
    INCLUDE_DIRECTORIES "${SOURCE_DIR}/idlib_lite;${SOURCE_DIR}/idscript")
