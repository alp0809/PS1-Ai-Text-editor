# kbtest - USB keyboard test for the ps1-writer wiring (no PS1, no SD card).
# Uses the same pins, USB setup and keymap as the real firmware.

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/../pico/lib/Pico-PIO-USB
    CACHE PATH "Path to Pico-PIO-USB")

include(pico_sdk_import.cmake)

project(kbtest C CXX ASM)
pico_sdk_init()

add_executable(kbtest
    main.c
    ../pico/keymap.c
    ../pico/usb_descriptors.c
)

target_include_directories(kbtest PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../pico      # tusb_config.h, keymap.h
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_compile_definitions(kbtest PRIVATE PICO_BUILD=1)

target_link_libraries(kbtest
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_dma
    hardware_clocks
    tinyusb_device
    tinyusb_host
    tinyusb_pico_pio_usb
)

pico_enable_stdio_usb(kbtest 0)
pico_enable_stdio_uart(kbtest 0)
pico_add_extra_outputs(kbtest)
