cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(w5500_pico2_c_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(w5500_pico2_firmware
    main.c
    w5500_driver.c
    http_server.c
    dhcp_client.c
    snmp_agent.c
    rtd_isp.c
    rs232_handler.c
)

target_include_directories(w5500_pico2_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(w5500_pico2_firmware
    pico_stdlib
    pico_unique_id
    hardware_spi
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_watchdog
    hardware_uart
    hardware_i2c
)

pico_enable_stdio_uart(w5500_pico2_firmware 0)
pico_enable_stdio_usb(w5500_pico2_firmware 0)

pico_add_extra_outputs(w5500_pico2_firmware)
