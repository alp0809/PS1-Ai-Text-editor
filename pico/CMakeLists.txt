cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

# Libraries (see README):
#   lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico   SD card + FatFs
#   lib/Pico-PIO-USB                       USB host on GPIO pins
set(FATFS_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    CACHE PATH "Path to no-OS-FatFS-SD-SDIO-SPI-RPi-Pico")
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB
    CACHE PATH "Path to Pico-PIO-USB")

include(pico_sdk_import.cmake)

project(ps1_writer C CXX ASM)
pico_sdk_init()

add_subdirectory(${FATFS_LIB_DIR}/src fatfs_build)

add_executable(ps1_writer
    main.c
    bus_logic.c
    shared.c
    keymap.c
    keyboard.c
    fileserver.c
    ai.c
    vfs_fatfs.c
    hw_config.c
    usb_descriptors.c
)

pico_generate_pio_header(ps1_writer ${CMAKE_CURRENT_LIST_DIR}/sio0_dev.pio)

target_include_directories(ps1_writer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_compile_definitions(ps1_writer PRIVATE PICO_BUILD=1)

target_link_libraries(ps1_writer
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_clocks
    tinyusb_device
    tinyusb_host
    tinyusb_pico_pio_usb
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# log messages go to our own USB serial port, not the SDK's stdio drivers
pico_enable_stdio_usb(ps1_writer 0)
pico_enable_stdio_uart(ps1_writer 0)

pico_add_extra_outputs(ps1_writer)
