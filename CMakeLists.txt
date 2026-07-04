# SeeTrue Task 1 "Eye Ring" controller firmware
# Build:
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   mkdir build && cd build
#   cmake ..
#   make
# Output seetrue_ring.uf2 gets dragged onto the Pico in BOOTSEL mode.

cmake_minimum_required(VERSION 3.13)

# Pulls the SDK. Copy pico_sdk_import.cmake from the SDK's external/ folder
# into this directory, or point to it directly.
include(pico_sdk_import.cmake)

project(seetrue_ring C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(seetrue_ring
    main.c
)

target_link_libraries(seetrue_ring
    pico_stdlib
    hardware_spi
    hardware_pwm
    hardware_flash
)

# USB CDC for the command interface, UART off.
pico_enable_stdio_usb(seetrue_ring 1)
pico_enable_stdio_uart(seetrue_ring 0)

# Produce .uf2, .bin, .hex, map, etc.
pico_add_extra_outputs(seetrue_ring)
