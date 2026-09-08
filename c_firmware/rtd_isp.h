#ifndef RTD_ISP_H
#define RTD_ISP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RTD_I2C_ADDR        0x4A
#define RTD_ISP_CHUNK_SIZE  64    // 64 Bytes per I2C write burst

typedef enum {
    RTD_STATUS_IDLE = 0,
    RTD_STATUS_ERASING,
    RTD_STATUS_WRITING,
    RTD_STATUS_VERIFYING,
    RTD_STATUS_SUCCESS,
    RTD_STATUS_ERROR
} rtd_flash_status_t;

typedef struct {
    rtd_flash_status_t status;
    uint32_t total_bytes;
    uint32_t processed_bytes;
    uint8_t progress_percent;
    uint8_t hardware_crc;
    char message[128];
    bool is_busy;
} rtd_flash_progress_t;

typedef struct {
    bool connected;
    uint8_t mfg_id;
    uint8_t mem_type;
    uint8_t capacity_id;
    uint32_t flash_size_bytes;
    char mfg_name[32];
    char flash_model[64];
    char flash_size_str[32];
    char status_msg[128];
} rtd_chip_info_t;

// Initialize Hardware I2C (Channel 2: GP2:SCL, GP3:SDA, 400kHz)
void rtd_isp_init(void);

// Ping Realtek Scaler I2C (0x4A) to verify hardware wiring
bool rtd_isp_ping(void);

// Read Scaler and SPI Flash Chip Identification (JEDEC ID 0x9F / REMS 0x90)
bool rtd_isp_read_chip_info(rtd_chip_info_t *info);

// Enter / Exit ISP Programming Mode
bool rtd_isp_enter(void);
bool rtd_isp_exit(void);

// Low-level register operations
bool rtd_isp_write_reg(uint8_t reg, uint8_t val);
int rtd_isp_read_reg(uint8_t reg);
bool rtd_isp_read_burst(uint8_t reg, uint8_t *rx_buf, size_t len);

// Erase 64KB Flash Block on Realtek Scaler
bool rtd_isp_erase_block(uint8_t block_idx);

// Write up to 128-byte chunk to Scaler via 0x70 Data Port
bool rtd_isp_write_chunk(uint32_t addr, const uint8_t *chunk, size_t length);

// Read Scaler Internal Hardware CRC
int rtd_isp_get_crc(uint32_t end_addr);

// Run full ISP Flash programming from Pico 2 Flash storage buffer
bool rtd_isp_flash_from_storage(uint32_t storage_flash_offset, uint32_t fw_size);

// Get current progress and status
const rtd_flash_progress_t* rtd_isp_get_progress(void);

#endif // RTD_ISP_H
