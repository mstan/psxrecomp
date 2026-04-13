cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

add_executable(psx-runtime
    src/main.cpp
    src/memory.c
    src/gpu.c
    src/dma.c
    src/timers.c
    src/interrupts.c
    src/sio.c
    src/memcard.c
    src/debug_server.c
    src/traps.c
    ../generated/SCPH1001_full.c
    ../generated/SCPH1001_dispatch.c
)

target_include_directories(psx-runtime PRIVATE
    include
)

# SDL2 for display presentation.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
target_include_directories(psx-runtime PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(psx-runtime PRIVATE ${SDL2_LIBRARIES})

# Winsock2 for TCP debug server.
if(WIN32 OR MINGW)
    target_link_libraries(psx-runtime PRIVATE ws2_32)
endif()

# Large stack for deep call chains during BIOS execution.
if(MINGW OR WIN32)
    target_link_options(psx-runtime PRIVATE -Wl,--stack,67108864)
endif()
