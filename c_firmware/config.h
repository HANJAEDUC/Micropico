#ifndef CONFIG_H
#define CONFIG_H

#include "version.h"

// ----------------------------------------------------
// 0. Flash Memory Map (2MB & 4MB Universal Safe Flash Map)
// ----------------------------------------------------
#define FLASH_OTA_STAGING_OFFSET (512 * 1024)                 // 512KB Offset (Staging Bank for Pico 2 OTA, size 384KB)
#define FLASH_OTA_META_OFFSET    (896 * 1024)                 // 896KB Offset (Pico 2 OTA Metadata Header, 4KB)
#define FLASH_INFO_OFFSET        (900 * 1024)                 // 900KB Offset (Realtek Persistent Metadata, 4KB)
#define FLASH_RTD_BIN_OFFSET     (1024 * 1024)                // 1.0MB Offset (Storage for Realtek Scaler Binary)
#define FLASH_RTD_MAX_SIZE       (2560 * 1024)                // Max 2.5MB (2560KB) for Realtek Firmware (on 4MB Flash)
#define FLASH_OTA_MAGIC          0x4F544131                   // "OTA1" Magic Header Flag
#define FLASH_OTA_MAX_SIZE       (384 * 1024)                 // Max 384KB per Pico 2 Firmware Binary (Actual is ~110KB)

// ----------------------------------------------------
// 1. Hardware Pin Configurations (W5500-EVB-Pico2)
// ----------------------------------------------------
#define SPI_PORT        spi0
#define PIN_MISO        16
#define PIN_CS          17
#define PIN_SCK         18
#define PIN_MOSI        19
#define PIN_RST         20
#define PIN_INT         21

#define PIN_LED         PICO_DEFAULT_LED_PIN

// RS-232 / Realtek Scaler UART Configuration (UART0 & UART1)
#define UART_ID         uart0
#define UART_BAUD_RATE  9600
#define PIN_UART_TX     0
#define PIN_UART_RX     1
#define PIN_UART0_TX    0
#define PIN_UART0_RX    1
#define PIN_UART1_TX    4
#define PIN_UART1_RX    5

// Realtek RTD Scaler ISP Channel 2 (I2C1: GP2:SCL, GP3:SDA)
#define PIN_RTD_SCL     2
#define PIN_RTD_SDA     3

// ----------------------------------------------------
// 2. Network Parameters
// ----------------------------------------------------
#define STATIC_IP_ADDR  {192, 168, 10, 177}
#define SUBNET_MASK     {255, 255, 255, 0}
#define GATEWAY_ADDR    {192, 168, 10, 1}
#define MAC_ADDR        {0x00, 0x08, 0xDC, 0x23, 0x50, 0x0A}

#define HTTP_PORT       80
#define SNMP_PORT       161

#endif // CONFIG_H
