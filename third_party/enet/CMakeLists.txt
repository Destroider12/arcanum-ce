cmake_minimum_required(VERSION 3.13)
project(enet LANGUAGES C)

set(ENET_SRCS
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
)
if(WIN32)
    list(APPEND ENET_SRCS win32.c)
else()
    list(APPEND ENET_SRCS unix.c)
endif()

add_library(enet STATIC ${ENET_SRCS})

target_include_directories(enet PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
    target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    include(CheckFunctionExists)
    foreach(_fn fcntl poll getaddrinfo getnameinfo gethostbyname_r gethostbyaddr_r inet_pton inet_ntop)
        string(TOUPPER "${_fn}" _FN)
        check_function_exists(${_fn} HAS_${_FN})
        if(HAS_${_FN})
            target_compile_definitions(enet PRIVATE HAS_${_FN}=1)
        endif()
    endforeach()
endif()

if(MSVC)
    target_compile_options(enet PRIVATE /W3)
else()
    target_compile_options(enet PRIVATE -w)
endif()
