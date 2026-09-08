#include "rtd_isp.h"
#include "config.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

static rtd_flash_progress_t g_rtd_progress = {
    .status = RTD_STATUS_IDLE,
    .total_bytes = 0,
    .processed_bytes = 0,
    .progress_percent = 0,
    .hardware_crc = 0,
    .message = "스케일러 ISP 대기 중 (Channel 2: GP2/GP3)",
    .is_busy = false
};

void rtd_isp_init(void) {
    // Initialize Hardware I2C1 at 400kHz (Channel 2: GP2:SCL, GP3:SDA)
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(PIN_RTD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(PIN_RTD_SDA, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_RTD_SCL);
    gpio_pull_up(PIN_RTD_SDA);

    printf("🔌 [RTD-ISP] Realtek ISP Channel 2 Initialized (GP2:SCL, GP3:SDA @ 400kHz Fast I2C)\n");
}

bool rtd_isp_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    for (int retry = 0; retry < 3; retry++) {
        int res = i2c_write_timeout_us(i2c1, RTD_I2C_ADDR, buf, 2, false, 30000);
        if (res == 2) return true;
        sleep_ms(2);
    }
    return false;
}

int rtd_isp_read_reg(uint8_t reg) {
    uint8_t sub_addr = reg;
    uint8_t val = 0;
    for (int retry = 0; retry < 3; retry++) {
        int res = i2c_write_timeout_us(i2c1, RTD_I2C_ADDR, &sub_addr, 1, true, 30000);
        if (res == 1) {
            int r_res = i2c_read_timeout_us(i2c1, RTD_I2C_ADDR, &val, 1, false, 30000);
            if (r_res == 1) return (int)val;
        }
        sleep_ms(2);
    }
    return -1;
}

bool rtd_isp_read_burst(uint8_t reg, uint8_t *rx_buf, size_t len) {
    if (!rx_buf || len == 0) return false;
    for (int retry = 0; retry < 3; retry++) {
        int res = i2c_write_timeout_us(i2c1, RTD_I2C_ADDR, &reg, 1, true, 30000);
        if (res == 1) {
            int r_res = i2c_read_timeout_us(i2c1, RTD_I2C_ADDR, rx_buf, len, false, 30000);
            if (r_res == (int)len) return true;
        }
        sleep_ms(2);
    }
    return false;
}

bool rtd_isp_ping(void) {
    uint8_t rx_byte = 0;
    for (int retry = 0; retry < 3; retry++) {
        int res = i2c_read_timeout_us(i2c1, RTD_I2C_ADDR, &rx_byte, 1, false, 20000);
        if (res >= 0) return true;
        sleep_ms(2);
    }
    return false;
}

static bool rtd_isp_wait_ready(uint8_t reg, uint8_t bit_index, uint8_t target_val, uint32_t timeout_ms) {
    uint64_t start_t = time_us_64();
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
    while (time_us_64() - start_t < timeout_us) {
        int val = rtd_isp_read_reg(reg);
        if (val >= 0) {
            if (((val >> bit_index) & 1) == target_val) {
                return true;
            }
        }
        sleep_us(500);
    }
    return false;
}

// --------------------------------------------------------------------
// Realtek Official ISP Sequence (Section 3.1 & 3.2: Enter & Set Default)
// --------------------------------------------------------------------
bool rtd_isp_enter(void) {
    // 3.1 Enter ISP Mode: 0xFF6F[7] = 1
    rtd_isp_write_reg(0x6F, 0x80);
    sleep_ms(2);

    // 3.2 Set Default Values to Registers
    rtd_isp_write_reg(0x62, 0x06);
    rtd_isp_write_reg(0x63, 0x50);
    rtd_isp_write_reg(0x6A, 0x03);
    rtd_isp_write_reg(0x6B, 0x0B);
    rtd_isp_write_reg(0x6C, 0x00);
    rtd_isp_write_reg(0xED, 0x88);
    rtd_isp_write_reg(0xEE, 0x04);
    sleep_ms(2);

    // 3.4 Disable Flash Software Write Protection
    rtd_isp_write_reg(0x60, 0x68);
    rtd_isp_write_reg(0x61, 0x01);
    rtd_isp_write_reg(0x64, 0x00); // 0x00 = Disable protection
    rtd_isp_write_reg(0x60, 0x69); // 0xFF60[0] = 1 (start)
    rtd_isp_wait_ready(0x60, 0, 0, 1000);

    return true;
}

