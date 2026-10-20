# ps1-writer, PlayStation side (PSn00bSDK)

cmake_minimum_required(VERSION 3.21)

project(
	ps1writer
	LANGUAGES    C ASM
	VERSION      1.0.0
	DESCRIPTION  "Text editor for the PlayStation (USB keyboard + SD card via a Pico)"
)

psn00bsdk_add_executable(
	ps1writer GPREL
	main.c editor.c doc.c link.c sio.c
)
target_include_directories(ps1writer PRIVATE ${PROJECT_SOURCE_DIR}/../common)
target_compile_options(ps1writer PRIVATE -O2 -Wall)

# controller-bus clock: 33.8688 MHz / value (0x88 = 250 kHz, the stock speed)
set(SIO_BAUD_RELOAD "" CACHE STRING "bus clock divider, e.g. 0x44 for 500 kHz")
if(SIO_BAUD_RELOAD)
	target_compile_definitions(ps1writer PRIVATE SIO_BAUD_RELOAD=${SIO_BAUD_RELOAD})
endif()

psn00bsdk_add_cd_image(
	iso          # target name
	ps1writer    # output: ps1writer.bin + ps1writer.cue
	iso.xml
	DEPENDS ps1writer system.cnf
)
