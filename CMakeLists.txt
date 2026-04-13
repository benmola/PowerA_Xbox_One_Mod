cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico)

include(pico_sdk_import.cmake)

project(pico_gamepad_host C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize Pico SDK
pico_sdk_init()

# Include the tusb_xinput driver (must be cloned into lib/tusb_xinput)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/tusb_xinput xinput_host)

add_executable(pico_gamepad_host
    main.c
)

target_include_directories(pico_gamepad_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Set up standard output to UART and redirect it (GP0/GP1 are standard for debugging)
target_compile_definitions(pico_gamepad_host PRIVATE
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

target_link_libraries(pico_gamepad_host PRIVATE
    pico_stdlib
    hardware_spi
    tinyusb_host
    tinyusb_board
    xinput_host
)

# Disable native USB stdio (we need USB host mode for the controller)
pico_enable_stdio_usb(pico_gamepad_host 0)
pico_enable_stdio_uart(pico_gamepad_host 1)

# Generate the .uf2 file
pico_add_extra_outputs(pico_gamepad_host)