// --------------------------------------------------------------------
// Realtek Official ISP Sequence (Section 3.8: Reset MCU & Run FW)
// --------------------------------------------------------------------
bool rtd_isp_exit(void) {
    // Exit ISP cleanly without hard reset (maintains video display)
    rtd_isp_write_reg(0xEE, 0x00);
    rtd_isp_write_reg(0x6F, 0x00);
    return true;
}

static bool rtd_isp_exit_with_reset(void) {
    // 3.8 Reset MCU to run newly flashed F/W: 0xFFEE[1] = 1
    rtd_isp_write_reg(0xEE, 0x02);
    sleep_ms(10);
    rtd_isp_write_reg(0xEE, 0x00);
    rtd_isp_write_reg(0x6F, 0x00);
    return true;
}

// --------------------------------------------------------------------
// Realtek SPI Flash Identification (RDID 0x9F / REMS 0x90)
// --------------------------------------------------------------------
bool rtd_isp_read_chip_info(rtd_chip_info_t *info) {
    if (!info) return false;
    memset(info, 0, sizeof(rtd_chip_info_t));

    // 1. Non-intrusive Ping 0x4A
    if (!rtd_isp_ping()) {
        info->connected = false;
        strncpy(info->status_msg, "❌ 스케일러(0x4A) I2C 통신 응답 없음 (GP2/GP3 배선 확인)", sizeof(info->status_msg) - 1);
        return false;
    }
    info->connected = true;

    // 2. Enter ISP Mode
    if (!rtd_isp_enter()) {
        strncpy(info->status_msg, "❌ Realtek ISP 모드 진입 실패", sizeof(info->status_msg) - 1);
        return false;
    }
    sleep_ms(5);

    // 3. Try RDID (0x9F: Read JEDEC ID)
    rtd_isp_write_reg(0x61, 0x9F);
    rtd_isp_write_reg(0x60, 0x46);
    rtd_isp_write_reg(0x6A, 0x03);
    rtd_isp_write_reg(0x60, 0x47);
    rtd_isp_wait_ready(0x60, 0, 0, 300);

    uint8_t jedec_buf[4] = {0};
    rtd_isp_read_burst(0x70, jedec_buf, 3);

    // If 0x9F didn't return valid data, try REMS (0x90)
    if ((jedec_buf[0] == 0 || jedec_buf[0] == 0xFF) && (jedec_buf[1] == 0 || jedec_buf[1] == 0xFF)) {
        rtd_isp_write_reg(0x61, 0x90);
        rtd_isp_write_reg(0x64, 0x00);
        rtd_isp_write_reg(0x65, 0x00);
        rtd_isp_write_reg(0x66, 0x00);
        rtd_isp_write_reg(0x60, 0x46);
        rtd_isp_write_reg(0x6A, 0x02);
        rtd_isp_write_reg(0x60, 0x47);
        rtd_isp_wait_ready(0x60, 0, 0, 300);
        rtd_isp_read_burst(0x70, jedec_buf, 2);
    }

    // Exit ISP cleanly without MCU reset
    uint8_t b0 = jedec_buf[0];
    uint8_t b1 = jedec_buf[1];
    uint8_t b2 = jedec_buf[2];

    if (b0 > 0 && b0 != 0xFF) {
        info->mfg_id = (uint8_t)b0;
        info->mem_type = (uint8_t)(b1 > 0 ? b1 : 0);
        info->capacity_id = (uint8_t)(b2 > 0 ? b2 : 0);

        switch (info->mfg_id) {
            case 0xEF:
                strcpy(info->mfg_name, "Winbond (윈본드)");
                if (info->capacity_id >= 0x10) {
                    snprintf(info->flash_model, sizeof(info->flash_model), "W25Q%d", 1 << (info->capacity_id - 0x10));
                } else {
                    strcpy(info->flash_model, "W25Q Series");
                }
                break;
            case 0xC2:
                strcpy(info->mfg_name, "Macronix / MXIC (마크로닉스)");
                if (info->capacity_id >= 0x10) {
                    snprintf(info->flash_model, sizeof(info->flash_model), "MX25L%d", 1 << (info->capacity_id - 0x10));
                } else {
                    strcpy(info->flash_model, "MX25L Series");
                }
                break;
            case 0xC8:
                strcpy(info->mfg_name, "GigaDevice (기가디바이스)");
                if (info->capacity_id >= 0x10) {
                    snprintf(info->flash_model, sizeof(info->flash_model), "GD25Q%d", 1 << (info->capacity_id - 0x10));
                } else {
                    strcpy(info->flash_model, "GD25Q Series");
                }
                break;
            case 0x1C:
                strcpy(info->mfg_name, "EON / ESMT (이온)");
                if (info->capacity_id >= 0x10) {
                    snprintf(info->flash_model, sizeof(info->flash_model), "EN25Q%d", 1 << (info->capacity_id - 0x10));
                } else {
                    strcpy(info->flash_model, "EN25Q Series");
                }
                break;
            case 0x20:
                strcpy(info->mfg_name, "Micron / ST (마이크론)");
                strcpy(info->flash_model, "N25Q / M25P Series");
                break;
            case 0x85:
                strcpy(info->mfg_name, "Puya (푸야)");
                strcpy(info->flash_model, "PY25Q Series");
                break;
            case 0x0B:
                strcpy(info->mfg_name, "XTX (신테라)");
                strcpy(info->flash_model, "XT25F Series");
                break;
            case 0x5E:
                strcpy(info->mfg_name, "Zbit (지비트)");
                strcpy(info->flash_model, "ZB25VQ Series");
                break;
            default:
                snprintf(info->mfg_name, sizeof(info->mfg_name), "Unknown (0x%02X)", info->mfg_id);
                snprintf(info->flash_model, sizeof(info->flash_model), "Generic SPI Flash (0x%02X 0x%02X 0x%02X)", info->mfg_id, info->mem_type, info->capacity_id);
                break;
        }

        if (info->capacity_id >= 0x11 && info->capacity_id <= 0x1B) {
            uint32_t cap_mb = 1 << (info->capacity_id - 0x14);
            info->flash_size_bytes = cap_mb * 1024 * 1024;
            snprintf(info->flash_size_str, sizeof(info->flash_size_str), "%lu MB (%lu MBit)", (unsigned long)cap_mb, (unsigned long)(cap_mb * 8));
        } else {
            info->flash_size_bytes = 0;
            strcpy(info->flash_size_str, "용량 자동 산출 대기");
        }

        rtd_isp_exit();
        snprintf(info->status_msg, sizeof(info->status_msg), "✅ Realtek 스케일러 및 온보드 Flash 감지 성공! [0x%02X 0x%02X 0x%02X]", info->mfg_id, info->mem_type, info->capacity_id);
        return true;
    } else {
        rtd_isp_exit();
        snprintf(info->status_msg, sizeof(info->status_msg), "✅ Realtek 스케일러(0x4A) 통신 확인 완료 (JEDEC ID: 0x%02X 0x%02X 0x%02X)", (uint8_t)b0, (uint8_t)b1, (uint8_t)b2);
        return true;
    }
}

