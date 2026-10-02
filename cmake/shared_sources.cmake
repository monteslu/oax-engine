include_guard(GLOBAL)

include(utils/add_git_dependency)
include(utils/disable_warnings)
include(idgui)

set(COMMON_SOURCES
    ${SOURCE_DIR}/qcommon/cm_load.c
    ${SOURCE_DIR}/qcommon/cm_patch.c
    ${SOURCE_DIR}/qcommon/cm_polylib.c
    ${SOURCE_DIR}/qcommon/cm_test.c
    ${SOURCE_DIR}/qcommon/cm_trace.c
    ${SOURCE_DIR}/qcommon/cm_terrain.c
    ${SOURCE_DIR}/qcommon/oax_terrain.c
    ${SOURCE_DIR}/qcommon/cm_navgeom.c
    ${SOURCE_DIR}/qcommon/cmd.c
    ${SOURCE_DIR}/qcommon/common.c
    ${SOURCE_DIR}/qcommon/cvar.c
    ${SOURCE_DIR}/qcommon/files.c
    ${SOURCE_DIR}/qcommon/md4.c
    ${SOURCE_DIR}/qcommon/md5.c
    ${SOURCE_DIR}/qcommon/msg.c
    ${SOURCE_DIR}/qcommon/net_chan.c
    ${SOURCE_DIR}/qcommon/net_ip.c
    ${SOURCE_DIR}/qcommon/huffman.c
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/q_detmath.c
    ${SOURCE_DIR}/qcommon/bspx.c
    ${SOURCE_DIR}/qcommon/oax_common.c
    ${SOURCE_DIR}/qcommon/detmath/__sin.c
    ${SOURCE_DIR}/qcommon/detmath/__cos.c
    ${SOURCE_DIR}/qcommon/detmath/__rem_pio2.c
    ${SOURCE_DIR}/qcommon/detmath/__rem_pio2_large.c
    ${SOURCE_DIR}/qcommon/detmath/sin.c
    ${SOURCE_DIR}/qcommon/detmath/cos.c
    ${SOURCE_DIR}/qcommon/detmath/atan.c
    ${SOURCE_DIR}/qcommon/detmath/atan2.c
    ${SOURCE_DIR}/qcommon/detmath/acos.c
    ${SOURCE_DIR}/qcommon/unzip.c
    ${SOURCE_DIR}/qcommon/ioapi.c
    ${SOURCE_DIR}/qcommon/vm.c
    ${SOURCE_DIR}/qcommon/vm_armv7l.c
    ${SOURCE_DIR}/qcommon/vm_interpreted.c
    ${SOURCE_DIR}/qcommon/vm_powerpc.c
    ${SOURCE_DIR}/qcommon/vm_sparc.c
    ${SOURCE_DIR}/qcommon/vm_x86.c
    ${SOURCE_DIR}/qcommon/cm_guisurf.c
    ${IDGUI_SOURCES}
)

# oax terrain collision: the same float results on every build
set_source_files_properties(${SOURCE_DIR}/qcommon/cm_terrain.c ${SOURCE_DIR}/qcommon/oax_terrain.c ${SOURCE_DIR}/qcommon/cm_navgeom.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# musl sources, kept unmodified (see qcommon/detmath/README.md)
file(GLOB DETMATH_SOURCES ${SOURCE_DIR}/qcommon/detmath/*.c)
set_source_files_properties(${DETMATH_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-builtin")

disable_warnings(
    ${DETMATH_SOURCES}
    ${SOURCE_DIR}/qcommon/unzip.c
    ${SOURCE_DIR}/qcommon/ioapi.c
)

add_git_dependency(${SOURCE_DIR}/qcommon/common.c)

if(WASMCART)
    set(SYSTEM_SOURCES
        ${SOURCE_DIR}/sys/con_log.c
        ${WASMCART_SYSTEM_SOURCES}
    )
else()
    set(SYSTEM_SOURCES
        ${SOURCE_DIR}/sys/con_log.c
        ${SOURCE_DIR}/sys/sys_autoupdater.c
        ${SOURCE_DIR}/sys/sys_main.c
        ${SYSTEM_PLATFORM_SOURCES}
    )
endif()

set(SERVER_SOURCES
    ${SOURCE_DIR}/server/sv_bot.c
    ${SOURCE_DIR}/server/sv_client.c
    ${SOURCE_DIR}/server/sv_ccmds.c
    ${SOURCE_DIR}/server/sv_game.c
    ${SOURCE_DIR}/server/sv_game_oax.c
    ${SOURCE_DIR}/server/sv_gui_oax.c
    ${SOURCE_DIR}/server/sv_init.c
    ${SOURCE_DIR}/server/sv_main.c
    ${SOURCE_DIR}/server/sv_net_chan.c
    ${SOURCE_DIR}/server/sv_snapshot.c
    ${SOURCE_DIR}/server/sv_world.c
    ${SOURCE_DIR}/server/sv_script_oax.c
    ${SOURCE_DIR}/server/sv_nav_oax.c
)

# id Tech 4 script VM (C++), driven by sv_script_oax.c
include(idscript)
list(APPEND SERVER_SOURCES ${IDSCRIPT_SOURCES})

# Box3D physics (code/box3d, code/physics): game and cgame worlds
include(physics)
list(APPEND SERVER_SOURCES ${PHYSICS_SOURCES})
# Recast/Detour navmesh (code/thirdparty/recastnavigation), driven by sv_nav_oax.c
include(recast)
list(APPEND SERVER_SOURCES ${RECAST_SOURCES})

set(BOTLIB_SOURCES
    ${SOURCE_DIR}/botlib/be_aas_bspq3.c
    ${SOURCE_DIR}/botlib/be_aas_cluster.c
    ${SOURCE_DIR}/botlib/be_aas_debug.c
    ${SOURCE_DIR}/botlib/be_aas_entity.c
    ${SOURCE_DIR}/botlib/be_aas_file.c
    ${SOURCE_DIR}/botlib/be_aas_main.c
    ${SOURCE_DIR}/botlib/be_aas_move.c
    ${SOURCE_DIR}/botlib/be_aas_optimize.c
    ${SOURCE_DIR}/botlib/be_aas_reach.c
    ${SOURCE_DIR}/botlib/be_aas_route.c
    ${SOURCE_DIR}/botlib/be_aas_routealt.c
    ${SOURCE_DIR}/botlib/be_aas_sample.c
    ${SOURCE_DIR}/botlib/be_ai_char.c
    ${SOURCE_DIR}/botlib/be_ai_chat.c
    ${SOURCE_DIR}/botlib/be_ai_gen.c
    ${SOURCE_DIR}/botlib/be_ai_goal.c
    ${SOURCE_DIR}/botlib/be_ai_move.c
    ${SOURCE_DIR}/botlib/be_ai_weap.c
    ${SOURCE_DIR}/botlib/be_ai_weight.c
    ${SOURCE_DIR}/botlib/be_ea.c
    ${SOURCE_DIR}/botlib/be_interface.c
    ${SOURCE_DIR}/botlib/l_crc.c
    ${SOURCE_DIR}/botlib/l_libvar.c
    ${SOURCE_DIR}/botlib/l_log.c
    ${SOURCE_DIR}/botlib/l_memory.c
    ${SOURCE_DIR}/botlib/l_precomp.c
    ${SOURCE_DIR}/botlib/l_script.c
    ${SOURCE_DIR}/botlib/l_struct.c
)
