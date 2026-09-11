cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(w5500_pico2_c_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

# Common Sources
set(COMMON_SOURCES
    main.c
    w5500_driver.c
    w6300_driver.c
    wizchip_qspi_pio.c
    http_server.c
    dhcp_client.c
    snmp_agent.c
    rtd_isp.c
    rs232_handler.c
)

# Common Libraries
set(COMMON_LIBS
    pico_stdlib
    pico_unique_id
    hardware_spi
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_watchdog
    hardware_uart
    hardware_i2c
)

# 1. W5500 Target
add_executable(w5500_pico2_firmware ${COMMON_SOURCES})
target_include_directories(w5500_pico2_firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(w5500_pico2_firmware PRIVATE TARGET_ETH_CHIP=1)
target_link_libraries(w5500_pico2_firmware ${COMMON_LIBS})
pico_enable_stdio_uart(w5500_pico2_firmware 0)
pico_enable_stdio_usb(w5500_pico2_firmware 1)
pico_add_extra_outputs(w5500_pico2_firmware)

# 2. W6300 Target
add_executable(w6300_pico2_firmware ${COMMON_SOURCES})
target_include_directories(w6300_pico2_firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(w6300_pico2_firmware PRIVATE TARGET_ETH_CHIP=2)
target_link_libraries(w6300_pico2_firmware ${COMMON_LIBS})
pico_enable_stdio_uart(w6300_pico2_firmware 0)
pico_enable_stdio_usb(w6300_pico2_firmware 1)
pico_add_extra_outputs(w6300_pico2_firmware)