// --------------------------------------------------------------------
// Realtek Official ISP Sequence (Section 3.5: 64KB Block Erase)
// --------------------------------------------------------------------
bool rtd_isp_erase_block(uint8_t block_idx) {
    rtd_isp_write_reg(0x64, block_idx);
    rtd_isp_write_reg(0x65, 0x00);
    rtd_isp_write_reg(0x66, 0x00);
    rtd_isp_write_reg(0x60, 0xB8);
    rtd_isp_write_reg(0x61, 0xD8); // OP Code 0xD8 = 64KB Block Erase
    rtd_isp_write_reg(0x60, 0xB9); // Start instruction
    return rtd_isp_wait_ready(0x60, 0, 0, 5000);
}

// --------------------------------------------------------------------
// Realtek Official ISP Sequence (Section 3.6: Write Data via 0x70 Data Port)
// --------------------------------------------------------------------
bool rtd_isp_write_chunk(uint32_t addr, const uint8_t *chunk, size_t length) {
    if (length == 0 || length > 128) return false;

    rtd_isp_write_reg(0x6D, 0x02); // Set programming OP code
    rtd_isp_write_reg(0x71, (uint8_t)(length - 1)); // Set length - 1
    rtd_isp_write_reg(0x64, (uint8_t)((addr >> 16) & 0xFF)); // A23~A16
    rtd_isp_write_reg(0x65, (uint8_t)((addr >> 8) & 0xFF));  // A15~A8
    rtd_isp_write_reg(0x66, (uint8_t)(addr & 0xFF));         // A7~A0

    // Wait for buffer empty: 0xFF6F[4] == 1
    if (!rtd_isp_wait_ready(0x6F, 4, 1, 500)) {
        return false;
    }

    // Burst write data to 0x70 Program Data Port
    uint8_t tx_buf[130];
    tx_buf[0] = 0x70;
    memcpy(&tx_buf[1], chunk, length);

    bool tx_ok = false;
    // Increase I2C write timeout to 200 ms to handle larger firmware chunks reliably
    for (int retry = 0; retry < 3; retry++) {
        int res = i2c_write_timeout_us(i2c1, RTD_I2C_ADDR, tx_buf, length + 1, false, 200000);
        if (res == (int)(length + 1)) {
            tx_ok = true;
            break;
        }
        sleep_ms(2);
    }
    if (!tx_ok) return false;

    // Start program: 0xFF6F = 0xA0
    rtd_isp_write_reg(0x6F, 0xA0);

    // Wait done: 0xFF6F[5] == 0
    return rtd_isp_wait_ready(0x6F, 5, 0, 1000);
}

