# miniupnpc 2.2.8 (BSD-3-Clause, see LICENSE) — vendored subset, static only.
# Used by src/net/coop_upnp.c for optional UDP port mapping (PT51).
add_library(miniupnpc STATIC
    src/addr_is_reserved.c src/connecthostport.c src/igd_desc_parse.c
    src/minisoap.c src/minissdpc.c src/miniupnpc.c src/miniwget.c src/minixml.c
    src/portlistingparse.c src/receivedata.c src/upnpcommands.c src/upnpdev.c
    src/upnperrors.c src/upnpreplyparse.c)
target_include_directories(miniupnpc PUBLIC include PRIVATE src)
target_compile_definitions(miniupnpc PUBLIC MINIUPNP_STATICLIB
    PRIVATE MINIUPNPC_GET_SRC_ADDR)
if(WIN32)
    target_compile_definitions(miniupnpc PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(miniupnpc PUBLIC ws2_32 iphlpapi)
else()
    target_compile_definitions(miniupnpc PRIVATE MINIUPNPC_SET_SOCKET_TIMEOUT _BSD_SOURCE _DEFAULT_SOURCE)
endif()
