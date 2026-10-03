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