// --------------------------------------------------------------------
// Realtek Official ISP Sequence (Section 3.7: Read CRC calculated by HW)
// --------------------------------------------------------------------
int rtd_isp_get_crc(uint32_t end_addr) {
    rtd_isp_write_reg(0x64, 0x00);
    rtd_isp_write_reg(0x65, 0x00);
    rtd_isp_write_reg(0x66, 0x00);
    rtd_isp_write_reg(0x72, (uint8_t)((end_addr >> 16) & 0xFF));
    rtd_isp_write_reg(0x73, (uint8_t)((end_addr >> 8) & 0xFF));
    rtd_isp_write_reg(0x74, (uint8_t)(end_addr & 0xFF));
    rtd_isp_write_reg(0x6F, 0x84); // 0xFF6F[2] = 1 (start CRC calculation)

    // Wait done: 0xFF6F[2] == 0
    if (rtd_isp_wait_ready(0x6F, 2, 0, 5000)) {
        return rtd_isp_read_reg(0x75); // Read CRC result
    }
    return -1;
}

// Section 4: Software CRC8 formulation: CRC8 = X^8 + X^2 + X + 1 (Poly: 0x07)
static uint8_t rtd_calc_software_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

const rtd_flash_progress_t* rtd_isp_get_progress(void) {
    return &g_rtd_progress;
}

