cmake_minimum_required(VERSION 3.13)

project(arcanum LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED YES)

# ------------------------------------------------------------------------------
# DEPENDENCIES
# ------------------------------------------------------------------------------

add_subdirectory("third_party/fpattern" EXCLUDE_FROM_ALL)
add_subdirectory("third_party/sdl3" EXCLUDE_FROM_ALL)
add_subdirectory("third_party/sdl3_mixer" EXCLUDE_FROM_ALL)
add_subdirectory("third_party/zlib" EXCLUDE_FROM_ALL)

# ENet v1.3.18 (MIT) — vendored under third_party/enet; C-only target so we
# do not require a C++ toolchain just for networking.
add_subdirectory("third_party/enet" EXCLUDE_FROM_ALL)

# PT51: optional UPnP port mapping for the co-op host. miniupnpc 2.2.8
# (BSD-3-Clause) vendored under third_party/miniupnpc; static, C only, links
# ws2_32 + iphlpapi on Windows (mingw-w64 OK).
option(ARCANUM_UPNP "Co-op host maps its UDP port via UPnP (miniupnpc)" ON)
if(ARCANUM_UPNP)
    add_subdirectory("third_party/miniupnpc" EXCLUDE_FROM_ALL)
endif()

add_subdirectory("first_party/bink_compat" EXCLUDE_FROM_ALL)
add_subdirectory("first_party/mss_compat" EXCLUDE_FROM_ALL)
add_subdirectory("first_party/tig" EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------------------
# ARCANUM
# ------------------------------------------------------------------------------

# Keep alphabetically sorted.
set(SRCS
    "src/game/a_name.c"
    "src/game/a_name.h"
    "src/game/ai.c"
    "src/game/ai.h"
    "src/game/anim_private.c"
    "src/game/anim_private.h"
    "src/game/anim.c"
    "src/game/anim.h"
    "src/game/animfx.c"
    "src/game/animfx.h"
    "src/game/antiteleport.c"
    "src/game/antiteleport.h"
    "src/game/area.c"
    "src/game/area.h"
    "src/game/background.c"
    "src/game/background.h"
    "src/game/bless.c"
    "src/game/bless.h"
    "src/game/brightness.c"
    "src/game/brightness.h"
    "src/game/broadcast.c"
    "src/game/broadcast.h"
    "src/game/ci.c"
    "src/game/ci.h"
    "src/game/combat.c"
    "src/game/combat.h"
    "src/game/coop_lite.c"
    "src/game/coop_menu.c"
    "src/game/coop_menu_sys.c"
    "src/game/coop_lite.h"
    "src/game/context.h"
    "src/game/criticals.h"
    "src/game/critter.c"
    "src/game/critter.h"
    "src/game/curse.c"
    "src/game/curse.h"
    "src/game/damage_type.h"
    "src/game/description.c"
    "src/game/description.h"
    "src/game/descriptions.h"
    "src/game/dialog.c"
    "src/game/dialog.h"
    "src/game/effect.c"
    "src/game/effect.h"
    "src/game/facade.c"
    "src/game/facade.h"
    "src/game/fate.c"
    "src/game/fate.h"
    "src/game/gameinit.c"
    "src/game/gameinit.h"
    "src/game/gamelib.c"
    "src/game/gamelib.h"
    "src/game/gfade.c"
    "src/game/gfade.h"
    "src/game/gmovie.c"
    "src/game/gmovie.h"
    "src/game/gsound.c"
    "src/game/gsound.h"
    "src/game/highres_config.c"
    "src/game/highres_config.h"
    "src/game/hrp.c"
    "src/game/hrp.h"
    "src/game/invensource.c"
    "src/game/invensource.h"
    "src/game/item_effect.c"
    "src/game/item_effect.h"
    "src/game/item.c"
    "src/game/item.h"
    "src/game/jumppoint.c"
    "src/game/jumppoint.h"
    "src/game/level.c"
    "src/game/level.h"
    "src/game/li.c"
    "src/game/li.h"
    "src/game/light_scheme.c"
    "src/game/light_scheme.h"
    "src/game/light.c"
    "src/game/light.h"
    "src/game/location.c"
    "src/game/location.h"
    "src/game/logbook.c"
    "src/game/logbook.h"
    "src/game/magictech.c"
    "src/game/magictech.h"
    "src/game/map.c"
    "src/game/map.h"
    "src/game/material.h"
    "src/game/mes.c"
    "src/game/mes.h"
    "src/game/monstergen.c"
    "src/game/monstergen.h"
    "src/game/mp_utils.c"
    "src/game/mp_utils.h"
    "src/game/mt_ai.c"
    "src/game/mt_ai.h"
    "src/game/mt_item.c"
    "src/game/mt_item.h"
    "src/game/mt_obj_node.c"
    "src/game/mt_obj_node.h"
    "src/game/multiplayer.c"
    "src/game/multiplayer.h"
    "src/game/name.c"
    "src/game/name.h"
    "src/game/newspaper.c"
    "src/game/newspaper.h"
    "src/game/obj_file.c"
    "src/game/obj_file.h"
    "src/game/obj_find.c"
    "src/game/obj_find.h"
    "src/game/obj_flags.c"
    "src/game/obj_flags.h"
    "src/game/obj_id.c"
    "src/game/obj_id.h"
    "src/game/obj_pool.c"
    "src/game/obj_pool.h"
    "src/game/obj_private.c"
    "src/game/obj_private.h"
    "src/game/obj.c"
    "src/game/obj.h"
    "src/game/object_node.c"
    "src/game/object_node.h"
    "src/game/object.c"
    "src/game/object.h"
    "src/game/oname.c"
    "src/game/oname.h"
    "src/game/party.c"
    "src/game/party.h"
    "src/game/path.c"
    "src/game/path.h"
    "src/game/player.c"
    "src/game/player.h"
    "src/game/portal.c"
    "src/game/portal.h"
    "src/game/portrait.c"
    "src/game/portrait.h"
    "src/game/proto.c"
    "src/game/proto.h"
    "src/game/quest.c"
    "src/game/quest.h"
    "src/game/random.c"
    "src/game/random.h"
    "src/game/reaction.c"
    "src/game/reaction.h"
    "src/game/reputation.c"
    "src/game/reputation.h"
    "src/game/resistance.c"
    "src/game/resistance.h"
    "src/game/roof.c"
    "src/game/roof.h"
    "src/game/rumor.c"
    "src/game/rumor.h"
    "src/game/sa.c"
    "src/game/sa.h"
    "src/game/schematic.h"
    "src/game/script_name.c"
    "src/game/script_name.h"
    "src/game/script.c"
    "src/game/script.h"
    "src/game/scroll.c"
    "src/game/scroll.h"
    "src/game/sector_block_list.c"
    "src/game/sector_block_list.h"
    "src/game/sector_light_list.c"
    "src/game/sector_light_list.h"
    "src/game/sector_object_list.c"
    "src/game/sector_object_list.h"
    "src/game/sector_roof_list.c"
    "src/game/sector_roof_list.h"
    "src/game/sector_script_list.c"
    "src/game/sector_script_list.h"
    "src/game/sector_script.c"
    "src/game/sector_script.h"
    "src/game/sector_sound_list.c"
    "src/game/sector_sound_list.h"
    "src/game/sector_tile_list.c"
    "src/game/sector_tile_list.h"
    "src/game/sector.c"
    "src/game/sector.h"
    "src/game/settings.c"
    "src/game/settings.h"
    "src/game/sfx.c"
    "src/game/sfx.h"
    "src/game/skill.c"
    "src/game/skill.h"
    "src/game/snd.h"
    "src/game/spell.c"
    "src/game/spell.h"
    "src/game/stat.c"
    "src/game/stat.h"
    "src/game/target.c"
    "src/game/target.h"
    "src/game/tb.c"
    "src/game/tb.h"
    "src/game/tc.c"
    "src/game/tc.h"
    "src/game/tech.c"
    "src/game/tech.h"
    "src/game/teleport.c"
    "src/game/teleport.h"
    "src/game/terrain.c"
    "src/game/terrain.h"
    "src/game/text_filter.c"
    "src/game/text_filter.h"
    "src/game/tf.c"
    "src/game/tf.h"
    "src/game/tile_block.c"
    "src/game/tile_block.h"
    "src/game/tile_script_list.c"
    "src/game/tile_script_list.h"
    "src/game/tile_script.c"
    "src/game/tile_script.h"
    "src/game/tile.c"
    "src/game/tile.h"
    "src/game/timeevent.c"
    "src/game/timeevent.h"
    "src/game/townmap.c"
    "src/game/townmap.h"
    "src/game/trap.c"
    "src/game/trap.h"
    "src/game/ui.c"
    "src/game/ui.h"
    "src/game/walkmask.c"
    "src/game/walkmask.h"
    "src/game/wall.c"
    "src/game/wall.h"
    "src/game/wallcheck.c"
    "src/game/wallcheck.h"
    "src/game/wp.c"
    "src/game/wp.h"
    "src/game/written.h"
    "src/ui/anim_ui.c"
    "src/ui/anim_ui.h"
    "src/ui/broadcast_ui.c"
    "src/ui/broadcast_ui.h"
    "src/ui/coop_chat_ui.c"
    "src/ui/coop_chat_ui.h"
    "src/ui/charedit_ui.c"
    "src/ui/charedit_ui.h"
    "src/ui/combat_ui.c"
    "src/ui/combat_ui.h"
    "src/ui/compact_ui.c"
    "src/ui/compact_ui.h"
    "src/ui/cyclic_ui.c"
    "src/ui/cyclic_ui.h"
    "src/ui/dialog_ui.c"
    "src/ui/dialog_ui.h"
    "src/ui/fate_ui.c"
    "src/ui/fate_ui.h"
    "src/ui/follower_ui.c"
    "src/ui/follower_ui.h"
    "src/ui/gameuilib.c"
    "src/ui/gameuilib.h"
    "src/ui/hotkey_ui.c"
    "src/ui/hotkey_ui.h"
    "src/ui/intgame.c"
    "src/ui/intgame.h"
    "src/ui/inven_ui.c"
    "src/ui/inven_ui.h"
    "src/ui/iso.c"
    "src/ui/iso.h"
    "src/ui/item_ui.c"
    "src/ui/item_ui.h"
    "src/ui/logbook_ui.c"
    "src/ui/logbook_ui.h"
    "src/ui/mainmenu_ui.c"
    "src/ui/mainmenu_ui.h"
    "src/ui/mp_ctrl_ui.c"
    "src/ui/mp_ctrl_ui.h"
    "src/ui/options_ui.c"
    "src/ui/options_ui.h"
    "src/ui/roller_ui.c"
    "src/ui/roller_ui.h"
    "src/ui/schematic_ui.c"
    "src/ui/schematic_ui.h"
    "src/ui/scrollbar_ui.c"
    "src/ui/scrollbar_ui.h"
    "src/ui/skill_ui.c"
    "src/ui/skill_ui.h"
    "src/ui/sleep_ui.c"
    "src/ui/sleep_ui.h"
    "src/ui/slide_ui.c"
    "src/ui/slide_ui.h"
    "src/ui/spell_ui.c"
    "src/ui/spell_ui.h"
    "src/ui/tb_ui.c"
    "src/ui/tb_ui.h"
    "src/ui/tech_ui.c"
    "src/ui/tech_ui.h"
    "src/ui/textedit_ui.c"
    "src/ui/textedit_ui.h"
    "src/ui/types.h"
    "src/ui/wmap_rnd.c"
    "src/ui/wmap_rnd.h"
    "src/ui/wmap_ui.c"
    "src/ui/wmap_ui.h"
    "src/ui/written_ui.c"
    "src/ui/written_ui.h"
    "src/net_compat.h"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/net/coop_upnp.c"
    "src/net/coop_upnp.h"
    "src/net/tig_net.h"
    "src/main.c"
    "src/crash_handler.c"
    "src/coop_paths.c"
    "src/coop_paths.h"
)

if(ANDROID)
    add_library(arcanum-ce SHARED)
else()
    add_executable(arcanum-ce WIN32 MACOSX_BUNDLE)
endif()

target_sources(arcanum-ce PUBLIC
    ${SRCS}
)

# PT28: build id (git short sha at configure time) for the log and title.
execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE COOP_BUILD_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT COOP_BUILD_SHA)
    set(COOP_BUILD_SHA "unknown")
