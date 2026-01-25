cmake_minimum_required(VERSION 3.13)

# Stop Pico SDK from trying to fetch/build picotool on this machine
set(PICO_SDK_FETCH_PICOTOOL OFF)

include(pico_sdk_import.cmake)

project(erb_standalone_mmu C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(erb_standalone_mmu
    main.c
)

target_link_libraries(erb_standalone_mmu pico_stdlib)

pico_enable_stdio_usb(erb_standalone_mmu 1)
pico_enable_stdio_uart(erb_standalone_mmu 0)

# pico_add_extra_outputs(erb_standalone_mmu)