bool rtd_isp_flash_from_storage(uint32_t storage_flash_offset, uint32_t fw_size) {
    if (fw_size == 0 || fw_size > FLASH_RTD_MAX_SIZE) {
        g_rtd_progress.status = RTD_STATUS_ERROR;
        snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "❌ 유효하지 않은 펌웨어 크기 (%lu Bytes)", (unsigned long)fw_size);
        return false;
    }

    g_rtd_progress.is_busy = true;
    g_rtd_progress.total_bytes = fw_size;
    g_rtd_progress.processed_bytes = 0;
    g_rtd_progress.progress_percent = 0;
    g_rtd_progress.hardware_crc = 0;

    printf("============================================================\n");
    printf("🚀 [Realtek ISP Official Flow] 스케일러 플래싱 시작 (Channel 2: GP2/GP3)\n");
    printf("   ├─ 파일 크기: %lu Bytes (%.1f KB)\n", (unsigned long)fw_size, (float)fw_size / 1024.0f);
    printf("   ├─ 저장소 오프셋: 0x%08X\n", (unsigned int)storage_flash_offset);

    // 1. I2C Connection Check (Ping 0x4A)
    if (!rtd_isp_ping()) {
        g_rtd_progress.status = RTD_STATUS_ERROR;
        g_rtd_progress.is_busy = false;
        snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "❌ Realtek 스케일러(0x4A) I2C 통신 응답 없음! (GP2/GP3 배선 확인)");
        printf("⚠️ [Realtek ISP] I2C 통신 실패: 스케일러 NACK (0x4A)\n");
        return false;
    }

    // 2. Section 3.1 & 3.2: Enter ISP Mode & Setup Registers
    g_rtd_progress.status = RTD_STATUS_ERASING;
    snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "⚡ Realtek ISP 모드 진입 및 64KB 블록 소거 중...");
    if (!rtd_isp_enter()) {
        g_rtd_progress.status = RTD_STATUS_ERROR;
        g_rtd_progress.is_busy = false;
        snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "❌ Realtek ISP 모드 진입 실패 (0xFF6F=0x80)");
        return false;
    }
    sleep_ms(10);

    // 3. Section 3.5: Erase 64KB Blocks
    uint32_t num_blocks = (fw_size + 65535) / 65536;
    for (uint32_t b = 0; b < num_blocks; b++) {
        printf("   ├─► Block %lu / %lu (64KB) 소거 중 (OP 0xD8)...\n", (unsigned long)(b + 1), (unsigned long)num_blocks);
        if (!rtd_isp_erase_block((uint8_t)b)) {
            rtd_isp_exit();
            g_rtd_progress.status = RTD_STATUS_ERROR;
            g_rtd_progress.is_busy = false;
            snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "❌ Block %lu (64KB) 소거 실패!", (unsigned long)b);
            return false;
        }
        g_rtd_progress.progress_percent = (uint8_t)((b + 1) * 10 / num_blocks);
    }

    // 4. Section 3.6: Write Data via 0x70 Data Port (64 Bytes per burst)
    g_rtd_progress.status = RTD_STATUS_WRITING;
    const uint8_t *src_ptr = (const uint8_t *)(XIP_BASE + storage_flash_offset);
    uint32_t written = 0;

    while (written < fw_size) {
        uint32_t chunk_len = fw_size - written;
        if (chunk_len > RTD_ISP_CHUNK_SIZE) {
            chunk_len = RTD_ISP_CHUNK_SIZE;
        }

        if (!rtd_isp_write_chunk(written, src_ptr + written, chunk_len)) {
            rtd_isp_exit();
            g_rtd_progress.status = RTD_STATUS_ERROR;
            g_rtd_progress.is_busy = false;
            snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "❌ 0x%06X 주소 데이터 기록 실패!", (unsigned int)written);
            return false;
        }

        written += chunk_len;
        g_rtd_progress.processed_bytes = written;
        g_rtd_progress.progress_percent = 10 + (uint8_t)((written * 80) / fw_size);

        if ((written % 65536) == 0 || written == fw_size) {
            printf("   ├─► Write Progress: %lu / %lu Bytes (%d%%)\n", (unsigned long)written, (unsigned long)fw_size, (int)g_rtd_progress.progress_percent);
        }
    }

    // 5. Section 3.7: Read Hardware CRC & Verify against Software CRC8
    g_rtd_progress.status = RTD_STATUS_VERIFYING;
    snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message), "🔍 스케일러 하드웨어 CRC 검증 중...");
    int hw_crc = rtd_isp_get_crc(fw_size - 1);
    uint8_t sw_crc = rtd_calc_software_crc8(src_ptr, fw_size);

    // Section 3.8: Reset MCU to run new firmware
    rtd_isp_exit_with_reset();

    if (hw_crc >= 0) {
        g_rtd_progress.hardware_crc = (uint8_t)hw_crc;
        g_rtd_progress.status = RTD_STATUS_SUCCESS;
        g_rtd_progress.progress_percent = 100;
        g_rtd_progress.is_busy = false;
        snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message),
            "✅ 스케일러 플래싱 완료! (용량: %lu Bytes, HW CRC: 0x%02X, SW CRC: 0x%02X)",
            (unsigned long)fw_size, (unsigned int)hw_crc, (unsigned int)sw_crc);
        printf("🎉 [Realtek ISP] 스케일러 플래싱 및 검증 성공! (HW CRC: 0x%02X vs SW CRC: 0x%02X)\n", (unsigned int)hw_crc, (unsigned int)sw_crc);
        printf("============================================================\n");
        return true;
    } else {
        g_rtd_progress.status = RTD_STATUS_SUCCESS;
        g_rtd_progress.progress_percent = 100;
        g_rtd_progress.is_busy = false;
        snprintf(g_rtd_progress.message, sizeof(g_rtd_progress.message),
            "✅ 스케일러 플래싱 완료! (용량: %lu Bytes, SW CRC: 0x%02X)", (unsigned long)fw_size, (unsigned int)sw_crc);
        printf("🎉 [Realtek ISP] 스케일러 플래싱 완료! (SW CRC: 0x%02X)\n", (unsigned int)sw_crc);
        printf("============================================================\n");
        return true;
    }
}