endif()
set_source_files_properties(src/main.c PROPERTIES COMPILE_DEFINITIONS "COOP_BUILD_SHA=\"${COOP_BUILD_SHA}\"")

target_include_directories(arcanum-ce PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(arcanum-ce PUBLIC
    ${TIG_LIBRARY}
    enet
)
if(ARCANUM_UPNP)
    target_link_libraries(arcanum-ce PRIVATE miniupnpc)
    target_compile_definitions(arcanum-ce PRIVATE ARCANUM_UPNP=1)
endif()

# Headless transport loopback test (no game data / SDL window required).
add_executable(tig_net_loopback_test
    "tests/tig_net_loopback_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
)
target_include_directories(tig_net_loopback_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(tig_net_loopback_test PRIVATE
    enet
)
# Minimal tig_debug_printf stub for the test binary (avoid linking full tig/SDL).
target_compile_definitions(tig_net_loopback_test PRIVATE TIG_NET_TEST_STUB_DEBUG=1)
if(WIN32)
    target_link_libraries(tig_net_loopback_test PRIVATE ws2_32 winmm)
    target_link_libraries(arcanum-ce PUBLIC ws2_32 winmm)
endif()

enable_testing()
add_test(NAME tig_net_loopback COMMAND tig_net_loopback_test)

if(NOT WIN32 AND NOT ANDROID)
    # tig file helpers against the real tig library (native paths only).
    add_executable(tig_file_empty_dir_test "tests/tig_file_empty_dir_test.c")
    target_link_libraries(tig_file_empty_dir_test PRIVATE ${TIG_LIBRARY})
    add_test(NAME tig_file_empty_dir COMMAND tig_file_empty_dir_test)
    # PT68: coop role overlay (one game folder, separate host/client saves).
    add_executable(tig_file_overlay_test "tests/tig_file_overlay_test.c")
    target_link_libraries(tig_file_overlay_test PRIVATE ${TIG_LIBRARY})
    add_test(NAME tig_file_overlay COMMAND tig_file_overlay_test)
    # PT53c: overflowing partial debug line (no OOB read, no stalled log).
    add_executable(tig_debug_long_line_test "tests/tig_debug_long_line_test.c")
    target_link_libraries(tig_debug_long_line_test PRIVATE ${TIG_LIBRARY})
    add_test(NAME tig_debug_long_line COMMAND tig_debug_long_line_test)
endif()

add_executable(coop_lite_policy_test
    "tests/coop_lite_policy_test.c"
    "src/net/coop_upnp.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_policy_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_policy_test PRIVATE enet)
target_compile_definitions(coop_lite_policy_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_lite_policy_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME coop_lite_policy COMMAND coop_lite_policy_test)

add_executable(coop_story_test
    "tests/coop_story_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_story_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_story_test PRIVATE enet)
target_compile_definitions(coop_story_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_story_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME coop_story COMMAND coop_story_test)

add_executable(coop_dialog_test
    "tests/coop_dialog_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_dialog_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_dialog_test PRIVATE enet)
target_compile_definitions(coop_dialog_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_dialog_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME coop_dialog COMMAND coop_dialog_test)

add_executable(coop_menu_test
    "tests/coop_menu_test.c"
    "src/game/coop_menu.c"
)
target_include_directories(coop_menu_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
add_test(NAME coop_menu COMMAND coop_menu_test)

add_executable(coop_lite_dialog_filter_test
    "tests/coop_lite_dialog_filter_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_dialog_filter_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_dialog_filter_test PRIVATE enet)
target_compile_definitions(coop_lite_dialog_filter_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_lite_dialog_filter_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME coop_lite_dialog_filter COMMAND coop_lite_dialog_filter_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(coop_lite_session_sync_test
    "tests/coop_lite_session_sync_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_session_sync_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_session_sync_test PRIVATE enet)
target_compile_definitions(coop_lite_session_sync_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_lite_session_sync_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME coop_lite_session_sync COMMAND coop_lite_session_sync_test)

add_executable(coop_lite_net_hardening_test
    "tests/coop_lite_net_hardening_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_net_hardening_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_net_hardening_test PRIVATE enet)
target_compile_definitions(coop_lite_net_hardening_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_lite_net_hardening_test PRIVATE ws2_32 winmm)
endif()
if(NOT WIN32)
    add_test(NAME coop_lite_net_hardening COMMAND coop_lite_net_hardening_test)
endif()


# PT53d: transport-level receive-handler fuzz (host via loopback, client role direct).
add_executable(coop_lite_rx_fuzz_test
    "tests/coop_lite_rx_fuzz_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_rx_fuzz_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_rx_fuzz_test PRIVATE enet)
target_compile_definitions(coop_lite_rx_fuzz_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(NOT WIN32)
    add_test(NAME coop_lite_rx_fuzz COMMAND coop_lite_rx_fuzz_test)
    set_tests_properties(coop_lite_rx_fuzz PROPERTIES ENVIRONMENT "COOP_RX_FUZZ_SECONDS=3")
endif()

# PT52j: validator fuzz + notice text + 8-player pure logic.
add_executable(coop_lite_fuzz8_test
    "tests/coop_lite_fuzz8_test.c"
    "src/net/coop_upnp.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_fuzz8_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_fuzz8_test PRIVATE enet)
target_compile_definitions(coop_lite_fuzz8_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(WIN32)
    target_link_libraries(coop_lite_fuzz8_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME coop_lite_fuzz8 COMMAND coop_lite_fuzz8_test)

# PT52k: 7 raw-ENet clients vs a tig_net host (storms, malformed packets).
add_executable(coop_lite_net8_test
    "tests/coop_lite_net8_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_net8_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_net8_test PRIVATE enet)
target_compile_definitions(coop_lite_net8_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(NOT WIN32)
    add_test(NAME coop_lite_net8 COMMAND coop_lite_net8_test)
endif()

# PT55: 8-slot stress / edge cases over ENet loopback.
add_executable(coop_lite_stress8_test
    "tests/coop_lite_stress8_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
    "src/game/coop_lite.c"
)
target_include_directories(coop_lite_stress8_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(coop_lite_stress8_test PRIVATE enet)
target_compile_definitions(coop_lite_stress8_test PRIVATE
    TIG_NET_TEST_STUB_DEBUG=1
    COOP_LITE_TEST_STUBS=1
)
if(NOT WIN32)
    add_test(NAME coop_lite_stress8 COMMAND coop_lite_stress8_test)
    set_tests_properties(coop_lite_stress8 PROPERTIES TIMEOUT 300)
endif()

# SDL HID on Android requires C++ linker.
if(ANDROID)
    enable_language(CXX)
endif()

if(APPLE)
    if(IOS)
        set(RESOURCES
            "ios/AppIcon.xcassets"
            "ios/LaunchScreen.storyboard"
        )

        target_sources(arcanum-ce PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(arcanum-ce PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/ios/Info.plist"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.arcanum-ce"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME arcanum-ce)
        set(MACOSX_BUNDLE_DISPLAY_NAME "Arcanum")
    else()
        set(RESOURCES
            "macos/arcanum-ce.icns"
        )

        target_sources(arcanum-ce PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(arcanum-ce PROPERTIES
            OUTPUT_NAME "Arcanum Community Edition"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/Info.plist"
            XCODE_ATTRIBUTE_EXECUTABLE_NAME "arcanum-ce"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.arcanum-ce"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME "Arcanum: Community Edition")
        set(MACOSX_BUNDLE_ICON_FILE "arcanum-ce.icns")
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.alexbatalov.arcanum-ce")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1")
endif()

if(APPLE)
    if(IOS)
        install(TARGETS arcanum-ce DESTINATION "Payload")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
        set(CPACK_PACKAGE_FILE_NAME "arcanum-ce")
        set(CPACK_ARCHIVE_FILE_EXTENSION "ipa")
    else()
        install(TARGETS arcanum-ce DESTINATION .)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK ON)
        set(CPACK_PACKAGE_FILE_NAME "Arcanum Community Edition")
    endif()

    include(CPack)
endif()

# PT58: transport abstraction (loopback fake + Steam backend on a fake flat API).
add_executable(tig_net_transport_test
    "tests/tig_net_transport_test.c"
    "src/net/tig_net.c"
    "src/net/tig_net_enet.c"
    "src/net/tig_net_steam.c"
)
target_include_directories(tig_net_transport_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/first_party/tig/include"
)
target_link_libraries(tig_net_transport_test PRIVATE enet)
target_compile_definitions(tig_net_transport_test PRIVATE TIG_NET_TEST_STUB_DEBUG=1)
if(WIN32)
    target_link_libraries(tig_net_transport_test PRIVATE ws2_32 winmm)
endif()
add_test(NAME tig_net_transport COMMAND tig_net_transport_test)

# PT58: co-op launcher (Win32 GUI, Windows builds only).
if(WIN32)
    add_executable(arcanum-coop-launcher WIN32 "tools/launcher/launcher.c")
    target_link_libraries(arcanum-coop-launcher PRIVATE ws2_32 comctl32 shell32 wininet bcrypt)
    # PT62: build sha + protocol go into the bug report.
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/net/tig_net.h" COOP_PROTO_LINE REGEX "^#define TIG_NET_PROTOCOL ")
    string(REGEX REPLACE ".*TIG_NET_PROTOCOL +([0-9]+).*" "\\1" COOP_PROTO_NUM "${COOP_PROTO_LINE}")
    target_compile_definitions(arcanum-coop-launcher PRIVATE COOP_BUILD_SHA="${COOP_BUILD_SHA}" COOP_PROTOCOL=${COOP_PROTO_NUM})
    if(ARCANUM_UPNP)
        target_link_libraries(arcanum-coop-launcher PRIVATE miniupnpc)
        target_compile_definitions(arcanum-coop-launcher PRIVATE LAUNCHER_UPNP=1)
    endif()
    if(MINGW)
        target_link_options(arcanum-coop-launcher PRIVATE -static -static-libgcc)
    endif()
endif()
