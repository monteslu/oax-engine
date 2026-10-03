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

include_guard(GLOBAL)

# In-world GUIs: the C++ port of DOOM-3's GUI system (code/idgui, GPLv3,
# see docs/idtech4-attribution.md). Linked into every binary that runs a
# server or a client; the engine reaches it only through idgui_public.h.

enable_language(CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(IDGUI_SOURCES
    ${SOURCE_DIR}/idgui/idlib/Str.cpp
    ${SOURCE_DIR}/idgui/idlib/Token.cpp
    ${SOURCE_DIR}/idgui/idlib/Lexer.cpp
    ${SOURCE_DIR}/idgui/idlib/Parser.cpp
    ${SOURCE_DIR}/idgui/idlib/Lib.cpp
    ${SOURCE_DIR}/idgui/idlib/HashIndex.cpp
    ${SOURCE_DIR}/idgui/idgui_main.cpp
    ${SOURCE_DIR}/idgui/DeviceContext.cpp
    ${SOURCE_DIR}/idgui/Winvar.cpp
    ${SOURCE_DIR}/idgui/RegExp.cpp
    ${SOURCE_DIR}/idgui/GuiScript.cpp
    ${SOURCE_DIR}/idgui/Window.cpp
    ${SOURCE_DIR}/idgui/SimpleWindow.cpp
    ${SOURCE_DIR}/idgui/ChoiceWindow.cpp
    ${SOURCE_DIR}/idgui/SliderWindow.cpp
    ${SOURCE_DIR}/idgui/ListWindow.cpp
    ${SOURCE_DIR}/idgui/EditWindow.cpp
    ${SOURCE_DIR}/idgui/UserInterface.cpp
)

# id's code predates most of today's warnings; exceptions and RTTI are off
# (errors longjmp, see idgui_main.cpp)
set_source_files_properties(${IDGUI_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-fno-exceptions;-fno-rtti;-w;-ffp-contract=off")
