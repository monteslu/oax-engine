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

# Recast/Detour (zlib, code/thirdparty/recastnavigation, see its
# README.oax.md): the navigation mesh oax bots use where a map has no AAS.
# Linked into every binary that has the server, driven by
# code/server/sv_nav_oax.c through code/server/nav_oax.cpp.
#
# Built without exceptions or RTTI, and with -ffp-contract=off so native
# and wasm compute the same navmesh bytes.

include_guard(GLOBAL)

set(RECAST_DIR ${SOURCE_DIR}/thirdparty/recastnavigation)

set(RECAST_CXX_SOURCES
    ${RECAST_DIR}/Recast/Source/Recast.cpp
    ${RECAST_DIR}/Recast/Source/RecastAlloc.cpp
    ${RECAST_DIR}/Recast/Source/RecastArea.cpp
    ${RECAST_DIR}/Recast/Source/RecastAssert.cpp
    ${RECAST_DIR}/Recast/Source/RecastContour.cpp
    ${RECAST_DIR}/Recast/Source/RecastFilter.cpp
    ${RECAST_DIR}/Recast/Source/RecastLayers.cpp
    ${RECAST_DIR}/Recast/Source/RecastMesh.cpp
    ${RECAST_DIR}/Recast/Source/RecastMeshDetail.cpp
    ${RECAST_DIR}/Recast/Source/RecastRasterization.cpp
    ${RECAST_DIR}/Recast/Source/RecastRegion.cpp
    ${RECAST_DIR}/Detour/Source/DetourAlloc.cpp
    ${RECAST_DIR}/Detour/Source/DetourAssert.cpp
    ${RECAST_DIR}/Detour/Source/DetourCommon.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMesh.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMeshBuilder.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMeshQuery.cpp
    ${RECAST_DIR}/Detour/Source/DetourNode.cpp
    ${SOURCE_DIR}/server/nav_oax.cpp
)

set(RECAST_SOURCES ${RECAST_CXX_SOURCES})

set_source_files_properties(${RECAST_CXX_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-rtti;-ffp-contract=off;-std=gnu++11;-w"
    INCLUDE_DIRECTORIES "${RECAST_DIR}/Recast/Include;${RECAST_DIR}/Detour/Include")
