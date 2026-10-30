cmake_minimum_required(VERSION 3.5.1)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER /home/wang/ZMHT-Tool/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER /home/wang/ZMHT-Tool/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

project(canfd_server)

set(TARGETS "canfd_server")

set(CMAKE_CXX_STANDARD 14)
# Link libstdc++/libgcc statically: the PetaLinux rootfs may not ship
# libstdc++.so.6, which would make the binary fail to start on the board.
set(CMAKE_CXX_FLAGS "-O2 -g -std=c++14 -Wno-error -static-libstdc++ -static-libgcc")

add_definitions(-D_GNU_SOURCE -DLINUX)

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/src/network/
    ${CMAKE_CURRENT_LIST_DIR}/src/canfd/
    ${CMAKE_CURRENT_LIST_DIR}/src/hardware/
)

add_executable(${TARGETS}
    src/main.cpp
    src/network/thread.cpp
    src/network/tcp_socket.cpp
    src/network/sync_send.cpp
    src/network/network_main.cpp
    src/canfd/protocol.cpp
    src/canfd/canfd_codec.cpp
    src/canfd/canfd_hw.cpp
    src/canfd/canfd_service.cpp
    src/canfd/can_tx_queue.cpp
    src/hardware/fifo_engine.cpp
    src/hardware/ringbuffer.c
)

target_link_libraries(${TARGETS}
    pthread
)
