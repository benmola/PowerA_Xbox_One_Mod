cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico)

include(pico_sdk_import.cmake)

project(pico_gamepad_host C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---- tusb_xinput library (Ryzee119) ----
# Clone into: lib/tusb_xinput
#   git submodule add https://github.com/Ryzee119/tusb_xinput.git lib/tusb_xinput
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/tusb_xinput xinput_host)

add_executable(pico_gamepad_host
    main.c
)

target_include_directories(pico_gamepad_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Override default UART0 pins to GP4/GP5 (GP0/GP1 are used by USB)
target_compile_definitions(pico_gamepad_host PRIVATE
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=4
    PICO_DEFAULT_UART_RX_PIN=5
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

target_link_libraries(pico_gamepad_host PRIVATE
    pico_stdlib
    pico_multicore
    hardware_uart
    tinyusb_host
    tinyusb_board
    xinput_host
)

# Debug output over UART on GP4(TX)/GP5(RX)
# Native USB port is used for USB Host (controller)
pico_enable_stdio_usb(pico_gamepad_host 0)
pico_enable_stdio_uart(pico_gamepad_host 1)

pico_add_extra_outputs(pico_gamepad_host)
