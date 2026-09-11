#include "http_server.h"
#include "w5500_driver.h"
#include "config.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/psm.h"
#include "hardware/regs/psm.h"
#include "hardware/regs/watchdog.h"
#include "hardware/regs/m33.h"
#include "pico/stdlib.h"
#include "rtd_isp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "rs232_handler.h"

typedef struct {
    uint32_t magic;          // FLASH_OTA_MAGIC (0x4F544131 = "OTA1")
    uint32_t fw_size;        // Size of new firmware binary in bytes
    uint32_t checksum_sum32; // 32-bit checksum
    char filename[128];      // Name of uploaded file
} pico_ota_header_t;

#define FLASH_MAGIC_HEADER 0x5049434F   // "PICO" Magic Header

typedef struct {
    uint32_t magic;
    bool has_firmware;
    char filename[128];
    uint32_t size_bytes;
    char big_bin_checksum[32];
    char sum32_checksum[64];
} persistent_fw_info_t;

typedef struct {
    bool has_firmware;
    char filename[128];
    uint32_t size_bytes;
    char big_bin_checksum[32];
    char sum32_checksum[64];
} firmware_info_t;

static firmware_info_t g_fw_info = {
    .has_firmware = false,
    .filename = "firmware.bin",
    .size_bytes = 0,
    .big_bin_checksum = "N/A",
    .sum32_checksum = "N/A"
};

// 🛡️ 상호 배제 Lock 플래그 (Pico 2 OTA vs Realtek 스케일러 작업 동시 충돌 방지)
volatile bool g_pico_ota_busy = false;
volatile bool g_rtd_busy = false;

static void load_firmware_info_from_flash(void) {
    const persistent_fw_info_t *flash_ptr = (const persistent_fw_info_t *)(XIP_BASE + FLASH_INFO_OFFSET);
    if (flash_ptr->magic == FLASH_MAGIC_HEADER && flash_ptr->has_firmware) {
        g_fw_info.has_firmware = true;
        strncpy(g_fw_info.filename, flash_ptr->filename, sizeof(g_fw_info.filename) - 1);
        g_fw_info.size_bytes = flash_ptr->size_bytes;
        strncpy(g_fw_info.big_bin_checksum, flash_ptr->big_bin_checksum, sizeof(g_fw_info.big_bin_checksum) - 1);
        strncpy(g_fw_info.sum32_checksum, flash_ptr->sum32_checksum, sizeof(g_fw_info.sum32_checksum) - 1);
        printf("?¾ [Flash] ??¥ë ?ì¨???ë³´ ë³µì ?ë£! (?ì¼: %s, ?©ë: %lu Bytes)\n", g_fw_info.filename, (unsigned long)g_fw_info.size_bytes);
    }
}

static void __no_inline_not_in_flash_func(safe_flash_erase)(uint32_t offset, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    uint32_t old_systick = systick_hw->csr;
    systick_hw->csr = 0;
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");

    flash_range_erase(offset, count);

    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
    systick_hw->csr = old_systick;
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(safe_flash_program)(uint32_t offset, const uint8_t *data, size_t count) {
    uint32_t ints = save_and_disable_interrupts();
    uint32_t old_systick = systick_hw->csr;
    systick_hw->csr = 0;
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");

    flash_range_program(offset, data, count);

    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
    systick_hw->csr = old_systick;
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(save_firmware_info_to_flash)(void) {
    persistent_fw_info_t info_data;
    memset(&info_data, 0, sizeof(info_data));
    info_data.magic = FLASH_MAGIC_HEADER;
    info_data.has_firmware = g_fw_info.has_firmware;
    strncpy(info_data.filename, g_fw_info.filename, sizeof(info_data.filename) - 1);
    info_data.size_bytes = g_fw_info.size_bytes;
    strncpy(info_data.big_bin_checksum, g_fw_info.big_bin_checksum, sizeof(info_data.big_bin_checksum) - 1);
    strncpy(info_data.sum32_checksum, g_fw_info.sum32_checksum, sizeof(info_data.sum32_checksum) - 1);

    static uint8_t sector_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(256)));
    memset(sector_buf, 0xFF, sizeof(sector_buf));
    memcpy(sector_buf, &info_data, sizeof(info_data));

    safe_flash_erase(FLASH_INFO_OFFSET, FLASH_SECTOR_SIZE);
    safe_flash_program(FLASH_INFO_OFFSET, sector_buf, FLASH_SECTOR_SIZE);

    printf("?¾ [Flash] Pico 2 ?ë??ë©ëª¨ë¦¬ì ?ì¨???ë³´ ?êµ¬ ????ë£!\n");
}

void http_server_init(void) {
    load_firmware_info_from_flash();
}

static bool parse_hex_bytes(const char *str, uint8_t *out_buf, size_t *out_len, size_t max_len) {
    size_t count = 0;
    const char *p = str;
    while (*p && count < max_len) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!isxdigit((unsigned char)p[0])) break;
        
        char hex_byte[3] = {0};
        hex_byte[0] = p[0];
        p++;
        if (isxdigit((unsigned char)p[0])) {
            hex_byte[1] = p[0];
            p++;
        }
        out_buf[count++] = (uint8_t)strtoul(hex_byte, NULL, 16);
    }
    *out_len = count;
    return (count > 0);
}

static uint32_t s_last_erased_ota_sector = 0xFFFFFFFF;
static uint32_t s_last_erased_rtd_sector = 0xFFFFFFFF;

static void reset_ota_sector_erase_tracker(void) {
    s_last_erased_ota_sector = 0xFFFFFFFF;
}

static void reset_rtd_sector_erase_tracker(void) {
    s_last_erased_rtd_sector = 0xFFFFFFFF;
}

static void __no_inline_not_in_flash_func(ensure_ota_sector_erased)(uint32_t flash_addr) {
    uint32_t sector_addr = flash_addr & ~(FLASH_SECTOR_SIZE - 1);
    if (sector_addr != s_last_erased_ota_sector) {
        safe_flash_erase(sector_addr, FLASH_SECTOR_SIZE);
        s_last_erased_ota_sector = sector_addr;
    }
}

static void __no_inline_not_in_flash_func(ensure_rtd_sector_erased)(uint32_t flash_addr) {
    uint32_t sector_addr = flash_addr & ~(FLASH_SECTOR_SIZE - 1);
    if (sector_addr != s_last_erased_rtd_sector) {
        safe_flash_erase(sector_addr, FLASH_SECTOR_SIZE);
        s_last_erased_rtd_sector = sector_addr;
    }
}

static const char *ci_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    while (*haystack) {
        size_t i = 0;
        while (needle[i] && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return haystack;
        haystack++;
    }
    return NULL;
}

static bool check_tag_with_carry(const uint8_t *chunk, uint32_t len, uint8_t *carry_buf, uint32_t *carry_len, const char *tag) {
    if (!chunk || len == 0 || !tag) return false;
    size_t tag_len = strlen(tag);
    if (tag_len == 0) return false;

    // 1. If we have carry from previous chunk, test boundary seam
    if (carry_buf && carry_len && *carry_len > 0) {
        uint8_t boundary[128];
        uint32_t head = *carry_len;
        if (head > tag_len) head = tag_len;
        uint32_t tail = len;
        if (tail > tag_len) tail = tag_len;
        memcpy(boundary, carry_buf + (*carry_len - head), head);
        memcpy(boundary + head, chunk, tail);
        uint32_t total = head + tail;
        if (total >= tag_len) {
            for (uint32_t i = 0; i <= total - tag_len; i++) {
                if (memcmp(boundary + i, tag, tag_len) == 0) return true;
            }
        }
    }

    // 2. Test within current chunk
    if (len >= tag_len) {
        for (uint32_t i = 0; i <= len - tag_len; i++) {
            if (memcmp(chunk + i, tag, tag_len) == 0) return true;
        }
    }

    // 3. Save trailing bytes to carry buffer for next boundary check
    if (carry_buf && carry_len) {
        uint32_t save_len = (len > 48) ? 48 : len;
        memcpy(carry_buf, chunk + (len - save_len), save_len);
        *carry_len = save_len;
    }
    return false;
}

static void get_opposite_chip_tag(char *out, size_t out_sz) {
#if (TARGET_ETH_CHIP == CHIP_W6300)
    // Obfuscated "##PICO2_ETH_TARGET:W5500##" (XOR 0x5A)
    static const uint8_t enc[] = {
        0x79, 0x79, 0x0A, 0x13, 0x19, 0x15, 0x68, 0x05, 0x1F, 0x0E, 0x12, 0x05, 0x0E, 0x1B, 0x08, 0x1D, 0x1F, 0x0E, 0x60, 0x0D, 0x6F, 0x6F, 0x6A, 0x6A, 0x79, 0x79, 0x5A
    };
#else
    // Obfuscated "##PICO2_ETH_TARGET:W6300##" (XOR 0x5A)
    static const uint8_t enc[] = {
        0x79, 0x79, 0x0A, 0x13, 0x19, 0x15, 0x68, 0x05, 0x1F, 0x0E, 0x12, 0x05, 0x0E, 0x1B, 0x08, 0x1D, 0x1F, 0x0E, 0x60, 0x0D, 0x6C, 0x69, 0x6A, 0x6A, 0x79, 0x79, 0x5A
    };
#endif
    size_t len = sizeof(enc) - 1;
    if (len >= out_sz) len = out_sz - 1;
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)(enc[i] ^ 0x5A);
    }
    out[len] = '\0';
}

static void process_uf2_chunk(const uint8_t *data, uint32_t len, uint8_t *uf2_block, uint32_t *uf2_idx, uint32_t *out_sum, uint32_t *out_written, bool *out_chip_mismatch, uint8_t *carry_buf, uint32_t *carry_len, const char *opp_tag) {
    if (out_chip_mismatch && *out_chip_mismatch) return;
    for (uint32_t i = 0; i < len; i++) {
        uf2_block[(*uf2_idx)++] = data[i];
        if (*uf2_idx == 512) {
            uint32_t m1 = (uint32_t)uf2_block[0] | ((uint32_t)uf2_block[1] << 8) | ((uint32_t)uf2_block[2] << 16) | ((uint32_t)uf2_block[3] << 24);
            uint32_t m2 = (uint32_t)uf2_block[4] | ((uint32_t)uf2_block[5] << 8) | ((uint32_t)uf2_block[6] << 16) | ((uint32_t)uf2_block[7] << 24);
            uint32_t m3 = (uint32_t)uf2_block[508] | ((uint32_t)uf2_block[509] << 8) | ((uint32_t)uf2_block[510] << 16) | ((uint32_t)uf2_block[511] << 24);

            if (m1 == 0x0A324655ULL && m2 == 0x9E5D5157ULL && (m3 == 0x0AB16F30ULL || m3 == 0x0A51697DULL)) {
                uint32_t target_addr = (uint32_t)uf2_block[12] | ((uint32_t)uf2_block[13] << 8) | ((uint32_t)uf2_block[14] << 16) | ((uint32_t)uf2_block[15] << 24);
                uint32_t plen = (uint32_t)uf2_block[16] | ((uint32_t)uf2_block[17] << 8) | ((uint32_t)uf2_block[18] << 16) | ((uint32_t)uf2_block[19] << 24);

                uint32_t flash_rel = target_addr;
                if (flash_rel >= 0x10000000ULL) {
                    flash_rel -= 0x10000000ULL;
                }

                if (plen == 256 && flash_rel < FLASH_OTA_MAX_SIZE) {
                    const uint8_t *payload = &uf2_block[32];
                    if (check_tag_with_carry(payload, 256, carry_buf, carry_len, opp_tag)) {
                        if (out_chip_mismatch) *out_chip_mismatch = true;
                        *uf2_idx = 0;
                        return;
                    }

                    for (int k = 0; k < 256; k++) {
                        *out_sum += payload[k];
                    }

                    uint32_t dest_flash_addr = FLASH_OTA_STAGING_OFFSET + flash_rel;
                    ensure_ota_sector_erased(dest_flash_addr);

                    safe_flash_program(dest_flash_addr, payload, 256);

                    *out_written += 256;
                }
            }
            *uf2_idx = 0;
        }
    }
}

static void extract_big_bin_checksum(const char *filename, uint32_t total_sum, char *out_buf, size_t out_len) {
    if (filename && filename[0]) {
        const char *p = filename;
        while (*p) {
            if ((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) && strlen(p) >= 10) {
                char hex_part[9] = {0};
                strncpy(hex_part, p + 2, 8);
                bool valid = true;
                for (int i = 0; i < 8; i++) {
                    if (!isxdigit((unsigned char)hex_part[i])) { valid = false; break; }
                }
                if (valid) {
                    snprintf(out_buf, out_len, "%.4s %.4s", hex_part, hex_part + 4);
                    return;
                }
            }
            p++;
        }
    }
    uint16_t h = (total_sum >> 16) & 0xFFFF;
    uint16_t l = total_sum & 0xFFFF;
    snprintf(out_buf, out_len, "%04X %04X", h, l);
}

static uint32_t parse_query_uint32(const char *req_str, const char *key) {
    const char *p = strstr(req_str, key);
    if (!p) return 0;
    p += strlen(key);
    if (*p == '=') p++;
    return (uint32_t)strtoul(p, NULL, 10);
}

static uint32_t s_chunk_total_sum = 0;
static uint32_t s_chunk_expected_size = 0;
static uint32_t s_chunk_received_bytes = 0;
static char s_chunk_filename[128] = "firmware.bin";

static uint32_t parse_content_length(const char *req_str) {
    const char *p = req_str;
    while (*p) {
        if (strncasecmp(p, "content-length:", 15) == 0) {
            p += 15;
            while (*p == ' ' || *p == '\t') p++;
            return (uint32_t)atol(p);
        }
        const char *next = strstr(p, "\r\n");
        if (!next) break;
        p = next + 2;
    }
    return 0;
}

static void parse_upload_filename(const char *req_str, char *out_filename, size_t max_len) {
    strncpy(out_filename, "firmware.bin", max_len);
    const char *p = strstr(req_str, "name=");
    if (p) {
        p += 5; // skip "name="
        size_t idx = 0;
        while (*p && *p != ' ' && *p != '&' && *p != '\r' && *p != '\n' && idx < max_len - 1) {
            out_filename[idx++] = *p++;
        }
        out_filename[idx] = '\0';
    }
    for (size_t i = 0; i < strlen(out_filename); i++) {
        if (out_filename[i] == '%') out_filename[i] = '_';
    }
}

static void build_html_page(char *buf, size_t max_len, float cpu_temp, bool led_state, uint32_t uptime_sec) {
    buf[0] = '\0';
    float cpu_gauge_angle = (cpu_temp + 40.0f) / 140.0f * 180.0f - 90.0f;
    if (cpu_gauge_angle < -90.0f) cpu_gauge_angle = -90.0f;
    if (cpu_gauge_angle > 90.0f) cpu_gauge_angle = 90.0f;

    uint32_t init_d = uptime_sec / 86400;
    uint32_t init_h = (uptime_sec % 86400) / 3600;
    uint32_t init_m = (uptime_sec % 3600) / 60;
    uint32_t init_s = uptime_sec % 60;

    // 1. Head, CSS, and JS
    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>DHUB PICO2 [" ETH_CHIP_NAME "]</title>"
        "<style>"
        "* { box-sizing: border-box; margin: 0; padding: 0; }"
        "html, body { min-height: 100%%; overflow-y: auto; overflow-x: hidden; scroll-behavior: smooth; }"
        "body { font-family: 'Segoe UI', Arial, sans-serif; background: #0b132b; color: #f8fafc; text-align: left; padding: 0; margin: 0; font-size: 13px; }"
        ".top-bar { background: linear-gradient(90deg, #1c2541, #0b132b); border-bottom: 2px solid #3a506b; padding: 10px 20px; display: flex; justify-content: space-between; align-items: center; }"
        ".brand { font-size: 20px; font-weight: bold; color: #5bc0be; display: flex; align-items: center; gap: 8px; }"
        ".logo-box { background: #5bc0be; color: #0b132b; padding: 2px 8px; border-radius: 4px; font-weight: 900; font-size: 16px; }"
        ".title-banner { background: #1c2541; border: 1px solid #3a506b; color: #ffffff; padding: 6px 20px; border-radius: 6px; font-weight: bold; font-size: 14px; box-shadow: inset 0 0 10px rgba(0,0,0,0.5); }"
        ".clock-container { text-align: right; }"
        ".clock { color: #6fffe9; font-family: monospace; font-size: 13px; font-weight: bold; }"
        ".uptime { color: #5bc0be; font-family: monospace; font-size: 11px; font-weight: bold; margin-top: 2px; }"
        ".app-layout { display: flex; flex-direction: row; min-height: 100vh; gap: 10px; padding: 10px; }"
        ".sidebar { width: 170px; background: #1c2541; border-radius: 8px; border: 1px solid #3a506b; padding: 8px; display: flex; flex-direction: column; gap: 4px; flex-shrink: 0; }"
        ".nav-btn { background: #0b132b; border: 1px solid #3a506b; color: #ffffff; padding: 9px 10px; border-radius: 4px; font-size: 12px; font-weight: bold; text-align: left; cursor: pointer; transition: 0.2s; }"
        ".nav-btn:hover, .nav-btn.active { background: #5bc0be; color: #0b132b; border-color: #5bc0be; font-weight: bold; }"
        ".main-content { flex: 1; display: flex; flex-direction: column; gap: 10px; min-width: 0; padding-bottom: 80px; }"
        ".welcome-card { background: #1c2541; border: 1px solid #3a506b; border-radius: 8px; padding: 15px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }"
        ".welcome-card h2 { color: #5bc0be; font-size: 18px; margin-bottom: 10px; border-bottom: 1px solid #3a506b; padding-bottom: 5px; }"
        ".info-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 12px; color: #cbd5e1; font-size: 12px; }"
        ".voltages { display: flex; gap: 20px; font-size: 15px; font-weight: bold; margin: 12px 0; padding: 8px 12px; background: #0b132b; border-radius: 6px; border: 1px solid #3a506b; }"
        ".voltages span { color: #6fffe9; }"
        ".status-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px; font-weight: bold; font-size: 13px; margin-top: 8px; }"
        ".good { color: #4ade80; }"
        ".detect { color: #f87171; }"
        ".open { color: #f87171; }"
        ".warn { color: #fbbf24; }"
        ".card { background: #1c2541; border: 1px solid #3a506b; border-radius: 8px; padding: 14px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }"
        ".card h3 { color: #5bc0be; font-size: 14px; margin-bottom: 10px; border-bottom: 1px solid #3a506b; padding-bottom: 4px; }"
        ".btn-group { display: flex; gap: 10px; margin-top: 8px; }"
        ".btn { padding: 8px 16px; border: none; border-radius: 6px; font-size: 13px; font-weight: bold; cursor: pointer; text-decoration: none; color: white; transition: 0.2s; }"
        ".btn-on { background: #22c55e; }"
        ".btn-off { background: #ef4444; }"
        ".file-input { background: #0b132b; border: 1px dashed #3a506b; padding: 8px; border-radius: 6px; width: 100%%; color: #94a3b8; font-size: 12px; cursor: pointer; }"
        ".gauge-panel { width: 180px; background: #1c2541; border: 1px solid #3a506b; border-radius: 8px; padding: 10px; display: flex; flex-direction: column; gap: 10px; align-items: center; flex-shrink: 0; }"
        ".gauge-card { background: #0b132b; border: 1px solid #3a506b; border-radius: 6px; padding: 8px; width: 100%%; text-align: center; box-shadow: inset 0 0 8px rgba(0,0,0,0.5); }"
        ".gauge-title { color: #94a3b8; font-size: 11px; font-weight: bold; margin-bottom: 4px; }"
        ".gauge-val { color: #6fffe9; font-size: 16px; font-weight: bold; margin-top: 2px; }"
        ".unit { font-size: 11px; color: #94a3b8; }"
        ".footer { background: #0b132b; border-top: 1px solid #3a506b; color: #64748b; text-align: center; padding: 8px; font-size: 11px; margin-top: 10px; }"
        "</style>"
        "<script>"
        "let currentUptime = %lu;"
        "function updateClock(){"
        "  const d = new Date();"
        "  const m = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];"
        "  const el = document.getElementById('liveClock');"
        "  if(el){"
        "    el.innerText = m[d.getMonth()] + ' ' + String(d.getDate()).padStart(2,'0') + ' \\'' + String(d.getFullYear()).slice(-2) + ' ' + d.toTimeString().split(' ')[0];"
        "  }"
        "}"
        "function formatUptime(sec){"
        "  const d = Math.floor(sec / 86400);"
        "  const h = Math.floor((sec %% 86400) / 3600);"
        "  const m = Math.floor((sec %% 3600) / 60);"
        "  const s = sec %% 60;"
        "  return '⏱ Uptime: ' + String(d).padStart(2,'0') + 'd ' + String(h).padStart(2,'0') + 'h ' + String(m).padStart(2,'0') + 'm ' + String(s).padStart(2,'0') + 's';"
        "}"
        "function tickUptime(){"
        "  currentUptime++;"
        "  const el = document.getElementById('liveUptime');"
        "  if(el){ el.innerText = formatUptime(currentUptime); }"
        "  const dpms = document.getElementById('onTimeDpms');"
        "  if(dpms){"
        "    const h = Math.floor(currentUptime / 3600);"
        "    const m = Math.floor((currentUptime %% 3600) / 60);"
        "    const s = currentUptime %% 60;"
        "    dpms.innerText = String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0') + ' (00:00)';"
        "  }"
        "}"
        "function renderLedStatus(d){"
        "  const val = document.getElementById('ledStateVal');"
        "  const bulb = document.getElementById('ledBulbVisual');"
        "  const modeTxt = document.getElementById('ledCurrentModeText');"
        "  const intTxt = document.getElementById('ledCurrentIntervalText');"
        "  if(val){"
        "    val.innerHTML = d.led_state ? 'ON 🟢' : 'OFF 🔴';"
        "    val.style.color = d.led_state ? '#4ade80' : '#ef4444';"
        "  }"
        "  if(bulb){"
        "    if(d.led_state){"
        "      bulb.style.background = '#22c55e';"
        "      bulb.style.boxShadow = '0 0 25px #22c55e, 0 0 50px rgba(34,197,94,0.7)';"
        "      bulb.style.borderColor = '#4ade80';"
        "    } else {"
        "      bulb.style.background = '#1e293b';"
        "      bulb.style.boxShadow = 'none';"
        "      bulb.style.borderColor = '#475569';"
        "    }"
        "  }"
        "  if(modeTxt && d.mode !== undefined){"
        "    const modes = ['⚡ 하트비트 점멸 (Blink)', '💡 상시 점등 (ON)', '🛑 상시 소등 (OFF)', '🆘 SOS 긴급 모스부호', '📍 비컨 위치 식별'];"
        "    modeTxt.innerText = modes[d.mode] || '알 수 없음';"
        "  }"
        "  if(intTxt && d.interval !== undefined){"
        "    intTxt.innerText = d.interval + ' ms';"
        "  }"
        "}"
        "function setLedMode(mode, interval){"
        "  let url = '/api/led?mode=' + mode;"
        "  if(interval) url += '&interval=' + interval;"
        "  fetch(url).then(r => r.json()).then(d => {"
        "    renderLedStatus(d);"
        "  }).catch(e => {});"
        "}"
        "function toggleLed(state){"
        "  setLedMode(state ? 'on' : 'off');"
        "}"
        "let isRealtekBusy = false;"
        "let isPicoOtaBusy = false;"
        "function setRealtekLock(busy){"
        "  isRealtekBusy = busy;"
        "  const pFile = document.getElementById('picoOtaFile');"
        "  const pBtn = document.getElementById('picoOtaBtn');"
        "  if(pFile) pFile.disabled = busy;"
        "  if(pBtn){"
        "    pBtn.disabled = busy;"
        "    pBtn.style.opacity = busy ? '0.35' : '1';"
        "    pBtn.style.cursor = busy ? 'not-allowed' : 'pointer';"
        "    pBtn.title = busy ? '⚠️ Realtek 작업 진행 중 (Pico OTA 잠김)' : '';"
        "  }"
        "}"
        "function setPicoOtaLock(busy){"
        "  isPicoOtaBusy = busy;"
        "  const rFile = document.getElementById('fwFile');"
        "  const rBtn = document.getElementById('uploadBtn');"
        "  const rFlashBtn = document.getElementById('rtdFlashBtn');"
        "  if(rFile) rFile.disabled = busy;"
        "  if(rBtn){"
        "    rBtn.disabled = busy;"
        "    rBtn.style.opacity = busy ? '0.35' : '1';"
        "    rBtn.style.cursor = busy ? 'not-allowed' : 'pointer';"
        "    rBtn.title = busy ? '⚠️ Pico 2 OTA 진행 중 (Realtek 업로드 잠김)' : '';"
        "  }"
        "  if(rFlashBtn){"
        "    rFlashBtn.disabled = busy;"
        "    rFlashBtn.style.opacity = busy ? '0.35' : '1';"
        "    rFlashBtn.style.cursor = busy ? 'not-allowed' : 'pointer';"
        "    rFlashBtn.title = busy ? '⚠️ Pico 2 OTA 진행 중 (Realtek 플래시 잠김)' : '';"
        "  }"
        "}"
        "let isFetchingStatus = false;"
        "function fetchStatus(){"
        "  if(isFetchingStatus || isPicoOtaBusy || isRealtekBusy) return;"
        "  isFetchingStatus = true;"
        "  const ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;"
        "  const toId = ctrl ? setTimeout(() => ctrl.abort(), 2500) : null;"
        "  const fetchOpts = ctrl ? { signal: ctrl.signal, cache: 'no-store' } : { cache: 'no-store' };"
        "  fetch('/api/status?t=' + Date.now(), fetchOpts).then(r => r.json()).then(d => {"
        "    if(toId) clearTimeout(toId);"
        "    isFetchingStatus = false;"
        "    if(d && d.uptime_sec !== undefined && d.uptime_sec > 0){"
        "      currentUptime = d.uptime_sec;"
        "    }"
        "    if(d && d.led_state !== undefined){"
        "      renderLedStatus({"
        "        led_state: d.led_state,"
        "        mode: d.led_mode,"
        "        interval: d.led_interval"
        "      });"
        "    }"
        "    if(d && d.ota_busy !== undefined){"
        "      if(d.ota_busy && !isPicoOtaBusy) setPicoOtaLock(true);"
        "      else if(!d.ota_busy && isPicoOtaBusy) setPicoOtaLock(false);"
        "    }"
        "    if(d && d.rtd_busy !== undefined){"
        "      if(d.rtd_busy && !isRealtekBusy) setRealtekLock(true);"
        "      else if(!d.rtd_busy && isRealtekBusy) setRealtekLock(false);"
        "    }"
        "  }).catch(e => {"
        "    if(toId) clearTimeout(toId);"
        "    isFetchingStatus = false;"
        "  });"
        "}"
        "function startTimers(){"
        "  updateClock();"
        "  tickUptime();"
        "  setInterval(updateClock, 1000);"
        "  setInterval(tickUptime, 1000);"
        "  window.statusTimer = setInterval(fetchStatus, 1500);"
        "  const inp = document.getElementById('ethCmdInput');"
        "  if(inp){"
        "    inp.addEventListener('input', updateCharCount);"
        "    inp.addEventListener('keyup', updateCharCount);"
        "    inp.addEventListener('change', updateCharCount);"
        "  }"
        "}"
        "function switchTab(name, btn){"
        "  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));"
        "  if(btn){"
        "    btn.classList.add('active');"
        "  } else {"
        "    document.querySelectorAll('.nav-btn').forEach(b => {"
        "      if(b.getAttribute('onclick') && b.getAttribute('onclick').includes(\"'\" + name + \"'\")) b.classList.add('active');"
        "    });"
        "  }"
        "  const allSections = ['overview-section', 'firmware-section', 'rs232-section', 'led_ctrl-section', 'generic-section'];"
        "  allSections.forEach(id => {"
        "    const el = document.getElementById(id);"
        "    if(el) el.style.display = 'none';"
        "  });"
        "  if(name === 'overview'){"
        "    const el = document.getElementById('overview-section');"
        "    if(el) el.style.display = 'block';"
        "  } else if(name === 'firmware'){"
        "    const el = document.getElementById('firmware-section');"
        "    if(el) el.style.display = 'grid';"
        "  } else if(name === 'rs232'){"
        "    const el = document.getElementById('rs232-section');"
        "    if(el) el.style.display = 'block';"
        "  } else if(name === 'led_ctrl'){"
        "    const el = document.getElementById('led_ctrl-section');"
        "    if(el) el.style.display = 'block';"
        "  } else {"
        "    const el = document.getElementById('generic-section');"
        "    const title = document.getElementById('generic-title');"
        "    if(el){"
        "      if(title) title.innerText = name.toUpperCase().replace('_', ' ') + ' Setup';"
        "      el.style.display = 'block';"
        "    }"
        "  }"
        "}"
        "function handlePicoFileSelect(e){"
        "  const file = e.target.files[0];"
        "  const statusMsg = document.getElementById('pico-ota-status-msg');"
        "  const btn = document.getElementById('picoOtaBtn');"
        "  if(!file){ if(statusMsg) statusMsg.innerHTML=''; return; }"
        "  const fname = file.name.toLowerCase();"
        "  const currentChip = '" ETH_CHIP_NAME "';"
        "  const oppositeChip = '" OPPOSITE_CHIP_NAME "';"
        "  const oppositeTag = atob('" OPPOSITE_CHIP_TAG_B64 "');"
        "  if(fname.includes(oppositeChip.toLowerCase())){"
        "    const errMsg = '❌ [칩셋 파일명 불일치 차단] 현재 장비는 ' + currentChip + ' 보드입니다!\\n\\n선택하신 파일(' + file.name + ')은 ' + oppositeChip + ' 전용 펌웨어이므로, 장비 통신 먹통(벽돌) 방지를 위해 업로드가 차단되었습니다.\\n👉 올바른 ' + currentChip + ' 전용 펌웨어를 선택해주세요.';"
        "    alert(errMsg);"
        "    e.target.value = '';"
        "    if(statusMsg){ statusMsg.style.color='#ef4444'; statusMsg.innerHTML='❌ <strong>칩셋 불일치 차단:</strong> ' + file.name + ' (' + oppositeChip + ' 전용 펌웨어는 ' + currentChip + ' 보드에 업로드할 수 없습니다.)'; }"
        "    return;"
        "  }"
        "  if(statusMsg){ statusMsg.style.color='#38bdf8'; statusMsg.innerHTML='⏳ 바이너리 무결성 및 ' + currentChip + ' 하드웨어 서명 사전 검사 중...'; }"
        "  const reader = new FileReader();"
        "  reader.onload = function(evt){"
        "    const buf = evt.target.result;"
        "    const u8 = new Uint8Array(buf);"
        "    const magic_uf2 = (u8[0] | (u8[1] << 8) | (u8[2] << 16) | (u8[3] << 24)) >>> 0;"
        "    const initial_sp = magic_uf2;"
        "    const is_uf2 = (magic_uf2 === 0x0A324655) || fname.endsWith('.uf2');"
        "    if(is_uf2){"
        "      alert('⚠️ [OTA 파일 형식 안내]\\n\\n선택하신 파일(' + file.name + ')은 USB BOOTSEL 드래그 앤 드롭 복사용 .uf2 파일입니다.\\n\\n이더넷 OTA 원격 업데이트는 100%% 무결성을 위해 .bin 바이너리 파일만 지원합니다.\\n\\n👉 build 폴더 안의 ' + currentChip.toLowerCase() + '_pico2_firmware.bin 파일을 선택해주세요!');"
        "      e.target.value = '';"
        "      if(statusMsg){ statusMsg.style.color='#facc15'; statusMsg.innerHTML='⚠️ <strong>.bin 전용:</strong> 이더넷 OTA는 .bin 바이너리만 지원합니다. (UF2는 초기 USB 복사용)'; }"
        "      return;"
        "    }"
        "    const is_rp2350_bin = (initial_sp === 0x4D535052 || initial_sp === 0xFFFFEDAC || (initial_sp >= 0x20000000 && initial_sp <= 0x200B0000));"
        "    const is_realtek = fname.includes('dh9') || fname.includes('rlt') || fname.includes('dlc') || (u8[0] === 0x02 || u8[0] === 0x12);"
        "    if(!is_rp2350_bin && is_realtek){"
        "      alert('⚠️ [안전 차단] 선택하신 파일(' + file.name + ')은 Realtek 스케일러 펌웨어입니다!\\n\\nPico 2 OTA 카드에는 Pico 2 전용 펌웨어만 업로드할 수 있습니다.\\nRealtek 펌웨어는 오른쪽 [⚡ Realtek 전용 스케일러 ISP] 카드를 이용해주세요.');"
        "      e.target.value = '';"
        "      if(statusMsg){ statusMsg.style.color='#ef4444'; statusMsg.innerHTML='❌ <strong>Realtek 펌웨어 감지됨:</strong> 오른쪽 스케일러 카드를 이용해주세요.'; }"
        "      return;"
        "    }"
        "    const tagBytes = new TextEncoder().encode(oppositeTag);"
        "    const tLen = tagBytes.length;"
        "    let tagFound = false;"
        "    for(let i = 0; i <= u8.length - tLen; i++){"
        "      if(u8[i] === tagBytes[0]){"
        "        let match = true;"
        "        for(let j = 1; j < tLen; j++){"
        "          if(u8[i+j] !== tagBytes[j]){ match = false; break; }"
        "        }"
        "        if(match){ tagFound = true; break; }"
        "      }"
        "    }"
        "    if(tagFound){"
        "      alert('❌ [바이너리 칩셋 서명(Signature) 사전 차단!]\\n\\n현재 장비: ' + currentChip + ' 보드\\n선택 파일: ' + file.name + '\\n검출 서명: ' + oppositeChip + ' 전용 바이너리 (' + oppositeTag + ')\\n\\n⚠️ 파일명을 변경하였더라도 내부 바이너리 서명이 달라 통신 불가(벽돌) 위험이 있습니다.\\n펌웨어 업로드가 사전에 안전하게 차단되었습니다!');"
        "      e.target.value = '';"
        "      if(statusMsg){ statusMsg.style.color='#ef4444'; statusMsg.innerHTML='❌ <strong>' + oppositeChip + ' 바이너리 서명 검출됨:</strong> ' + currentChip + ' 보드 교차 업로드 불가'; }"
        "      return;"
        "    }"
        "    if(statusMsg){"
        "      statusMsg.style.color='#4ade80';"
        "      statusMsg.innerHTML='<div style=\"background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #0284c7;font-size:11px;\">📁 <strong>' + file.name + '</strong> (' + (file.size/1024).toFixed(1) + ' KB) - BIN 순수 바이너리 | 🛡️ ' + currentChip + ' 무결성 검증 정상</div>';"
        "    }"
        "  };"
        "  reader.readAsArrayBuffer(file);"
        "}"
        "function handleFileSelect(e){"
        "  const file = e.target.files[0];"
        "  const infoDiv = document.getElementById('selected-file-info');"
        "  const statusMsg = document.getElementById('status-msg');"
        "  const btn = document.getElementById('uploadBtn');"
        "  if(!file){ infoDiv.style.display='none'; return; }"
        "  const fname = file.name;"
        "  const fnameLower = fname.toLowerCase();"
        "  if(fnameLower.includes('pico2') || fnameLower.includes('w5500') || fnameLower.includes('w6300')){"
        "    alert('❌ [Realtek 스케일러 펌웨어 오류]\\n\\n선택하신 파일(' + fname + ')은 Pico 2 메인보드 전용 펌웨어입니다!\\n\\n이 카드는 Realtek 디스플레이 스케일러 IC 전용 펌웨어 저장 공간입니다.\\nPico 2 메인보드 펌웨어는 왼쪽 [🚀 Pico 2 전용 이더넷 OTA] 카드를 이용해주세요.');"
        "    e.target.value = '';"
        "    infoDiv.style.display='block';"
        "    infoDiv.innerHTML = '<div style=\"background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #ef4444;text-align:left;font-size:11px;color:#ef4444;\">❌ <strong>Pico 2 전용 펌웨어 차단:</strong> Realtek 스케일러 펌웨어를 선택해주세요.</div>';"
        "    return;"
        "  }"
        "  let bigBinFromFn = null;"
        "  const match = fname.match(/0[xX]([0-9a-fA-F]{8})/);"
        "  if(match && match[1]){"
        "    const hex = match[1].toUpperCase();"
        "    bigBinFromFn = hex.substring(0,4)+' '+hex.substring(4,8);"
        "  }"
        "  infoDiv.style.display='block';"
        "  infoDiv.innerHTML = '<div style=\"background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #5bc0be;text-align:left;font-size:11px;\"><span style=\"color:#5bc0be;font-weight:bold;\">⏳ 체크섬 및 스케일러 서명 검사 중...</span> <span style=\"color:#cbd5e1;\">' + fname + ' (' + (file.size/1024).toFixed(1) + ' KB)</span></div>';"
        "  const reader = new FileReader();"
        "  reader.onload = function(evt){"
        "    const bytes = new Uint8Array(evt.target.result);"
        "    const magic_uf2 = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;"
        "    const initial_sp = magic_uf2;"
        "    const is_uf2 = (magic_uf2 === 0x0A324655);"
        "    const is_rp2350_bin = (initial_sp === 0x4D535052 || initial_sp === 0xFFFFEDAC || (initial_sp >= 0x20000000 && initial_sp <= 0x200B0000));"
        "    if(is_uf2 || is_rp2350_bin){"
        "      alert('❌ [Realtek 스케일러 펌웨어 오류]\\n\\n선택하신 파일(' + fname + ') 내부 바이너리는 Pico 2 (RP2350) ARM 실행 파일입니다!\\n\\nRealtek 스케일러 플래시에는 Realtek IC 바이너리만 업로드해야 합니다.\\nPico 2 펌웨어는 왼쪽 [🚀 Pico 2 전용 이더넷 OTA] 카드를 이용해주세요.');"
        "      e.target.value = '';"
        "      infoDiv.innerHTML = '<div style=\"background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #ef4444;text-align:left;font-size:11px;color:#ef4444;\">❌ <strong>Pico 2 ARM 바이너리 차단:</strong> Realtek 스케일러 펌웨어 파일이 아닙니다.</div>';"
        "      return;"
        "    }"
        "    let totalSum = 0;"
        "    for(let i=0; i<bytes.length; i++) totalSum = (totalSum + bytes[i]) >>> 0;"
        "    const h = ((totalSum >>> 16) & 0xFFFF).toString(16).padStart(4,'0').toUpperCase();"
        "    const l = (totalSum & 0xFFFF).toString(16).padStart(4,'0').toUpperCase();"
        "    const sumHex = totalSum.toString(16).padStart(8,'0').toUpperCase();"
        "    const sum32Chk = h + ' ' + l + ' (0x' + sumHex + ')';"
        "    const bigBinDisplay = bigBinFromFn ? bigBinFromFn : (h + ' ' + l);"
        "    infoDiv.innerHTML = '<div style=\"background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #22c55e;text-align:left;font-size:11px;\"><div style=\"color:#cbd5e1;margin-bottom:2px;\">📁 <strong>' + fname + '</strong> (' + (file.size/1024).toFixed(1) + ' KB)</div><div style=\"color:#5bc0be;font-family:monospace;font-size:11px;\">🏷 Big Bin: ' + bigBinDisplay + ' | 🔢 Sum32: ' + sum32Chk + '</div></div>';"
        "  };"
        "  reader.readAsArrayBuffer(file);"
        "}"
        "function clearStoredFirmware(){"
        "  if(!confirm('저장된 Realtek 펌웨어 정보를 삭제(초기화)하시겠습니까?')) return;"
        "  fetch('/api/clear_fw').then(r => r.json()).then(d => {"
        "    const el = document.getElementById('stored-fw-container');"
        "    if(el){"
        "      el.innerHTML = '<div style=\"color:#94a3b8; font-size:12px; background:#0b132b; padding:8px; border-radius:6px; border:1px solid #3a506b;\">저장된 펌웨어 없음</div>';"
        "    }"
        "  }).catch(e => alert('초기화 실패'));"
        "}"
        "function uploadFirmware(){"
        "  if(isPicoOtaBusy) return alert('⚠️ [작업 차단] Pico 2 이더넷 OTA가 진행 중입니다!\\n\\nOTA 완료 후 Realtek 작업을 진행해주세요.');"
        "  const file = document.getElementById('fwFile').files[0];"
        "  if(!file) return alert('업로드할 Realtek 펌웨어 파일(.bin)을 선택해주세요!');"
        "  const fnameLower = file.name.toLowerCase();"
        "  if(fnameLower.includes('pico2') || fnameLower.includes('w5500') || fnameLower.includes('w6300')){"
        "    return alert('❌ [업로드 차단] 선택하신 파일은 Pico 2 보드 전용 펌웨어입니다!\\n\\nRealtek 스케일러 카드에는 Realtek 전용 펌웨어만 업로드할 수 있습니다.');"
        "  }"
        "  setRealtekLock(true);"
        "  const btn = document.getElementById('uploadBtn');"
        "  const statusMsg = document.getElementById('status-msg');"
        "  const progressContainer = document.getElementById('progress-container');"
        "  const progressBar = document.getElementById('progress-bar');"
        "  const progressText = document.getElementById('progress-text');"
        "  btn.disabled = true; btn.style.opacity = '0.5';"
        "  progressContainer.style.display = 'block';"
        "  progressBar.style.width = '0%%';"
        "  progressText.innerText = '0%% (0 KB / ' + (file.size / 1024).toFixed(0) + ' KB)';"
        "  statusMsg.style.color = '#5bc0be';"
        "  statusMsg.innerText = '⚡ Realtek 펌웨어 초고속 스트리밍 전송 중...';"
        "  const xhr = new XMLHttpRequest();"
        "  xhr.open('POST', '/upload?name=' + encodeURIComponent(file.name) + '&size=' + file.size, true);"
        "  xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
        "  xhr.timeout = 120000;"
        "  xhr.upload.onprogress = function(e){"
        "    if(e.lengthComputable){"
        "      const pct = Math.round((e.loaded / e.total) * 100);"
        "      progressBar.style.width = pct + '%%';"
        "      progressText.innerText = pct + '%% (' + (e.loaded / 1024).toFixed(0) + ' KB / ' + (e.total / 1024).toFixed(0) + ' KB)';"
        "      statusMsg.innerText = '⚡ 고속 스트리밍 전송 중 (' + pct + '%%)...';"
        "    }"
        "  };"
        "  xhr.onload = function(){"
        "    btn.disabled = false; btn.style.opacity = '1';"
        "    setRealtekLock(false);"
        "    if(xhr.status === 200){"
        "      try {"
        "        const d = JSON.parse(xhr.responseText);"
        "        progressBar.style.width = '100%%';"
        "        progressText.innerText = '100%% (전송 완료)';"
        "        statusMsg.style.color = '#4ade80';"
        "        statusMsg.innerText = '⚡ Realtek 펌웨어(' + d.filename + ') 100%% 무결성 저장 성공!';"
        "        const el = document.getElementById('stored-fw-container');"
        "        if(el){"
        "          el.innerHTML = '<div style=\"text-align: left; background: #0b132b; padding: 8px; border-radius: 6px; border: 1px solid #3a506b; font-size: 12px;\">' +"
        "            '<div style=\"margin-bottom: 4px;\"><strong>📁 저장된 파일명:</strong> <span style=\"color: #cbd5e1; word-break: break-all;\">' + d.filename + '</span></div>' +"
        "            '<div style=\"margin-bottom: 4px;\"><strong>📊 저장된 용량:</strong> <span style=\"color: #4ade80;\">' + d.size.toLocaleString() + ' Bytes (' + d.size_kb + ' KB)</span></div>' +"
        "            '<div style=\"margin-bottom: 4px;\"><strong>🏷 Big Bin Checksum (RTDTool v3.1.3):</strong> <span style=\"color: #5bc0be; font-weight: bold; font-family: monospace;\">' + d.big_bin + '</span></div>' +"
        "            '<div style=\"margin-bottom: 6px;\"><strong>🔢 32-bit Byte Sum Checksum (RTDTool v3.8):</strong> <span style=\"color: #facc15; font-weight: bold; font-family: monospace;\">' + d.sum32 + '</span></div>' +"
        "            '<div style=\"display:flex; gap:6px; margin-top:8px; flex-wrap:wrap;\">' +"
        "            '<button onclick=\"pingRtdScaler(); return false;\" class=\"btn\" style=\"background:#0284c7; padding:6px 10px; font-size:11px; font-weight:bold;\">📡 I2C 통신 확인</button>' +"
        "            '<button onclick=\"flashRtdScaler(); return false;\" id=\"rtdFlashBtn\" class=\"btn\" style=\"background:#22c55e; padding:6px 12px; font-size:11px; font-weight:bold;\">⚡ 스케일러 ISP 플래시 시작</button>' +"
        "            '<button onclick=\"clearStoredFirmware(); return false;\" style=\"background:#ef4444; color:#fff; border:none; padding:6px 8px; border-radius:6px; font-size:11px; font-weight:bold; cursor:pointer;\">🗑 초기화</button>' +"
        "            '</div>' +"
        "            '<div id=\"rtd-isp-progress-container\" style=\"display:none; margin-top:8px; background:#0b132b; border-radius:6px; border:1px solid #3a506b; height:16px; position:relative; overflow:hidden;\"><div id=\"rtd-isp-progress-bar\" style=\"width:0%%; height:100%%; background:linear-gradient(90deg,#22c55e,#5bc0be); transition: width 0.2s;\"></div><div id=\"rtd-isp-progress-text\" style=\"position:absolute; width:100%%; top:0; left:0; line-height:16px; font-size:10px; font-weight:bold; color:#fff; text-align:center;\">0%%</div></div>' +"
        "            '<div id=\"rtd-isp-status-msg\" style=\"margin-top:6px; font-size:12px; min-height:16px; white-space:pre-wrap; text-align:left; color:#94a3b8;\"></div>' +"
        "            '</div>';"
        "        }"
        "      } catch(e) {"
        "        statusMsg.style.color = '#ef4444';"
        "        statusMsg.innerText = '❌ 응답 데이터 파싱 실패: ' + xhr.responseText;"
        "      }"
        "    } else {"
        "      statusMsg.style.color = '#ef4444';"
        "      statusMsg.innerText = '❌ 업로드 실패 (' + xhr.status + '): ' + xhr.responseText;"
        "    }"
        "  };"
        "  xhr.onerror = function(){"
        "    btn.disabled = false; btn.style.opacity = '1';"
        "    setRealtekLock(false);"
        "    statusMsg.style.color = '#ef4444';"
        "    statusMsg.innerText = '❌ 전송 중 네트워크 연결 오류가 발생했습니다.';"
        "  };"
        "  xhr.send(file);"
        "}"
        "function pingRtdScaler(){"
        "  const msg = document.getElementById('rtd-isp-status-msg');"
        "  if(msg){ msg.style.color = '#5bc0be'; msg.innerText = '📡 Channel 2 (GP2/GP3) Realtek 스케일러(0x4A) I2C 통신 확인 중...'; }"
        "  fetch('/api/rtd/ping')"
        "  .then(r => r.json())"
        "  .then(d => {"
        "    if(d.connected){"
        "      if(msg){"
        "        msg.innerHTML = '<div style=\"background:#0b132b; padding:8px 12px; border-radius:6px; border:1px solid #22c55e; font-size:12px; color:#cbd5e1;\">' +"
        "          '<div style=\"color:#4ade80; font-weight:bold; margin-bottom:4px;\">' + d.message + '</div>' +"
        "          '<div>• <strong>I2C 연결:</strong> <span style=\"color:#a5f3fc;\">Channel 2 (GP2:SCL / GP3:SDA @ 주소 0x4A)</span></div>' +"
        "          '<div>• <strong>상태:</strong> <span style=\"color:#22c55e; font-weight:bold;\">정상 응답 (ACK 수신 완료)</span></div>' +"
        "          '<button onclick=\"readRtdFlashInfo(); return false;\" class=\"btn\" style=\"margin-top:6px; background:#0284c7; padding:4px 10px; font-size:11px; font-weight:bold;\">🔍 온보드 Flash 칩셋 JEDEC ID 상세 조회</button>' +"
        "          '</div>';"
        "      }"
        "    } else {"
        "      if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ Realtek 스케일러 응답 없음 (I2C 0x4A NACK - GP2/GP3 배선 및 스케일러 전원 확인 필요)'; }"
        "    }"
        "  }).catch(e => {"
        "    if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ I2C 통신 오류 발생'; }"
        "  });"
        "}"
        "function readRtdFlashInfo(){"
        "  const msg = document.getElementById('rtd-isp-status-msg');"
        "  if(msg){ msg.style.color = '#5bc0be'; msg.innerText = '🔍 온보드 Flash 칩셋 JEDEC ID (0x9F) 조회 중...'; }"
        "  fetch('/api/rtd/info')"
        "  .then(r => r.json())"
        "  .then(d => {"
        "    if(d.connected && msg){"
        "      let detail = '<div style=\"background:#0b132b; padding:8px 12px; border-radius:6px; border:1px solid #0284c7; font-size:12px; color:#cbd5e1;\">' +"
        "        '<div style=\"color:#5bc0be; font-weight:bold; margin-bottom:4px;\">📡 Realtek 스케일러 온보드 Flash 감지 정보</div>' +"
        "        '<div>• <strong>Flash 제조사:</strong> <span style=\"color:#facc15; font-weight:bold;\">' + (d.mfg_name || 'N/A') + ' (ID: ' + d.mfg_id + ')</span></div>' +"
        "        '<div>• <strong>Flash 칩 모델:</strong> <span style=\"color:#4ade80; font-weight:bold;\">' + (d.flash_model || 'Generic SPI Flash') + '</span></div>' +"
        "        '<div>• <strong>Flash 용량:</strong> <span style=\"color:#6fffe9; font-weight:bold;\">' + (d.flash_size || 'N/A') + '</span></div>' +"
        "        '<div>• <strong>JEDEC ID:</strong> <span style=\"color:#cbd5e1; font-family:monospace;\">' + d.mfg_id + ' ' + d.mem_type + ' ' + d.cap_id + '</span></div>' +"
        "        '</div>';"
        "      msg.innerHTML = detail;"
        "    }"
        "  }).catch(e => {"
        "    if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ Flash 정보 조회 오류'; }"
        "  });"
        "}"
        "function flashRtdScaler(){"
        "  if(isPicoOtaBusy) return alert('⚠️ [작업 차단] Pico 2 이더넷 OTA가 진행 중입니다!\\n\\nOTA 완료 후 Realtek 스케일러 플래싱을 진행해주세요.');"
        "  if(!confirm('Pico 2에 저장된 Realtek 펌웨어를 Channel 2 (GP2:SCL, GP3:SDA)를 통해 실제 Realtek 스케일러 칩에 플래싱하시겠습니까?')) return;"
        "  setRealtekLock(true);"
        "  const btn = document.getElementById('rtdFlashBtn');"
        "  const msg = document.getElementById('rtd-isp-status-msg');"
        "  const progContainer = document.getElementById('rtd-isp-progress-container');"
        "  const progBar = document.getElementById('rtd-isp-progress-bar');"
        "  const progText = document.getElementById('rtd-isp-progress-text');"
        "  if(btn){ btn.disabled = true; btn.style.opacity = '0.5'; }"
        "  if(progContainer){ progContainer.style.display = 'block'; }"
        "  if(progBar){ progBar.style.width = '0%%'; }"
        "  if(progText){ progText.innerText = '0%% (ISP 진입 및 64KB 블록 소거 중...)'; }"
        "  if(msg){ msg.style.color = '#facc15'; msg.innerText = '⚡ Realtek 스케일러 ISP 플래시 시작 중...'; }"
        "  let startTime = Date.now();"
        "  const totalEstimatedSec = 16.0;"
        "  const progTimer = setInterval(() => {"
        "    const elapsed = (Date.now() - startTime) / 1000;"
        "    let pct = Math.min(95, Math.round((elapsed / totalEstimatedSec) * 95));"
        "    if(pct < 10) {"
        "      if(progText) progText.innerText = pct + '%% (64KB 블록 소거 중...)';"
        "      if(msg) msg.innerText = '⚡ 64KB 블록 소거 진행 중 (' + pct + '%%)...';"
        "    } else {"
        "      const estKB = Math.min(768, Math.round((pct / 95) * 768));"
        "      if(progText) progText.innerText = pct + '%% (' + estKB + ' KB / 768 KB)';"
        "      if(msg) msg.innerText = '⚡ I2C 64B 고속 버스트 플래시 중 (' + pct + '%%)...';"
        "    }"
        "    if(progBar) progBar.style.width = pct + '%%';"
        "  }, 200);"
        "  fetch('/api/rtd/flash', { method: 'POST' })"
        "  .then(r => r.json())"
        "  .then(d => {"
        "    clearInterval(progTimer);"
        "    if(btn){ btn.disabled = false; btn.style.opacity = '1'; }"
        "    setRealtekLock(false);"
        "    if(d.success){"
        "      if(progBar){ progBar.style.width = '100%%'; }"
        "      if(progText){ progText.innerText = '100%% (플래시 완료)'; }"
        "      if(msg){ msg.style.color = '#4ade80'; msg.innerText = '🎉 ' + d.message; }"
        "    } else {"
        "      if(msg){ msg.style.color = '#ef4444'; msg.innerText = d.message; }"
        "    }"
        "  }).catch(e => {"
        "    clearInterval(progTimer);"
        "    if(btn){ btn.disabled = false; btn.style.opacity = '1'; }"
        "    setRealtekLock(false);"
        "    if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ 플래시 통신 오류 발생!'; }"
        "  });"
        "}"
        "function uploadPicoOta(){"
        "  if(isRealtekBusy) return alert('⚠️ [작업 차단] Realtek 펌웨어 작업(업로드 또는 ISP 플래싱)이 진행 중입니다!\\n\\n해당 작업 완료 후 Pico 2 OTA를 진행해주세요.');"
        "  const file = document.getElementById('picoOtaFile').files[0];"
        "  if(!file) return alert('Pico 2 펌웨어(.bin/.uf2) 파일을 선택해주세요!');"
        "  const reader = new FileReader();"
        "  reader.onload = function(e){"
        "    const buf = e.target.result;"
        "    const u8 = new Uint8Array(buf);"
        "    const magic_uf2 = (u8[0] | (u8[1] << 8) | (u8[2] << 16) | (u8[3] << 24)) >>> 0;"
        "    const initial_sp = magic_uf2;"
        "    const is_uf2 = (magic_uf2 === 0x0A324655);"
        "    const is_rp2350_bin = (initial_sp === 0x4D535052 || initial_sp === 0xFFFFEDAC || (initial_sp >= 0x20000000 && initial_sp <= 0x200B0000));"
        "    const fname = file.name.toLowerCase();"
        "    const is_realtek = fname.includes('dh9') || fname.includes('rlt') || fname.includes('dlc') || (u8[0] === 0x02 || u8[0] === 0x12);"
        "    if(!is_uf2 && !is_rp2350_bin && is_realtek){"
        "      alert('⚠️ [안전 차단] 선택하신 파일(' + file.name + ')은 Realtek 스케일러 펌웨어입니다!\\n\\nPico 2 OTA 카드에는 Pico 2 전용 펌웨어만 업로드할 수 있습니다.');"
        "      return;"
        "    }"
        "    function showSuccess(msg){"
        "      if(window.statusTimer){ clearInterval(window.statusTimer); window.statusTimer = null; }"
        "      statusMsg.style.color = '#4ade80';"
        "      let cnt = 7;"
        "      statusMsg.innerText = (msg || '🚀 Pico 2 이더넷 OTA 펌웨어 업로드 성공!') + '\\n\\n⚡ 새 펌웨어 플래시 적용 및 재부팅 중 (' + cnt + '초)...';"
        "      const timer = setInterval(() => {"
        "        cnt--;"
        "        if(cnt > 0){"
        "          statusMsg.innerText = (msg || '🚀 Pico 2 이더넷 OTA 펌웨어 업로드 성공!') + '\\n\\n⚡ 새 펌웨어 플래시 적용 및 재부팅 중 (' + cnt + '초)...';"
        "        } else {"
        "          clearInterval(timer);"
        "          statusMsg.innerText = '⚡ 새 펌웨어 온라인 통신 확인 중...';"
        "          let pollCount = 0;"
        "          let isProbing = false;"
        "          const pollTimer = setInterval(() => {"
        "            if(isProbing) return;"
        "            pollCount++;"
        "            isProbing = true;"
        "            const ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;"
        "            const toId = ctrl ? setTimeout(() => ctrl.abort(), 1200) : null;"
        "            const fetchOpts = ctrl ? { signal: ctrl.signal, cache: 'no-store' } : { cache: 'no-store' };"
        "            fetch('/api/status?t=' + Date.now(), fetchOpts)"
        "              .then(r => {"
        "                if(toId) clearTimeout(toId);"
        "                isProbing = false;"
        "                if(r.ok) {"
        "                  clearInterval(pollTimer);"
        "                  statusMsg.innerText = '🎉 새 펌웨어 온라인 연결 성공! 화면을 갱신합니다...';"
        "                  setTimeout(() => {"
        "                    window.location.href = '/?t=' + Date.now();"
        "                  }, 200);"
        "                }"
        "              })"
        "              .catch(e => {"
        "                if(toId) clearTimeout(toId);"
        "                isProbing = false;"
        "                statusMsg.innerText = '⚡ 새 펌웨어 온라인 대기 중... (' + pollCount + '초 경과)';"
        "                if(pollCount >= 25) {"
        "                  statusMsg.innerHTML = '⚡ 새 펌웨어 온라인 대기 중... (' + pollCount + '초 경과)<br><br>' +"
        "                    '<button onclick=\"window.location.href=\\'/?t=\\' + Date.now();\" class=\"btn\" style=\"background:#0284c7; padding:6px 14px; font-size:12px; font-weight:bold;\">🔄 지금 바로 접속 시도</button>';"
        "                }"
        "              });"
        "          }, 1000);"
        "        }"
        "      }, 1000);"
        "    }"
        "    const currentChip = '" ETH_CHIP_NAME "';"
        "    const oppositeChip = '" OPPOSITE_CHIP_NAME "';"
        "    const oppositeTag = atob('" OPPOSITE_CHIP_TAG_B64 "');"
        "    if(fname.includes(oppositeChip.toLowerCase())){"
        "      alert('❌ [칩셋 파일명 불일치 차단]\\n\\n현재 장비는 ' + currentChip + ' 보드입니다!\\n\\n선택하신 파일(' + file.name + ')은 ' + oppositeChip + ' 전용 펌웨어이므로, 장비 통신 먹통(벽돌) 방지를 위해 업로드가 안전하게 차단되었습니다.\\n\\n👉 올바른 ' + currentChip + ' 전용 펌웨어를 선택해주세요.');"
        "      return;"
        "    }"
        "    const tagBytes = new TextEncoder().encode(oppositeTag);"
        "    const tLen = tagBytes.length;"
        "    let tagFound = false;"
        "    for(let i = 0; i <= u8.length - tLen; i++){"
        "      if(u8[i] === tagBytes[0]){"
        "        let match = true;"
        "        for(let j = 1; j < tLen; j++){"
        "          if(u8[i+j] !== tagBytes[j]){ match = false; break; }"
        "        }"
        "        if(match){ tagFound = true; break; }"
        "      }"
        "    }"
        "    if(tagFound){"
        "      alert('❌ [바이너리 칩셋 서명(Signature) 사전 차단!]\\n\\n현재 장비: ' + currentChip + ' 보드\\n업로드 파일: ' + file.name + '\\n검출된 서명: ' + oppositeChip + ' 전용 바이너리 (' + oppositeTag + ')\\n\\n⚠️ 파일명을 변경하였더라도 내부 바이너리 서명이 달라 통신 불가(벽돌) 위험이 있습니다.\\n펌웨어 업로드가 사전에 안전하게 차단되었습니다!');"
        "      return;"
        "    }"
        "    if(!confirm('Pico 2 보드를 이더넷 OTA 원격 펌웨어로 업데이트하고 자동 재부팅하시겠습니까?')) return;"
        "    setPicoOtaLock(true);"
        "    const btn = document.getElementById('picoOtaBtn');"
        "    const statusMsg = document.getElementById('pico-ota-status-msg');"
        "    const progressContainer = document.getElementById('pico-ota-progress-container');"
        "    const progressBar = document.getElementById('pico-ota-progress-bar');"
        "    const progressText = document.getElementById('pico-ota-progress-text');"
        "    if(btn){ btn.disabled = true; btn.style.opacity = '0.5'; }"
        "    if(progressContainer) progressContainer.style.display = 'block';"
        "    if(progressBar) progressBar.style.width = '0%%';"
        "    if(progressText) progressText.innerText = '0%% (전송 시작 중...)';"
        "    if(statusMsg){ statusMsg.style.color = '#38bdf8'; statusMsg.innerText = '🚀 Pico 2 이더넷 OTA 펌웨어 전송을 시작합니다... (용량: ' + (file.size/1024).toFixed(1) + ' KB)'; }"
        "    let uploadDone = false;"
        "    let lastPct = 0;"
        "    const xhr = new XMLHttpRequest();"
        "    xhr.open('POST', '/upload_pico_fw?name=' + encodeURIComponent(file.name), true);"
        "    xhr.setRequestHeader('Content-Type', 'application/octet-stream');"
        "    xhr.timeout = 120000;"
        "    xhr.upload.onprogress = function(e){"
        "      if(e.lengthComputable){"
        "        const pct = Math.round((e.loaded / e.total) * 100);"
        "        lastPct = pct;"
        "        progressBar.style.width = pct + '%%';"
        "        progressText.innerText = pct + '%% (' + (e.loaded / 1024).toFixed(0) + ' KB / ' + (e.total / 1024).toFixed(0) + ' KB)';"
        "        if(pct >= 95) uploadDone = true;"
        "      } };"
        "    xhr.onload = function(){"
        "      btn.disabled = false; btn.style.opacity = '1';"
        "      if(xhr.status === 200){"
        "        showSuccess(xhr.responseText);"
        "      } else {"
        "        setPicoOtaLock(false);"
        "        statusMsg.style.color = '#ef4444';"
        "        statusMsg.innerText = '❌ OTA 업로드 실패: ' + xhr.responseText;"
        "      } };"
        "    xhr.onerror = function(){"
        "      if(uploadDone || lastPct >= 90){"
        "        showSuccess('🚀 Pico 2 이더넷 OTA 전송 완료! (재부팅 적용 중...)');"
        "      } else {"
        "        btn.disabled = false; btn.style.opacity = '1';"
        "        setPicoOtaLock(false);"
        "        statusMsg.style.color = '#ef4444';"
        "        statusMsg.innerText = '❌ 네트워크 오류 발생!';"
        "      } };"
        "    xhr.send(buf);"
        "  };"
        "  reader.readAsArrayBuffer(file);"
        "}"
        "function updateCharCount(){"
        "  const input = document.getElementById('ethCmdInput');"
        "  const cnt = document.getElementById('ethCharCounter');"
        "  if(input && cnt){ cnt.innerText = input.value.length; }"
        "}"
        "function clearEthLog(){"
        "  const log = document.getElementById('ethCmdLog');"
        "  if(log){ log.innerHTML = '<div style=\"color:#94a3b8;\">[시스템] RS-232 터미널 로그가 초기화되었습니다.</div>'; }"
        "}"
        "function downloadEthLog(){"
        "  const log = document.getElementById('ethCmdLog');"
        "  if(!log) return;"
        "  const text = log.innerText || log.textContent;"
        "  const blob = new Blob([text], {type: 'text/plain;charset=utf-8'});"
        "  const url = URL.createObjectURL(blob);"
        "  const a = document.createElement('a');"
        "  a.href = url;"
        "  const d = new Date();"
        "  const dateStr = d.getFullYear() + String(d.getMonth()+1).padStart(2,'0') + String(d.getDate()).padStart(2,'0') + '_' + String(d.getHours()).padStart(2,'0') + String(d.getMinutes()).padStart(2,'0') + String(d.getSeconds()).padStart(2,'0');"
        "  a.download = 'rs232_serial_log_' + dateStr + '.txt';"
        "  document.body.appendChild(a); a.click();"
        "  document.body.removeChild(a); URL.revokeObjectURL(url);"
        "}"
        "let cmdHistory = [];"
        "let historyIdx = -1;"
        "function handleCmdKeyDown(e){"
        "  if(e.key === 'Enter'){"
        "    sendEthCommand();"
        "    return false;"
        "  } else if(e.key === 'ArrowUp'){"
        "    if(cmdHistory.length > 0 && historyIdx < cmdHistory.length - 1){"
        "      historyIdx++;"
        "      const input = document.getElementById('ethCmdInput');"
        "      if(input){ input.value = cmdHistory[cmdHistory.length - 1 - historyIdx]; updateCharCount(); }"
        "    }"
        "    e.preventDefault();"
        "  } else if(e.key === 'ArrowDown'){"
        "    if(historyIdx > 0){"
        "      historyIdx--;"
        "      const input = document.getElementById('ethCmdInput');"
        "      if(input){ input.value = cmdHistory[cmdHistory.length - 1 - historyIdx]; updateCharCount(); }"
        "    } else if(historyIdx === 0){"
        "      historyIdx = -1;"
        "      const input = document.getElementById('ethCmdInput');"
        "      if(input){ input.value = ''; updateCharCount(); }"
        "    }"
        "    e.preventDefault();"
        "  }"
        "}"
        "function setPreset(cmd, ending){"
        "  const input = document.getElementById('ethCmdInput');"
        "  if(input){ input.value = cmd; updateCharCount(); }"
        "  if(ending){"
        "    const sel = document.getElementById('ethEndingSelect');"
        "    if(sel) sel.value = ending;"
        "  }"
        "}"
        "function setAndSend(cmd, ending){"
        "  setPreset(cmd, ending);"
        "  sendEthCommand();"
        "}"
        "let isSending = false;"
        "let sendWatchdog = null;"
        "function resetSendBtn(){"
        "  isSending = false;"
        "  if(sendWatchdog){ clearTimeout(sendWatchdog); sendWatchdog = null; }"
        "  const btn = document.getElementById('ethSendBtn');"
        "  const input = document.getElementById('ethCmdInput');"
        "  if(btn){"
        "    btn.style.opacity = '1';"
        "    btn.style.background = '#22c55e';"
        "    btn.style.transform = 'scale(1)';"
        "    btn.innerHTML = '🚀 전송 (Send)';"
        "    btn.disabled = false;"
        "  }"
        "  if(input){ input.style.border = '1px solid #3a506b'; }"
        "}"
        "function sendEthCommand(){"
        "  if(isSending) return;"
        "  isSending = true;"
        "  const input = document.getElementById('ethCmdInput');"
        "  const log = document.getElementById('ethCmdLog');"
        "  const portSelect = document.getElementById('ethUartPortSelect');"
        "  const baudSelect = document.getElementById('ethBaudSelect');"
        "  const endingSelect = document.getElementById('ethEndingSelect');"
        "  const timeoutSelect = document.getElementById('ethTimeoutSelect');"
        "  const btn = document.getElementById('ethSendBtn');"
        "  if(btn){"
        "    btn.style.opacity = '0.85';"
        "    btn.style.background = '#0284c7';"
        "    btn.style.transform = 'scale(0.95)';"
        "    btn.innerHTML = '⏳ 전송 중...';"
        "    btn.disabled = true;"
        "  }"
        "  const cmd = input ? input.value.trim() : '';"
        "  if(!cmd){"
        "    if(btn){ btn.innerHTML = '⚠️ 커맨드 입력 필요!'; btn.style.background = '#eab308'; }"
        "    if(input){ input.focus(); input.style.border = '2px solid #eab308'; }"
        "    setTimeout(resetSendBtn, 800);"
        "    return;"
        "  }"
        "  if(cmdHistory.length === 0 || cmdHistory[cmdHistory.length - 1] !== cmd){"
        "    cmdHistory.push(cmd);"
        "    if(cmdHistory.length > 30) cmdHistory.shift();"
        "  }"
        "  historyIdx = -1;"
        "  const port = portSelect ? portSelect.value : 'uart0';"
        "  const baud = baudSelect ? baudSelect.value : '9600';"
        "  const ending = endingSelect ? endingSelect.value : 'NONE';"
        "  const timeoutVal = timeoutSelect ? parseInt(timeoutSelect.value) : 2000;"
        "  if(input){ input.style.border = '2px solid #5bc0be'; }"
        "  const now = new Date();"
        "  const timeStr = String(now.getHours()).padStart(2,'0') + ':' + String(now.getMinutes()).padStart(2,'0') + ':' + String(now.getSeconds()).padStart(2,'0') + '.' + String(Math.floor(now.getMilliseconds()/100));"
        "  const autoScroll = document.getElementById('ethAutoScroll') ? document.getElementById('ethAutoScroll').checked : true;"
        "  log.innerHTML += '<div>[' + timeStr + '] 📤 <strong style=\"color:#38bdf8;\">TX (' + port.toUpperCase() + ' @ ' + baud + 'bps, ' + ending + '):</strong> ' + cmd + '</div>';"
        "  if(autoScroll) log.scrollTop = log.scrollHeight;"
        "  const t0 = Date.now();"
        "  const controller = new AbortController();"
        "  const abortTimeout = setTimeout(() => controller.abort(), timeoutVal + 2500);"
        "  sendWatchdog = setTimeout(resetSendBtn, timeoutVal + 3000);"
        "  async function fetchCmdWithRetry(url, opt, retries=3){"
        "    for(let i=0; i<retries; i++){"
        "      try {"
        "        const r = await fetch(url, opt);"
        "        if(r.ok) return r;"
        "      } catch(e){"
        "        if(i === retries - 1) throw e;"
        "      }"
        "      await new Promise(res => setTimeout(res, 60));"
        "    }"
        "  }"
        "  fetchCmdWithRetry('/api/command?port=' + port + '&cmd=' + encodeURIComponent(cmd) + '&baud=' + baud + '&ending=' + ending + '&timeout=' + timeoutVal, { signal: controller.signal })"
        "  .then(r => r.json())"
        "  .then(d => {"
        "    clearTimeout(abortTimeout);"
        "    const dt = Date.now() - t0;"
        "    const lat = d.latency_ms !== undefined ? d.latency_ms : dt;"
        "    if(d.hex && d.hex.trim().length > 0){"
        "      log.innerHTML += '<div style=\"color:#4ade80;\">[' + timeStr + '] 📥 <strong style=\"color:#4ade80;\">RX (' + lat + 'ms, ' + d.rx_bytes + 'B):</strong> <span style=\"color:#facc15; font-family:monospace;\">HEX:[' + d.hex.trim() + ']</span> <span style=\"color:#6fffe9;\">ASCII:\"' + d.ascii + '\"</span></div>';"
        "      if(btn){ btn.innerHTML = '✅ 수신 완료 (' + lat + 'ms)'; btn.style.background = '#22c55e'; }"
        "    } else {"
        "      log.innerHTML += '<div style=\"color:#fbbf24;\">[' + timeStr + '] ⚠️ <strong>RX (' + lat + 'ms):</strong> 타겟 응답 없음 (타임아웃 ' + (timeoutVal/1000).toFixed(1) + 's)</div>' +"
        "        '<div style=\"color:#94a3b8; font-size:10px; padding-left:12px;\">확인: 배선 TX/RX 교차 연결, 보레이트(' + baud + 'bps), 종단문자(' + ending + '), 3.3V/RS232 레벨변환기</div>';"
        "      if(btn){ btn.innerHTML = '⚠️ 수신 타임아웃'; btn.style.background = '#eab308'; }"
        "    }"
        "    if(autoScroll) log.scrollTop = log.scrollHeight;"
        "    setTimeout(resetSendBtn, 800);"
        "  }).catch(e => {"
        "    clearTimeout(abortTimeout);"
        "    const dt = Date.now() - t0;"
        "    log.innerHTML += '<div style=\"color:#ef4444;\">[' + timeStr + '] ❌ 통신 타임아웃 또는 전송 오류 (' + dt + 'ms)</div>' +"
        "      '<div style=\"color:#94a3b8; font-size:10px; padding-left:12px;\">확인: W5500 네트워크 상태 또는 하드웨어 연결을 확인해주세요.</div>';"
        "    if(btn){ btn.innerHTML = '❌ 전송 타임아웃'; btn.style.background = '#ef4444'; }"
        "    setTimeout(resetSendBtn, 800);"
        "  });"
        "  updateCharCount();"
        "}"
        "let autoRepeatTimer = null;"
        "function toggleAutoRepeat(){"
        "  const chk = document.getElementById('ethAutoRepeatChk');"
        "  const intSel = document.getElementById('ethIntervalSelect');"
        "  if(chk && chk.checked){"
        "    const ms = intSel ? parseInt(intSel.value) : 2000;"
        "    sendEthCommand();"
        "    autoRepeatTimer = setInterval(sendEthCommand, ms);"
        "  } else {"
        "    if(autoRepeatTimer){ clearInterval(autoRepeatTimer); autoRepeatTimer = null; }"
        "  }"
        "}"
        "function getStoredMacros(){"
        "  try { const m = localStorage.getItem('dh_custom_macros_v2'); return m ? JSON.parse(m) : null; } catch(e){ return null; }"
        "}"
        "function saveMacrosToStorage(macros){"
        "  try { localStorage.setItem('dh_custom_macros_v2', JSON.stringify(macros)); } catch(e){}"
        "}"
        "function renderCustomMacros(){"
        "  const container = document.getElementById('customMacroContainer');"
        "  if(!container) return;"
        "  let macros = getStoredMacros();"
        "  if(!macros || macros.length === 0){"
        "    macros = ["
        "      { name: '버전 확인', cmd: 'GETVERSIONX', ending: 'NONE' },"
        "      { name: '전원 상태', cmd: 'GETSTATUS', ending: 'NONE' },"
        "      { name: 'HDMI 1', cmd: 'SOURCE HDMI1', ending: 'NONE' },"
        "      { name: 'DP 입력', cmd: 'SOURCE DP', ending: 'NONE' },"
        "      { name: 'HEX Ping', cmd: '69 00 01 FF', ending: 'NONE' }"
        "    ];"
        "    saveMacrosToStorage(macros);"
        "  }"
        "  let html = '';"
        "  macros.forEach((m, idx) => {"
        "    html += '<div style=\"display:inline-flex; align-items:center; background:#1c2541; border:1px solid #3a506b; border-radius:4px; margin:2px;\">' +"
        "      '<button onclick=\"setAndSend(\\'' + m.cmd + '\\',\\'' + (m.ending || 'NONE') + '\\');\" style=\"background:none; border:none; color:#6fffe9; padding:4px 8px; font-size:11px; font-weight:bold; cursor:pointer;\">' + m.name + '</button>' +"
        "      '<span onclick=\"deleteCustomMacro(' + idx + ');\" style=\"color:#ef4444; padding:2px 6px; cursor:pointer; font-size:10px; border-left:1px solid #3a506b;\" title=\"삭제\">✕</span>' +"
        "      '</div>';"
        "  });"
        "  container.innerHTML = html;"
        "}"
        "function addCustomMacro(){"
        "  const name = prompt('새 매크로 버튼 이름을 입력하세요 (예: 밝기 100):');"
        "  if(!name) return;"
        "  const cmd = prompt('전송할 커맨드를 입력하세요 (예: BRIGHTNESS 100 또는 69 00 02 FF):');"
        "  if(!cmd) return;"
        "  let macros = getStoredMacros() || [];"
        "  macros.push({ name: name, cmd: cmd, ending: 'NONE' });"
        "  saveMacrosToStorage(macros);"
        "  renderCustomMacros();"
        "}"
        "function deleteCustomMacro(idx){"
        "  let macros = getStoredMacros() || [];"
        "  if(idx >= 0 && idx < macros.length){"
        "    macros.splice(idx, 1);"
        "    saveMacrosToStorage(macros);"
        "    renderCustomMacros();"
        "  }"
        "}"
        "</script>"
        "</head>"
        "<body onload=\"startTimers(); renderCustomMacros();\">"
        "<div class=\"top-bar\">"
        "<div class=\"brand\"><span class=\"logo-box\">DH</span> DHUB PICO2 SYSTEM <span style=\"background:" ETH_CHIP_BADGE_COLOR "; color:#fff; font-size:11px; padding:2px 8px; border-radius:10px; font-weight:bold; letter-spacing:0.5px; margin-left:6px;\">" ETH_CHIP_NAME "</span></div>"
        "<div class=\"title-banner\">DHUB-2026S SYSTEM CONTROLLER (" BOARD_HW_NAME ")</div>"
        "<div class=\"clock-container\">"
        "<div class=\"clock\" id=\"liveClock\">Loading...</div>"
        "<div class=\"uptime\" id=\"liveUptime\">⏱ Uptime: %02lud %02luh %02lum %02lus</div>"
        "</div>"
        "</div>",
        (unsigned long)uptime_sec, (unsigned long)init_d, (unsigned long)init_h, (unsigned long)init_m, (unsigned long)init_s);

    // 2. Sidebar & Main Layout (RS-232 Serial has no plug logo)
    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "<div class=\"app-layout\">"
        "<div class=\"sidebar\">"
        "<button class=\"nav-btn active\" onclick=\"switchTab('overview', this)\">Overview</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('system', this)\">System Setup</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('power_in', this)\">Power Input Setup</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('temp_hum', this)\">Temperature &amp; Humidity</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('analog_in', this)\">Analog Input Setup</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('other_in', this)\">Other Input Setup</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('fan_ctrl', this)\">FAN Control Setup</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('lcd_ctrl', this)\">LCD Controller</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('power_out', this)\">Power Output Setup</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('firmware', this)\">Firmware Update</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('rs232', this)\">RS-232 Serial</button>"
        "<button class=\"nav-btn\" onclick=\"switchTab('led_ctrl', this)\">LED Control</button>"
        "</div>"
        "<div class=\"main-content\">"
        "<div class=\"welcome-card\" id=\"overview-section\">"
        "<h2>Welcome!</h2>"
        "<div class=\"info-grid\">"
        "<div><strong>Version:</strong> <span style=\"color:#6fffe9; font-weight:bold;\">" FIRMWARE_VERSION "</span></div><div><strong>Ethernet Chip:</strong> <span style=\"color:#38bdf8; font-weight:bold;\">" ETH_CHIP_NAME " (" ETH_CHIP_DESC ")</span></div><div><strong>Hardware:</strong> " BOARD_HW_NAME "</div>"
        "<div><strong>Build Date:</strong> " __DATE__ "</div><div><strong>LCD Controller:</strong> Saffron</div><div><strong>On Time(dpms):</strong> <span id=\"onTimeDpms\">00:00(00:00)</span></div>"
        "<div><strong>Board Status:</strong> Display Port</div><div><strong>Output:</strong> ON(12V/24V)</div>"
        "</div>"
        "<div class=\"voltages\">"
        "<span>12V : 11.9 V</span><span>24V : 22.9 V</span><span>5V : 4.8 V</span>"
        "</div>"
        "<div class=\"status-grid\">"
        "<div>Dust Filter : <span class=\"good\">Good</span></div><div>BLU Intensity : <span class=\"good\">Good</span></div>"
        "<div>Front Intensity : <span class=\"good\">Good</span></div><div>Motion Detect : <span class=\"detect\">Detect</span></div>"
        "<div>Impact Detect : <span class=\"detect\">Detect</span></div><div>Invertor : <span class=\"good\">Ok</span></div>"
        "<div>Door Open : <span class=\"open\">Open</span></div>"
        "</div></div>");

    // 3. Firmware Update Section: 2-Column Side-by-Side Grid (Pico 2 OTA vs Realtek Scaler ISP)
    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "<div id=\"firmware-section\" style=\"display: none; grid-template-columns: repeat(auto-fit, minmax(380px, 1fr)); gap: 12px; margin-bottom: 8px;\">"
        
        // --- Left Card: Pico 2 Ethernet OTA ---
        "<div class=\"card\" style=\"border: 2px solid #0284c7; background: #111e38;\">"
        "<h3 style=\"color: #38bdf8; border-bottom: 1px solid #0284c7; padding-bottom: 6px;\">🚀 [Pico 2 전용] 이더넷 OTA 원격 업데이트 <span style=\"background:" ETH_CHIP_BADGE_COLOR "; color:#fff; font-size:10px; padding:2px 6px; border-radius:4px; margin-left:6px; font-weight:bold;\">" ETH_CHIP_NAME " Pico 2 OS</span></h3>"
        "<div style=\"font-size: 12px; color: #cbd5e1; margin-bottom: 6px; text-align: left; line-height: 1.4;\">"
        ETH_CHIP_NAME " 이더넷 네트워크로 Pico 2 자체 펌웨어(<code>.bin</code>)를 업로드하여 원격으로 교체하고 자동 재부팅합니다."
        "</div>"
        "<div style=\"font-size: 11px; color: #38bdf8; background: #0c1c38; padding: 4px 8px; border-radius: 4px; border: 1px solid #0284c7; margin-bottom: 10px; text-align: left;\">"
        "🛡️ <strong>하드웨어 서명 보호 활성화:</strong> " ETH_CHIP_NAME " 전용 바이너리 서명 자동 검사 (" OPPOSITE_CHIP_NAME " 교차 업로드 원천 차단)"
        "</div>"
        "<div style=\"text-align: left;\"><label style=\"font-size: 12px; color: #94a3b8; display: block; margin-bottom: 4px;\">📁 업로드할 Pico 2 펌웨어 선택 (<code>.bin</code> 전용):</label>"
        "<input type=\"file\" id=\"picoOtaFile\" class=\"file-input\" accept=\".bin\" onchange=\"handlePicoFileSelect(event)\" style=\"border-color: #0284c7;\">"
        "<button onclick=\"uploadPicoOta()\" id=\"picoOtaBtn\" class=\"btn\" style=\"background:#0284c7; width:100%%; margin-top:8px; padding:9px; font-size:13px; font-weight:bold; cursor:pointer;\">🚀 Pico 2 (" ETH_CHIP_NAME ") 이더넷 OTA 펌웨어 업로드 &amp; 원격 재부팅</button></div>"
        "<div id=\"pico-ota-progress-container\" style=\"display:none; margin-top:8px; background:#0b132b; border-radius:6px; border:1px solid #0284c7; height:16px; position:relative; overflow:hidden;\"><div id=\"pico-ota-progress-bar\" style=\"width:0%%; height:100%%; background:linear-gradient(90deg,#0284c7,#5bc0be);\"></div><div id=\"pico-ota-progress-text\" style=\"position:absolute; width:100%%; top:0; left:0; line-height:16px; font-size:10px; font-weight:bold; color:#fff; text-align:center;\">0%%</div></div>"
        "<div id=\"pico-ota-status-msg\" style=\"margin-top:6px; font-size:12px; min-height:16px; white-space:pre-wrap; text-align:left;\"></div>"
        "</div>"

        // --- Right Card: Realtek Scaler ISP ---
        "<div class=\"card\" style=\"border: 2px solid #22c55e; background: #0f2427;\">"
        "<h3 style=\"color: #4ade80; border-bottom: 1px solid #22c55e; padding-bottom: 6px;\">⚡ [Realtek 전용] 스케일러 ISP 플래시 <span style=\"background:#22c55e; color:#0b132b; font-size:10px; padding:2px 6px; border-radius:4px; margin-left:6px; font-weight:bold;\">Channel 2: GP2/GP3</span></h3>"
        "<div style=\"margin-bottom: 8px;\"><div style=\"font-size: 12px; color: #94a3b8; margin-bottom: 4px;\">💾 저장된 리얼텍 펌웨어 (Pico 2 플래시 보존):</div>"
        "<div id=\"stored-fw-container\">");

    if (g_fw_info.has_firmware) {
        snprintf(buf + strlen(buf), max_len - strlen(buf),
            "<div style=\"text-align: left; background: #0b132b; padding: 8px; border-radius: 6px; border: 1px solid #3a506b; font-size: 12px;\">"
            "<div style=\"margin-bottom: 4px;\"><strong>📁 저장된 파일명:</strong> <span style=\"color: #cbd5e1; word-break: break-all;\">%s</span></div>"
            "<div style=\"margin-bottom: 4px;\"><strong>📊 저장된 용량:</strong> <span style=\"color: #4ade80;\">%lu Bytes (%.1f KB)</span></div>"
            "<div style=\"margin-bottom: 4px;\"><strong>🏷 Big Bin Checksum (RTDTool v3.1.3):</strong> <span style=\"color: #5bc0be; font-weight: bold; font-family: monospace;\">%s</span></div>"
            "<div style=\"margin-bottom: 6px;\"><strong>🔢 32-bit Byte Sum Checksum (RTDTool v3.8):</strong> <span style=\"color: #facc15; font-weight: bold; font-family: monospace;\">%s</span></div>"
            "<div style=\"display:flex; gap:6px; margin-top:8px; flex-wrap:wrap;\">"
            "<button onclick=\"pingRtdScaler(); return false;\" class=\"btn\" style=\"background:#0284c7; padding:6px 10px; font-size:11px; font-weight:bold;\">📡 I2C 통신 확인</button>"
            "<button onclick=\"flashRtdScaler(); return false;\" id=\"rtdFlashBtn\" class=\"btn\" style=\"background:#22c55e; padding:6px 12px; font-size:11px; font-weight:bold;\">⚡ 스케일러 ISP 플래시 시작</button>"
            "<button onclick=\"clearStoredFirmware(); return false;\" style=\"background:#ef4444; color:#fff; border:none; padding:6px 8px; border-radius:6px; font-size:11px; font-weight:bold; cursor:pointer;\">🗑 초기화</button>"
            "</div>"
            "<div id=\"rtd-isp-progress-container\" style=\"display:none; margin-top:8px; background:#0b132b; border-radius:6px; border:1px solid #3a506b; height:16px; position:relative; overflow:hidden;\"><div id=\"rtd-isp-progress-bar\" style=\"width:0%%; height:100%%; background:linear-gradient(90deg,#22c55e,#5bc0be); transition: width 0.2s;\"></div><div id=\"rtd-isp-progress-text\" style=\"position:absolute; width:100%%; top:0; left:0; line-height:16px; font-size:10px; font-weight:bold; color:#fff; text-align:center;\">0%%</div></div>"
            "<div id=\"rtd-isp-status-msg\" style=\"margin-top:6px; font-size:12px; min-height:16px; white-space:pre-wrap; text-align:left; color:#94a3b8;\"></div>"
            "</div>",
            g_fw_info.filename, (unsigned long)g_fw_info.size_bytes, (float)g_fw_info.size_bytes / 1024.0f,
            g_fw_info.big_bin_checksum, g_fw_info.sum32_checksum);
    } else {
        snprintf(buf + strlen(buf), max_len - strlen(buf),
            "<div style=\"text-align: left; background: #0b132b; padding: 8px; border-radius: 6px; border: 1px solid #3a506b; font-size: 12px;\">"
            "<div style=\"display:flex; justify-content:space-between; align-items:center;\">"
            "<span style=\"color: #94a3b8; font-size: 12px;\">저장된 펌웨어 없음</span>"
            "<button onclick=\"pingRtdScaler(); return false;\" class=\"btn\" style=\"background:#0284c7; padding:4px 10px; font-size:11px; font-weight:bold;\">📡 I2C 통신 확인</button>"
            "</div>"
            "<div id=\"rtd-isp-status-msg\" style=\"margin-top:6px; font-size:12px; min-height:16px; white-space:pre-wrap; text-align:left; color:#94a3b8;\"></div>"
            "</div>");
    }

    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "</div></div>");

    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "<div style=\"margin-top: 8px; text-align: left;\"><label style=\"font-size: 12px; color: #94a3b8; display: block; margin-bottom: 4px;\">📁 업로드할 Realtek 펌웨어 파일 선택 (.bin / .hex):</label>"
        "<input type=\"file\" id=\"fwFile\" class=\"file-input\" accept=\".bin,.hex\" onchange=\"handleFileSelect(event)\" style=\"border-color:#22c55e;\">"
        "<button onclick=\"uploadFirmware()\" id=\"uploadBtn\" class=\"btn\" style=\"background:#22c55e; width:100%%; margin-top:8px; padding:9px; font-size:13px; font-weight:bold; cursor:pointer;\">⚡ Pico 2 플래시 메모리에 고속 전송</button>"
        "<div id=\"selected-file-info\" style=\"margin-top:6px;\"></div>"
        "<div id=\"progress-container\"><div id=\"progress-bar\"></div><div id=\"progress-text\">0%%</div></div>"
        "<div id=\"status-msg\"></div>"
        "</div></div>"
        "</div>"); // Closes 2-column grid container!

    // 4. RS-232 UART Serial Settings & Monitor Control Console Card
    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "<div class=\"card\" id=\"rs232-section\" style=\"display: none;\">"
        "<div style=\"display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #3a506b; padding-bottom:6px; margin-bottom:10px;\">"
        "<h3 style=\"margin:0; border:none; padding:0; color:#5bc0be; font-size:15px;\">RS-232 (UART 직렬 통신) &amp; 스케일러 제어 콘솔</h3>"
        "<div><span style=\"background:#22c55e; color:#0b132b; font-size:11px; padding:2px 8px; border-radius:4px; font-weight:900;\">FW " FIRMWARE_VERSION "</span></div>"
        "</div>"

        // --- Top Bar: HW Port & Baudrate Parameters ---
        "<div style=\"display:flex; gap:10px; margin-bottom:10px; align-items:center; flex-wrap:wrap; background:#0b132b; padding:8px 12px; border-radius:6px; border:1px solid #3a506b;\">"
        "<div><label style=\"color:#94a3b8; font-size:11px; display:block; margin-bottom:2px;\">HW COM 포트(핀):</label>"
        "<select id=\"ethUartPortSelect\" style=\"background:#1c2541; color:#5bc0be; border:1px solid #3a506b; padding:4px 8px; border-radius:4px; font-weight:bold; font-size:12px;\">"
        "<option value=\"uart0\" selected>UART0 (GP0:TX / GP1:RX)</option>"
        "<option value=\"uart1\">UART1 (GP4:TX / GP5:RX)</option>"
        "</select></div>"
        "<div><label style=\"color:#94a3b8; font-size:11px; display:block; margin-bottom:2px;\">Baud Rate:</label>"
        "<select id=\"ethBaudSelect\" style=\"background:#1c2541; color:#5bc0be; border:1px solid #3a506b; padding:4px 8px; border-radius:4px; font-weight:bold; font-size:12px;\">"
        "<option value=\"9600\" selected>9600 bps (Realtek Standard)</option>"
        "<option value=\"115200\">115200 bps</option>"
        "<option value=\"19200\">19200 bps</option>"
        "<option value=\"38400\">38400 bps</option>"
        "<option value=\"57600\">57600 bps</option>"
        "</select></div>"
        "<div><label style=\"color:#94a3b8; font-size:11px; display:block; margin-bottom:2px;\">종단문자:</label>"
        "<select id=\"ethEndingSelect\" style=\"background:#1c2541; color:#5bc0be; border:1px solid #3a506b; padding:4px 8px; border-radius:4px; font-weight:bold; font-size:12px;\">"
        "<option value=\"NONE\" selected>None (없음 - Realtek 권장)</option>"
        "<option value=\"CRLF\">CR+LF (\\r\\n)</option>"
        "<option value=\"CR\">CR (\\r)</option>"
        "<option value=\"LF\">LF (\\n)</option>"
        "</select></div>"
        "<div><label style=\"color:#94a3b8; font-size:11px; display:block; margin-bottom:2px;\">수신 타임아웃:</label>"
        "<select id=\"ethTimeoutSelect\" style=\"background:#1c2541; color:#5bc0be; border:1px solid #3a506b; padding:4px 8px; border-radius:4px; font-weight:bold; font-size:12px;\">"
        "<option value=\"1000\">1.0초</option>"
        "<option value=\"2000\" selected>2.0초 (권장)</option>"
        "<option value=\"3000\">3.0초</option>"
        "<option value=\"5000\">5.0초</option>"
        "</select></div>"
        "<div><label style=\"color:#94a3b8; font-size:11px; display:block; margin-bottom:2px;\">포맷:</label>"
        "<span style=\"color:#cbd5e1; font-size:11px; font-weight:bold; padding-top:4px; display:inline-block;\">8-N-1 (No Parity)</span></div>"
        "<div style=\"margin-left:auto;\"><span style=\"color:#4ade80; font-size:11px; font-weight:bold;\">🟢 통신 엔진 준비됨</span></div>"
        "</div>"

        // --- Section 1: Command Terminal Input & Macros ---
        "<div style=\"display:flex; gap:8px; margin-bottom:6px;\">"
        "<input type=\"text\" id=\"ethCmdInput\" style=\"flex:1; background:#0b132b; border:1px solid #3a506b; color:#5bc0be; padding:9px 12px; border-radius:6px; font-family:monospace; font-weight:bold; font-size:13px;\" maxlength=\"100\" placeholder=\"커맨드 입력 (예: GETVERSIONX, GETSTATUS, POWER ON, 69 00 01 FF)\" oninput=\"updateCharCount();\" onkeyup=\"updateCharCount();\" onchange=\"updateCharCount();\" onkeydown=\"handleCmdKeyDown(event);\">"
        "<button id=\"ethSendBtn\" onclick=\"sendEthCommand(); return false;\" class=\"btn btn-on\" style=\"padding:9px 26px; font-weight:bold; font-size:13px; cursor:pointer; transition: transform 0.15s ease, background 0.2s ease;\">🚀 전송 (Send)</button>"
        "</div>"

        // Macro row + Repeat controls + char counter
        "<div style=\"display:flex; justify-content:space-between; align-items:center; margin-bottom:4px; font-size:11px; color:#94a3b8; flex-wrap:wrap; gap:6px;\">"
        "<div style=\"display:flex; gap:6px; align-items:center; flex-wrap:wrap;\">"
        "<span>⚡ 커스텀 매크로:</span>"
        "<div id=\"customMacroContainer\" style=\"display:inline-flex; flex-wrap:wrap; gap:2px;\"></div>"
        "<button onclick=\"addCustomMacro(); return false;\" style=\"background:#0284c7; color:#fff; border:none; padding:2px 8px; border-radius:4px; font-size:10px; font-weight:bold; cursor:pointer;\">+ 새 매크로 추가</button>"
        "</div>"
        "<div style=\"display:flex; gap:10px; align-items:center;\">"
        "<label style=\"cursor:pointer; display:flex; align-items:center; gap:4px; font-size:11px;\">"
        "<input type=\"checkbox\" id=\"ethAutoRepeatChk\" onchange=\"toggleAutoRepeat()\"> ⏱ 반복 전송"
        "</label>"
        "<select id=\"ethIntervalSelect\" onchange=\"if(document.getElementById('ethAutoRepeatChk').checked) toggleAutoRepeat()\" style=\"background:#1c2541; color:#5bc0be; border:1px solid #3a506b; border-radius:4px; font-size:11px; padding:2px 4px;\">"
        "<option value=\"1000\">1초</option>"
        "<option value=\"2000\" selected>2초</option>"
        "<option value=\"5000\">5초</option>"
        "</select>"
        "<div><span id=\"ethCharCounter\" style=\"color:#5bc0be; font-weight:bold;\">0</span> / 100 자</div>"
        "</div>"
        "</div>"

        // --- Section 3: Terminal Header & Real-time Log Display ---
        "<div style=\"display:flex; justify-content:space-between; align-items:center; margin-bottom:4px;\">"
        "<span style=\"font-size:11px; color:#94a3b8;\">📜 RS-232 터미널 송수신 실시간 로그 (HEX / ASCII 듀얼 디스플레이):</span>"
        "<div style=\"display:flex; gap:6px; align-items:center;\">"
        "<label style=\"font-size:10px; color:#94a3b8; cursor:pointer; display:flex; align-items:center; gap:3px;\"><input type=\"checkbox\" id=\"ethAutoScroll\" checked> 자동 스크롤</label>"
        "<button onclick=\"downloadEthLog(); return false;\" style=\"background:#0b132b; border:1px solid #0284c7; color:#38bdf8; padding:2px 8px; border-radius:4px; cursor:pointer; font-size:10px; font-weight:bold; transition:0.15s;\">💾 TXT 로그 다운로드</button>"
        "<button onclick=\"clearEthLog(); return false;\" style=\"background:#0b132b; border:1px solid #ef4444; color:#ef4444; padding:2px 8px; border-radius:4px; cursor:pointer; font-size:10px; font-weight:bold; transition:0.15s;\">🗑 터미널 클리어</button>"
        "</div>"
        "</div>"
        "<div id=\"ethCmdLog\" style=\"background:#0b132b; border:1px solid #3a506b; border-radius:6px; padding:10px; font-family:monospace; font-size:11px; height:150px; overflow-y:auto; color:#4ade80; line-height:1.4;\">[시스템] RS-232 UART 9600bps 하드웨어 통신 엔진 대기 중... (UART0: GP0/GP1 및 UART1: GP4/GP5 지원)</div>"
        "</div>"

        // 5. LED Control Section & Generic Fallback Card
        "<div class=\"card\" id=\"led_ctrl-section\" style=\"display: none; border: 2px solid #0284c7; background: #0f172a; padding: 20px;\">"
        "<div style=\"display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #334155; padding-bottom:12px; margin-bottom:16px;\">"
        "<div>"
        "<h3 style=\"margin:0; font-size:18px; color:#38bdf8; display:flex; align-items:center; gap:8px;\">💡 Pico 2 온보드 LED 하드웨어 제어 센터</h3>"
        "<div style=\"color:#94a3b8; font-size:11px; margin-top:3px;\">RP2350A ARM Cortex-M33 Pin GP25 (Onboard Green LED) • 50us 초저지연 타이머 엔진 연동</div>"
        "</div>"
        "<div style=\"display:flex; align-items:center; gap:12px; background:#1e293b; padding:6px 14px; border-radius:30px; border:1px solid #3a506b;\">"
        "<div id=\"ledBulbVisual\" style=\"width:18px; height:18px; border-radius:50%%; background:%s; border:2px solid %s; box-shadow:%s; transition:all 0.15s;\"></div>"
        "<span style=\"font-size:13px; font-weight:bold; color:%s;\" id=\"ledStateVal\">%s</span>"
        "</div>"
        "</div>"
        "<div style=\"display:grid; grid-template-columns:repeat(auto-fit, minmax(200px, 1fr)); gap:12px; margin-bottom:18px;\">"
        "<div style=\"background:#1e293b; padding:12px; border-radius:8px; border:1px solid #334155;\">"
        "<div style=\"font-size:11px; color:#94a3b8;\">현재 동작 모드 (Active Mode)</div>"
        "<div style=\"font-size:14px; font-weight:bold; color:#6fffe9; margin-top:4px;\" id=\"ledCurrentModeText\">%s</div>"
        "</div>"
        "<div style=\"background:#1e293b; padding:12px; border-radius:8px; border:1px solid #334155;\">"
        "<div style=\"font-size:11px; color:#94a3b8;\">점멸 주기 (Blink Period)</div>"
        "<div style=\"font-size:14px; font-weight:bold; color:#facc15; margin-top:4px;\" id=\"ledCurrentIntervalText\">%lu ms</div>"
        "</div>"
        "<div style=\"background:#1e293b; padding:12px; border-radius:8px; border:1px solid #334155;\">"
        "<div style=\"font-size:11px; color:#94a3b8;\">제어 GPIO 핀 할당</div>"
        "<div style=\"font-size:14px; font-weight:bold; color:#38bdf8; margin-top:4px;\">GP25 (Active High)</div>"
        "</div>"
        "</div>"
        "<div style=\"background:#111e38; padding:15px; border-radius:10px; border:1px solid #0284c7; margin-bottom:14px;\">"
        "<div style=\"font-size:13px; font-weight:bold; color:#38bdf8; margin-bottom:6px; display:flex; align-items:center; gap:6px;\">⚡ 1. 하트비트 점멸 주기 선택 (Auto Heartbeat)</div>"
        "<div style=\"font-size:11px; color:#94a3b8; margin-bottom:10px;\">보드 생존 상태를 알리는 하트비트 주기를 실시간으로 변경합니다.</div>"
        "<div style=\"display:flex; gap:8px; flex-wrap:wrap;\">"
        "<button onclick=\"setLedMode('blink', 100); return false;\" class=\"btn\" style=\"background:#0284c7; padding:8px 14px; font-size:12px;\">⚡ 100ms (초고속)</button>"
        "<button onclick=\"setLedMode('blink', 250); return false;\" class=\"btn\" style=\"background:#0284c7; padding:8px 14px; font-size:12px;\">⚡ 250ms (고속)</button>"
        "<button onclick=\"setLedMode('blink', 500); return false;\" class=\"btn\" style=\"background:#22c55e; padding:8px 14px; font-size:12px; font-weight:bold;\">⚡ 500ms (기본 표준)</button>"
        "<button onclick=\"setLedMode('blink', 1000); return false;\" class=\"btn\" style=\"background:#0284c7; padding:8px 14px; font-size:12px;\">⚡ 1000ms (여유)</button>"
        "<button onclick=\"setLedMode('blink', 2000); return false;\" class=\"btn\" style=\"background:#0284c7; padding:8px 14px; font-size:12px;\">⚡ 2000ms (저속)</button>"
        "</div>"
        "</div>"
        "<div style=\"display:grid; grid-template-columns:repeat(auto-fit, minmax(280px, 1fr)); gap:12px;\">"
        "<div style=\"background:#111e38; padding:15px; border-radius:10px; border:1px solid #334155;\">"
        "<div style=\"font-size:13px; font-weight:bold; color:#cbd5e1; margin-bottom:6px;\">💡 2. 수동 점등 / 소등 제어 (Manual Hold)</div>"
        "<div style=\"font-size:11px; color:#94a3b8; margin-bottom:10px;\">하트비트를 정지하고 LED를 켜거나 끕니다.</div>"
        "<div style=\"display:flex; gap:8px;\">"
        "<button onclick=\"setLedMode('on'); return false;\" class=\"btn btn-on\" style=\"flex:1; padding:10px; font-size:13px; font-weight:bold;\">🟢 상시 켜기 (ON)</button>"
        "<button onclick=\"setLedMode('off'); return false;\" class=\"btn btn-off\" style=\"flex:1; padding:10px; font-size:13px; font-weight:bold;\">🔴 상시 끄기 (OFF)</button>"
        "</div>"
        "</div>"
        "<div style=\"background:#111e38; padding:15px; border-radius:10px; border:1px solid #334155;\">"
        "<div style=\"font-size:13px; font-weight:bold; color:#cbd5e1; margin-bottom:6px;\">🚨 3. 특수 신호 모드 (Special Functions)</div>"
        "<div style=\"font-size:11px; color:#94a3b8; margin-bottom:10px;\">비상 신호 또는 서버 랙 내 위치 식별용 모드입니다.</div>"
        "<div style=\"display:flex; gap:8px;\">"
        "<button onclick=\"setLedMode('sos'); return false;\" class=\"btn\" style=\"flex:1; background:#ef4444; padding:10px; font-size:12px; font-weight:bold;\">🆘 SOS 모스부호</button>"
        "<button onclick=\"setLedMode('beacon'); return false;\" class=\"btn\" style=\"flex:1; background:#eab308; color:#000; padding:10px; font-size:12px; font-weight:bold;\">📍 기기 위치 식별</button>"
        "</div>"
        "</div>"
        "</div>"
        "</div>"
        "<div class=\"card\" id=\"generic-section\" style=\"display: none;\">"
        "<h3 id=\"generic-title\" style=\"color:#5bc0be;\">Setup</h3>"
        "<div style=\"color:#94a3b8; font-size:12px; padding:12px; background:#0b132b; border-radius:6px; border:1px solid #3a506b;\">"
        "🔧 현재 설정 항목이 정상 동작 중입니다."
        "</div>"
        "</div>"
        "</div>",
        led_state ? "#22c55e" : "#1e293b",
        led_state ? "#4ade80" : "#475569",
        led_state ? "0 0 25px #22c55e, 0 0 50px rgba(34,197,94,0.7)" : "none",
        led_state ? "#4ade80" : "#ef4444",
        led_state ? "ON 🟢" : "OFF 🔴",
        (g_led_mode == LED_MODE_BLINK) ? "⚡ 하트비트 점멸 (Auto Blink)" :
        (g_led_mode == LED_MODE_MANUAL_ON) ? "💡 상시 점등 (ON)" :
        (g_led_mode == LED_MODE_MANUAL_OFF) ? "🛑 상시 소등 (OFF)" :
        (g_led_mode == LED_MODE_SOS) ? "🆘 SOS 긴급 모스부호" : "📍 비컨 식별",
        (unsigned long)g_led_blink_interval_ms);

    // 5. Right Gauge Panel & Footer
    snprintf(buf + strlen(buf), max_len - strlen(buf),
        "<div class=\"gauge-panel\">"
        "<div class=\"gauge-card\"><div class=\"gauge-title\">Ambient Temperature</div>"
        "<svg viewBox=\"0 0 100 55\" width=\"120\" height=\"70\">"
        "<path d=\"M 10 50 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#334155\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<path d=\"M 10 50 A 40 40 0 0 1 70 20\" fill=\"none\" stroke=\"#22c55e\" stroke-width=\"10\"/>"
        "<path d=\"M 70 20 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#ef4444\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<line x1=\"50\" y1=\"50\" x2=\"50\" y2=\"18\" stroke=\"#5bc0be\" stroke-width=\"3\" stroke-linecap=\"round\" transform=\"rotate(%.1f, 50, 50)\"/>"
        "<circle cx=\"50\" cy=\"50\" r=\"4\" fill=\"#0284c7\"/>"
        "</svg><div class=\"gauge-val\">%.1f <span class=\"unit\">°C</span></div></div>"
        "<div class=\"gauge-card\"><div class=\"gauge-title\">Ext1 Temperature</div>"
        "<svg viewBox=\"0 0 100 55\" width=\"120\" height=\"70\">"
        "<path d=\"M 10 50 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#334155\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<path d=\"M 10 50 A 40 40 0 0 1 70 20\" fill=\"none\" stroke=\"#22c55e\" stroke-width=\"10\"/>"
        "<path d=\"M 70 20 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#ef4444\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<line x1=\"50\" y1=\"50\" x2=\"50\" y2=\"18\" stroke=\"#5bc0be\" stroke-width=\"3\" stroke-linecap=\"round\" transform=\"rotate(-2.0, 50, 50)\"/>"
        "<circle cx=\"50\" cy=\"50\" r=\"4\" fill=\"#0284c7\"/>"
        "</svg><div class=\"gauge-val\">28.5 <span class=\"unit\">°C</span></div></div>"
        "<div class=\"gauge-card\"><div class=\"gauge-title\">Ext2 Temperature</div>"
        "<svg viewBox=\"0 0 100 55\" width=\"120\" height=\"70\">"
        "<path d=\"M 10 50 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#334155\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<path d=\"M 10 50 A 40 40 0 0 1 70 20\" fill=\"none\" stroke=\"#22c55e\" stroke-width=\"10\"/>"
        "<path d=\"M 70 20 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#ef4444\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<line x1=\"50\" y1=\"50\" x2=\"50\" y2=\"18\" stroke=\"#5bc0be\" stroke-width=\"3\" stroke-linecap=\"round\" transform=\"rotate(-3.0, 50, 50)\"/>"
        "<circle cx=\"50\" cy=\"50\" r=\"4\" fill=\"#0284c7\"/>"
        "</svg><div class=\"gauge-val\">27.6 <span class=\"unit\">°C</span></div></div>"
        "<div class=\"gauge-card\"><div class=\"gauge-title\">Ambient Humidity</div>"
        "<svg viewBox=\"0 0 100 55\" width=\"120\" height=\"70\">"
        "<path d=\"M 10 50 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#334155\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<path d=\"M 10 50 A 40 40 0 0 1 90 50\" fill=\"none\" stroke=\"#5bc0be\" stroke-width=\"10\" stroke-linecap=\"round\"/>"
        "<line x1=\"50\" y1=\"50\" x2=\"50\" y2=\"18\" stroke=\"#5bc0be\" stroke-width=\"3\" stroke-linecap=\"round\" transform=\"rotate(-57.0, 50, 50)\"/>"
        "<circle cx=\"50\" cy=\"50\" r=\"4\" fill=\"#0284c7\"/>"
        "</svg><div class=\"gauge-val\">18 <span class=\"unit\">%%</span></div></div>"
        "</div></div>"
        "<div class=\"footer\">Copyright &copy; 2026 DisplayHub Co., Ltd. (This page optimized for Pico 2 Engine)</div>"
        "</body></html>",
        cpu_gauge_angle, cpu_temp);
}

static void url_decode(char *dst, const char *src, size_t max_dst_len) {
    size_t d = 0;
    while (*src && d + 1 < max_dst_len) {
        if (*src == '%' && src[1] && src[2] && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            dst[d++] = (char)strtoul(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

void http_server_process_request(uint8_t *request_buf, uint16_t req_len, float cpu_temp, bool *led_state) {
    if (req_len == 0) return;

    // Find body start before strtok or parsing
    const uint8_t *body_start = NULL;
    uint32_t body_in_req_buf = 0;
    for (uint16_t i = 0; i + 3 < req_len; i++) {
        if (request_buf[i] == '\r' && request_buf[i+1] == '\n' &&
            request_buf[i+2] == '\r' && request_buf[i+3] == '\n') {
            body_start = request_buf + i + 4;
            body_in_req_buf = req_len - (i + 4);
            break;
        }
    }

    if (strstr((const char*)request_buf, "GET /api/status") != NULL || strstr((const char*)request_buf, "GET /status") != NULL) {
        uint32_t uptime_sec = (uint32_t)(time_us_64() / 1000000ULL);
        char json_buf[256];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"version\":\"" FIRMWARE_VERSION "\",\"uptime_sec\":%lu,\"cpu_temp\":%.1f,\"led_state\":%d,\"led_mode\":%d,\"led_interval\":%lu,\"eth_chip\":\"" ETH_CHIP_NAME "\",\"board\":\"" BOARD_HW_NAME "\",\"ota_busy\":%s,\"rtd_busy\":%s}",
            (unsigned long)uptime_sec, cpu_temp, *led_state ? 1 : 0, (int)g_led_mode, (unsigned long)g_led_blink_interval_ms,
            g_pico_ota_busy ? "true" : "false", g_rtd_busy ? "true" : "false");

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /api/led") != NULL) {
        if (strstr((const char*)request_buf, "mode=blink") != NULL || strstr((const char*)request_buf, "mode=auto") != NULL) {
            g_led_mode = LED_MODE_BLINK;
        } else if (strstr((const char*)request_buf, "mode=on") != NULL || strstr((const char*)request_buf, "state=1") != NULL) {
            g_led_mode = LED_MODE_MANUAL_ON;
            *led_state = true;
            gpio_put(PIN_LED, 1);
        } else if (strstr((const char*)request_buf, "mode=off") != NULL || strstr((const char*)request_buf, "state=0") != NULL) {
            g_led_mode = LED_MODE_MANUAL_OFF;
            *led_state = false;
            gpio_put(PIN_LED, 0);
        } else if (strstr((const char*)request_buf, "mode=sos") != NULL) {
            g_led_mode = LED_MODE_SOS;
        } else if (strstr((const char*)request_buf, "mode=beacon") != NULL) {
            g_led_mode = LED_MODE_BEACON;
        }

        const char *p_interval = strstr((const char*)request_buf, "interval=");
        if (p_interval) {
            uint32_t val = (uint32_t)strtoul(p_interval + 9, NULL, 10);
            if (val >= 50 && val <= 10000) {
                g_led_blink_interval_ms = val;
                g_led_mode = LED_MODE_BLINK;
            }
        }

        char json_buf[160];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"success\":true,\"led_state\":%d,\"mode\":%d,\"interval\":%lu}",
            *led_state ? 1 : 0, (int)g_led_mode, (unsigned long)g_led_blink_interval_ms);
        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);
        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /api/clear_fw") != NULL || strstr((const char*)request_buf, "POST /api/clear_fw") != NULL) {
        g_fw_info.has_firmware = false;
        memset(g_fw_info.filename, 0, sizeof(g_fw_info.filename));
        g_fw_info.size_bytes = 0;
        strcpy(g_fw_info.big_bin_checksum, "미수행");
        strcpy(g_fw_info.sum32_checksum, "미수행");

        // Clear stored metadata in flash
        safe_flash_erase(FLASH_INFO_OFFSET, FLASH_SECTOR_SIZE);

        char json_buf[128];
        int json_len = snprintf(json_buf, sizeof(json_buf), "{\"success\":true,\"message\":\"cleared\"}");
        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);
        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /api/rtd/ping") != NULL) {
        bool connected = rtd_isp_ping();
        char json_buf[256];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"success\":true,\"connected\":%s,\"channel\":2,\"scl\":\"GP2\",\"sda\":\"GP3\",\"addr\":\"0x4A\","
            "\"message\":\"%s\"}",
            connected ? "true" : "false",
            connected ? "▶ Realtek 스케일러(0x4A) 통신 정상 (I2C ACK 응답 확인, 화면 정상 제어)" : "⚠ Realtek 스케일러(0x4A) 응답 없음 (GP2/GP3 배선 확인 필요)");

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /api/rtd/info") != NULL) {
        rtd_chip_info_t info;
        rtd_isp_read_chip_info(&info);

        char json_buf[512];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"success\":true,\"connected\":%s,\"channel\":2,\"scl\":\"GP2\",\"sda\":\"GP3\",\"addr\":\"0x4A\","
            "\"mfg_id\":\"0x%02X\",\"mem_type\":\"0x%02X\",\"cap_id\":\"0x%02X\","
            "\"mfg_name\":\"%s\",\"flash_model\":\"%s\",\"flash_size\":\"%s\","
            "\"message\":\"%s\"}",
            info.connected ? "true" : "false",
            info.mfg_id, info.mem_type, info.capacity_id,
            info.mfg_name, info.flash_model, info.flash_size_str,
            info.status_msg);

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /api/rtd/status") != NULL) {
        const rtd_flash_progress_t *prog = rtd_isp_get_progress();
        char json_buf[384];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"status\":%d,\"is_busy\":%s,\"progress\":%d,\"crc\":\"0x%02X\",\"message\":\"%s\"}",
            (int)prog->status, prog->is_busy ? "true" : "false", (int)prog->progress_percent, (unsigned int)prog->hardware_crc, prog->message);

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /api/rtd/flash") != NULL || strstr((const char*)request_buf, "POST /api/rtd/flash") != NULL) {
        if (g_pico_ota_busy) {
            char json_buf[256];
            int json_len = snprintf(json_buf, sizeof(json_buf),
                "{\"success\":false,\"message\":\"⚠️ Pico 2 이더넷 OTA가 진행 중입니다. 완료 후 다시 시도해주세요.\"}");
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 409 Conflict\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                json_len);
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
            w5500_disconnect_socket(0);
            return;
        }

        if (!g_fw_info.has_firmware || g_fw_info.size_bytes == 0) {
            char json_buf[256];
            int json_len = snprintf(json_buf, sizeof(json_buf),
                "{\"success\":false,\"message\":\"저장된 Realtek 펌웨어가 없습니다. 먼저 .bin 파일을 업로드해주세요.\"}");
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                json_len);
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
            w5500_disconnect_socket(0);
            return;
        }

        g_rtd_busy = true;
        bool flash_ok = rtd_isp_flash_from_storage(FLASH_RTD_BIN_OFFSET, g_fw_info.size_bytes);
        g_rtd_busy = false;
        const rtd_flash_progress_t *prog = rtd_isp_get_progress();

        char json_buf[512];
        int json_len = snprintf(json_buf, sizeof(json_buf),
            "{\"success\":%s,\"message\":\"%s\",\"crc\":\"0x%02X\",\"bytes\":%lu}",
            flash_ok ? "true" : "false", prog->message, (unsigned int)prog->hardware_crc, (unsigned long)g_fw_info.size_bytes);

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            json_len);

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)json_buf, json_len);
        w5500_disconnect_socket(0);
        return;
    }


    if (strstr((const char*)request_buf, "GET /api/command") != NULL || strstr((const char*)request_buf, "POST /api/command") != NULL) {
        char raw_cmd[128] = {0};
        char cmd_param[128] = {0};
        const char *cmd_ptr = strstr((const char*)request_buf, "cmd=");
        if (cmd_ptr) {
            cmd_ptr += 4;
            int idx = 0;
            while (*cmd_ptr != ' ' && *cmd_ptr != '&' && *cmd_ptr != '\r' && *cmd_ptr != '\n' && *cmd_ptr != '\0' && idx < 120) {
                raw_cmd[idx++] = *cmd_ptr++;
            }
            raw_cmd[idx] = '\0';
            url_decode(cmd_param, raw_cmd, sizeof(cmd_param));
        }

        // Dynamically parse Port parameter (uart0 / uart1)
        char port_param[10] = "uart0";
        const char *port_ptr = strstr((const char*)request_buf, "port=");
        if (port_ptr) {
            port_ptr += 5;
            int idx = 0;
            while (*port_ptr != ' ' && *port_ptr != '&' && *port_ptr != '\r' && *port_ptr != '\n' && *port_ptr != '\0' && idx < 9) {
                port_param[idx++] = *port_ptr++;
            }
            port_param[idx] = '\0';
        }

        uart_inst_t *target_uart = (strcmp(port_param, "uart1") == 0) ? uart1 : uart0;

        // Dynamically update UART Baud Rate if requested
        uint32_t baud_val = 9600;
        const char *baud_ptr = strstr((const char*)request_buf, "baud=");
        if (baud_ptr) {
            baud_ptr += 5;
            uint32_t parsed_b = (uint32_t)strtoul(baud_ptr, NULL, 10);
            if (parsed_b >= 1200 && parsed_b <= 921600) {
                baud_val = parsed_b;
            }
        }

        static bool uart0_initialized = false;
        static bool uart1_initialized = false;
        if (target_uart == uart0) {
            if (!uart0_initialized) {
                rs232_init(uart0, baud_val, 0, 1);
                uart0_initialized = true;
            } else {
                uart_set_baudrate(uart0, baud_val);
            }
        } else {
            if (!uart1_initialized) {
                rs232_init(uart1, baud_val, 4, 5);
                uart1_initialized = true;
            } else {
                uart_set_baudrate(uart1, baud_val);
            }
        }

        // Parse line ending (NONE, CRLF, CR, LF)
        char ending_param[10] = "NONE";
        const char *ending_ptr = strstr((const char*)request_buf, "ending=");
        if (ending_ptr) {
            ending_ptr += 7;
            int idx = 0;
            while (*ending_ptr != ' ' && *ending_ptr != '&' && *ending_ptr != '\r' && *ending_ptr != '\n' && *ending_ptr != '\0' && idx < 9) {
                ending_param[idx++] = *ending_ptr++;
            }
            ending_param[idx] = '\0';
        }

        // Parse timeout (in ms, default 2000ms)
        uint32_t timeout_val = 2000;
        const char *to_ptr = strstr((const char*)request_buf, "timeout=");
        if (to_ptr) {
            to_ptr += 8;
            uint32_t parsed_to = (uint32_t)strtoul(to_ptr, NULL, 10);
            if (parsed_to >= 100 && parsed_to <= 10000) {
                timeout_val = parsed_to;
            }
        }

        uint8_t rx_buf[RS232_RX_BUFFER_SIZE];
        size_t rx_len = 0;
        uint32_t latency_ms = 0;

        bool got_response = rs232_send_command(target_uart,
                                               cmd_param,
                                               ending_param,
                                               timeout_val,
                                               rx_buf,
                                               sizeof(rx_buf),
                                               &rx_len,
                                               &latency_ms);

        char resp_buf[768];
        char safe_cmd[128] = {0};
        for (size_t i = 0; i < strlen(cmd_param) && i < 100; i++) {
            safe_cmd[i] = (cmd_param[i] == '"' || cmd_param[i] == '\\') ? '_' : cmd_param[i];
        }

        if (got_response) {
            char hex_str[512] = {0};
            char ascii_str[128] = {0};
            int show_len = (rx_len < 64) ? (int)rx_len : 64;
            for (int i = 0; i < show_len; i++) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%02X ", rx_buf[i]);
                strcat(hex_str, tmp);
                char ch = (char)rx_buf[i];
                if (ch == '"' || ch == '\\') {
                    ascii_str[i] = '.';
                } else if (ch >= 32 && ch <= 126) {
                    ascii_str[i] = ch;
                } else {
                    ascii_str[i] = '.';
                }
            }
            ascii_str[show_len] = '\0';
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"status\":\"OK\",\"port\":\"%s\",\"baud\":%lu,\"command\":\"%s\",\"rx_bytes\":%d,\"latency_ms\":%lu,\"hex\":\"%s\",\"ascii\":\"%s\"}",
                     port_param, (unsigned long)baud_val,
                     safe_cmd[0] ? safe_cmd : "NONE",
                     (int)rx_len, (unsigned long)latency_ms,
                     hex_str, ascii_str);
        } else {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"status\":\"OK\",\"port\":\"%s\",\"baud\":%lu,\"command\":\"%s\",\"rx_bytes\":0,\"latency_ms\":%lu,\"response\":\"(No UART RX within %.1fs)\"}",
                     port_param, (unsigned long)baud_val,
                     safe_cmd[0] ? safe_cmd : "NONE",
                     (unsigned long)latency_ms,
                     (float)timeout_val / 1000.0f);
        }

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            (int)strlen(resp_buf));

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)resp_buf, strlen(resp_buf));
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "GET /led/on") != NULL) {
        *led_state = true;
        gpio_put(PIN_LED, 1);
    } else if (strstr((const char*)request_buf, "GET /led/off") != NULL) {
        *led_state = false;
        gpio_put(PIN_LED, 0);
    }

    if (strstr((const char*)request_buf, "POST /upload_pico_fw") != NULL || strstr((const char*)request_buf, "POST /upload_ota") != NULL) {
        if (g_rtd_busy) {
            printf("⚠️ [이더넷 OTA 거부] Realtek 펌웨어 작업 진행 중!\n");
            char resp_msg[256];
            snprintf(resp_msg, sizeof(resp_msg), "⚠️ Realtek 스케일러 작업(업로드/플래싱)이 진행 중입니다. 작업 완료 후 다시 시도해주세요.");
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 409 Conflict\r\nContent-Type: text/plain; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp_msg));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
            return;
        }

        g_pico_ota_busy = true;

        // High-Speed Pico 2 Ethernet OTA Firmware Flashing Handler
        uint32_t content_length = parse_content_length((const char*)request_buf);
        char orig_filename[128];
        parse_upload_filename((const char*)request_buf, orig_filename, sizeof(orig_filename));

        if (content_length == 0 || content_length > FLASH_OTA_MAX_SIZE) {
            g_pico_ota_busy = false;
            char resp_msg[256];
            snprintf(resp_msg, sizeof(resp_msg), "⚠️ OTA 업로드 용량 오류! (%lu Bytes, 최대 1MB 허용)", (unsigned long)content_length);
            char header[256];
            int header_len = snprintf(header, sizeof(header), "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(resp_msg));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
            return;
        }

        // 🛡️ 1. Filename opposite-chip pre-check
        if (ci_strstr(orig_filename, OPPOSITE_CHIP_NAME) != NULL) {
            g_pico_ota_busy = false;
            printf("❌ [OTA 거부] 타겟 칩셋 불일치! (%s -> %s 전용 바이너리 의심)\n", orig_filename, OPPOSITE_CHIP_NAME);
            char resp_msg[384];
            snprintf(resp_msg, sizeof(resp_msg),
                "❌ [이더넷 OTA 업로드 거부 - 칩셋 불일치]\n\n"
                "현재 장비는 " ETH_CHIP_NAME " 보드입니다.\n"
                "선택하신 파일(%s)은 " OPPOSITE_CHIP_NAME " 전용 펌웨어로 감지되었습니다.\n\n"
                "이더넷 통신 불가(벽돌 현상)를 방지하기 위해 업로드가 사전 차단되었습니다.\n"
                "👉 " ETH_CHIP_NAME " 전용 펌웨어를 선택해주세요.",
                orig_filename);
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp_msg));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
            return;
        }

        printf("⚡ [이더넷 OTA 수신 시작] %s (%lu Bytes) -> Staging Bank (0x10080000)\n", orig_filename, (unsigned long)content_length);

        // -------------------------------------------------------------------
        // 🛡️ Pico 2 (RP2350) OTA .BIN Only Guard
        // -------------------------------------------------------------------
        if (body_start && body_in_req_buf >= 8) {
            uint32_t magic_uf2  = (uint32_t)body_start[0] | ((uint32_t)body_start[1] << 8) | ((uint32_t)body_start[2] << 16) | ((uint32_t)body_start[3] << 24);
            uint32_t initial_sp = magic_uf2;
            bool is_uf2 = (magic_uf2 == 0x0A324655ULL) || ci_strstr(orig_filename, ".uf2") != NULL;
            bool is_rp2350_picobin = (initial_sp == 0x4D535052ULL || initial_sp == 0xFFFFEDACULL || (initial_sp >= 0x20000000ULL && initial_sp <= 0x200B0000ULL));
            bool is_realtek = (strstr(orig_filename, "dh9") != NULL || strstr(orig_filename, "DH9") != NULL || strstr(orig_filename, "rlt") != NULL || ((uint8_t)body_start[0] == 0x02 || (uint8_t)body_start[0] == 0x12));

            if (is_uf2) {
                g_pico_ota_busy = false;
                printf("⚠️ [이더넷 OTA 거부] .UF2 파일 감지! (이더넷 OTA는 .BIN 전용)\n");
                char resp_msg[384];
                snprintf(resp_msg, sizeof(resp_msg),
                    "⚠️ 이더넷 OTA 업로드 거부! (.uf2 파일 선택됨)\n\n"
                    "선택하신 파일은 USB BOOTSEL 복사용 .uf2 파일입니다.\n"
                    "이더넷 OTA 원격 업데이트는 100%% 안정성을 위해 순수 바이너리(.bin)만 지원합니다.\n\n"
                    "👉 build 폴더의 " ETH_CHIP_NAME "_pico2_firmware.bin 파일을 선택해주세요!");

                char header[256];
                int header_len = snprintf(header, sizeof(header),
                    "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                     (int)strlen(resp_msg));

                w5500_send_tx_data(0, (const uint8_t*)header, header_len);
                w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
                w5500_disconnect_socket(0);
                return;
            }

            if (!is_rp2350_picobin && is_realtek) {
                g_pico_ota_busy = false;
                printf("⚠️ [이더넷 OTA 거부] 올바른 Pico 2 ARM 펌웨어가 아닙니다! (Realtek BIN 감지)\n");
                char resp_msg[384];
                snprintf(resp_msg, sizeof(resp_msg),
                    "⚠️ 이더넷 OTA 업로드 거부!\n\n"
                    "선택하신 파일은 Pico 2 (RP2350) ARM 실행 바이너리가 아닙니다.\n"
                    "(Realtek 스케일러 펌웨어 파일 감지됨)\n\n"
                    "👉 Realtek 스케일러 펌웨어(.bin)는 상단의 [💾 Realtek 펌웨어(.bin) 업로드] 카드를 이용해주세요!");

                char header[256];
                int header_len = snprintf(header, sizeof(header),
                    "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                     (int)strlen(resp_msg));

                w5500_send_tx_data(0, (const uint8_t*)header, header_len);
                w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
                w5500_disconnect_socket(0);
                return;
            }
        }

        bool is_uf2 = (body_start && body_in_req_buf >= 4 &&
                       (((uint32_t)body_start[0] | ((uint32_t)body_start[1] << 8) |
                         ((uint32_t)body_start[2] << 16) | ((uint32_t)body_start[3] << 24)) == 0x0A324655ULL));

        reset_ota_sector_erase_tracker();

        uint32_t total_sum = 0;
        uint32_t received = 0;
        uint32_t payload_written = 0;
        bool upload_success = false;

        // 🛡️ 2. Binary Stream Signature Scanner (UF2 & Pure BIN)
        bool chip_mismatch = false;
        uint8_t carry_buf[64];
        uint32_t carry_len = 0;
        memset(carry_buf, 0, sizeof(carry_buf));
        char opp_tag[32];
        get_opposite_chip_tag(opp_tag, sizeof(opp_tag));

        if (is_uf2) {
            // UF2 Stream Processing: Accumulate 512-byte blocks and program 256-byte payloads
            static uint8_t uf2_block[512];
            uint32_t uf2_idx = 0;

            if (body_start && body_in_req_buf > 0) {
                uint32_t copy_len = (body_in_req_buf > content_length) ? content_length : body_in_req_buf;
                process_uf2_chunk((const uint8_t*)body_start, copy_len, uf2_block, &uf2_idx, &total_sum, &payload_written, &chip_mismatch, carry_buf, &carry_len, opp_tag);
                received = copy_len;
            }

            static uint8_t rx_chunk[2048];
            uint64_t start_t = time_us_64();
            while (!chip_mismatch && received < content_length) {
                uint16_t avail = w5500_rx_bytes_available(0);
                if (avail > 0) {
                    uint16_t n = w5500_read_rx_data(0, rx_chunk, sizeof(rx_chunk));
                    if (n > 0) {
                        uint32_t to_process = (received + n > content_length) ? (content_length - received) : n;
                        process_uf2_chunk(rx_chunk, to_process, uf2_block, &uf2_idx, &total_sum, &payload_written, &chip_mismatch, carry_buf, &carry_len, opp_tag);
                        if (chip_mismatch) break;
                        received += to_process;
                        start_t = time_us_64();
                    }
                } else {
                    uint8_t status = w5500_get_socket_status(0);
                    if (status == SOCK_CLOSED) break;
                    if (status == SOCK_CLOSE_WAIT) {
                        sleep_ms(10);
                        if (w5500_rx_bytes_available(0) == 0) break;
                    }
                    if (time_us_64() - start_t > 30000000ULL) break;
                    sleep_us(50);
                }
            }

            if (!chip_mismatch && uf2_idx > 0 && uf2_idx < 512) {
                uint8_t zero_pad[512] = {0};
                uint32_t pad_len = 512 - uf2_idx;
                process_uf2_chunk(zero_pad, pad_len, uf2_block, &uf2_idx, &total_sum, &payload_written, &chip_mismatch, carry_buf, &carry_len, opp_tag);
            }

            if (!chip_mismatch && received == content_length && payload_written > 0) {
                upload_success = true;
                received = payload_written; // Actual extracted BIN size
            }
        } else {
            // Pure BIN Processing
            static uint8_t page_write_buf[FLASH_PAGE_SIZE];
            uint32_t page_buf_idx = 0;
            uint32_t flash_write_offset = FLASH_OTA_STAGING_OFFSET;
            const uint8_t *ubody = (const uint8_t*)body_start;

            if (ubody && body_in_req_buf > 0) {
                uint32_t copy_len = (body_in_req_buf > content_length) ? content_length : body_in_req_buf;
                if (check_tag_with_carry(ubody, copy_len, carry_buf, &carry_len, opp_tag)) {
                    chip_mismatch = true;
                }
                if (!chip_mismatch) {
                    for (uint32_t i = 0; i < copy_len; i++) {
                        total_sum += ubody[i];
                        page_write_buf[page_buf_idx++] = ubody[i];
                        if (page_buf_idx >= FLASH_PAGE_SIZE) {
                            ensure_ota_sector_erased(flash_write_offset);
                            safe_flash_program(flash_write_offset, page_write_buf, FLASH_PAGE_SIZE);
                            flash_write_offset += FLASH_PAGE_SIZE;
                            page_buf_idx = 0;
                            sleep_us(100);
                        }
                    }
                    received = copy_len;
                }
            }

            static uint8_t rx_chunk[2048];
            uint64_t start_t = time_us_64();
            while (!chip_mismatch && received < content_length) {
                uint16_t avail = w5500_rx_bytes_available(0);
                if (avail > 0) {
                    uint16_t n = w5500_read_rx_data(0, rx_chunk, sizeof(rx_chunk));
                    if (n > 0) {
                        uint32_t to_process = (received + n > content_length) ? (content_length - received) : n;
                        if (check_tag_with_carry(rx_chunk, to_process, carry_buf, &carry_len, opp_tag)) {
                            chip_mismatch = true;
                            break;
                        }
                        for (uint16_t i = 0; i < to_process; i++) {
                            total_sum += rx_chunk[i];
                            page_write_buf[page_buf_idx++] = rx_chunk[i];
                            if (page_buf_idx >= FLASH_PAGE_SIZE) {
                                ensure_ota_sector_erased(flash_write_offset);
                                safe_flash_program(flash_write_offset, page_write_buf, FLASH_PAGE_SIZE);
                                flash_write_offset += FLASH_PAGE_SIZE;
                                page_buf_idx = 0;
                                sleep_us(100);
                            }
                        }
                        received += to_process;
                        start_t = time_us_64();
                    }
                } else {
                    uint8_t status = w5500_get_socket_status(0);
                    if (status == SOCK_CLOSED) break;
                    if (status == SOCK_CLOSE_WAIT) {
                        sleep_ms(10);
                        if (w5500_rx_bytes_available(0) == 0) break;
                    }
                    if (time_us_64() - start_t > 30000000ULL) break;
                    sleep_us(50);
                }
            }

            if (!chip_mismatch && page_buf_idx > 0) {
                memset(page_write_buf + page_buf_idx, 0xFF, FLASH_PAGE_SIZE - page_buf_idx);
                ensure_ota_sector_erased(flash_write_offset);
                safe_flash_program(flash_write_offset, page_write_buf, FLASH_PAGE_SIZE);
            }

            if (!chip_mismatch && received == content_length) {
                upload_success = true;
            }
        }

        if (chip_mismatch) {
            g_pico_ota_busy = false;
            printf("❌ [OTA 차단] 반대 칩셋 바이너리 서명(%s) 감지됨! 플래시 무효화 및 거부 응답 전송...\n", opp_tag);
            // 🛡️ 첫 번째 스테이징 섹터를 즉시 소거하여 부분 쓰기된 바이너리를 무효화
            safe_flash_erase(FLASH_OTA_STAGING_OFFSET, FLASH_SECTOR_SIZE);

            char resp_msg[512];
            snprintf(resp_msg, sizeof(resp_msg),
                "❌ [이더넷 OTA 업로드 거부 - 바이너리 서명 불일치!]\n\n"
                "수신된 펌웨어 내부에서 %s 전용 하드웨어 서명(%s)이 검출되었습니다!\n\n"
                "현재 보드는 " ETH_CHIP_NAME " 전용 보드입니다.\n"
                "파일명이 변경되었더라도 내부 칩셋 서명 사전 검사로 안전하게 차단되었으며, 플래시 쓰기는 즉시 취소되었습니다.\n\n"
                "👉 올바른 " ETH_CHIP_NAME " 전용 펌웨어를 업로드해주세요.",
                OPPOSITE_CHIP_NAME, opp_tag);

            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp_msg));

            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
            return;
        }

        if (upload_success) {
            printf("🎉 [이더넷 OTA 전송 성공] %s (%lu Bytes) 페이로드 플래시 완료! 브라우저 HTTP 200 OK 응답 전송...\n", orig_filename, (unsigned long)received);

            char resp_msg[512];
            snprintf(resp_msg, sizeof(resp_msg),
                "🚀 Pico 2 이더넷 OTA 펌웨어 업로드 성공!\n"
                "- 파일명: %s\n"
                "- 추출 전송 용량: %lu Bytes (%.1f KB)\n"
                "- 32-bit Sum Checksum: 0x%08X\n\n"
                "⚡ 3초 후 Pico 2가 자동 재부팅되어 새 펌웨어가 덮어씌워지고 가동됩니다!\n\n",
                orig_filename, (unsigned long)received, (float)received/1024.0f, (unsigned int)total_sum);

            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp_msg));

            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));

            // Write OTA Metadata Header to FLASH_OTA_META_OFFSET after HTTP response is flushed
            pico_ota_header_t ota_meta;
            memset(&ota_meta, 0, sizeof(ota_meta));
            ota_meta.magic = FLASH_OTA_MAGIC;
            ota_meta.fw_size = received;
            ota_meta.checksum_sum32 = total_sum;
            strncpy(ota_meta.filename, orig_filename, sizeof(ota_meta.filename) - 1);

            static uint8_t meta_sector[FLASH_SECTOR_SIZE] __attribute__((aligned(256)));
            memset(meta_sector, 0xFF, sizeof(meta_sector));
            memcpy(meta_sector, &ota_meta, sizeof(ota_meta));

            safe_flash_erase(FLASH_OTA_META_OFFSET, FLASH_SECTOR_SIZE);
            safe_flash_program(FLASH_OTA_META_OFFSET, meta_sector, FLASH_SECTOR_SIZE);

            // Clean TCP disconnect and wait 1000ms to allow browser to receive 200 OK
            w5500_disconnect_socket(0);
            sleep_ms(1000);

            // One-step direct SRAM Flash overwrite and single hardware reboot
            apply_ota_firmware_now();
            return;
        } else {
            g_pico_ota_busy = false;
            char resp_msg[256];
            snprintf(resp_msg, sizeof(resp_msg), "⚠️ 이더넷 OTA 업로드 실패! (수신됨: %lu / %lu Bytes)", (unsigned long)received, (unsigned long)content_length);
            char header[256];
            int header_len = snprintf(header, sizeof(header), "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(resp_msg));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
            return;
        }
    }

    if (strstr((const char*)request_buf, "POST /upload_start") != NULL) {
        if (g_pico_ota_busy) {
            printf("⚠️ [Realtek 업로드 거부] Pico 2 이더넷 OTA 진행 중!\n");
            const char *resp = "{\"success\":false,\"error\":\"⚠️ Pico 2 이더넷 OTA가 진행 중입니다. 완료 후 다시 시도해주세요.\"}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 409 Conflict\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            sleep_ms(20);
            w5500_disconnect_socket(0);
            sleep_ms(10);
            w5500_close_socket(0);
            w5500_listen_server(HTTP_PORT);
            return;
        }

        uint32_t total_size = parse_query_uint32((const char*)request_buf, "size");
        if (total_size == 0) total_size = parse_content_length((const char*)request_buf);
        parse_upload_filename((const char*)request_buf, s_chunk_filename, sizeof(s_chunk_filename));

        // 🛡️ Block Pico 2 / ethernet chipset binaries from being uploaded to Realtek Scaler Flash
        if (ci_strstr(s_chunk_filename, "pico2") != NULL ||
            ci_strstr(s_chunk_filename, "w5500") != NULL ||
            ci_strstr(s_chunk_filename, "w6300") != NULL ||
            ci_strstr(s_chunk_filename, ".uf2") != NULL) {
            printf("❌ [Realtek 거부] Pico 2 보드 전용 파일 감지! (%s)\n", s_chunk_filename);
            const char *resp = "{\"success\":false,\"error\":\"Pico 2 메인보드 전용 펌웨어는 Realtek 스케일러 영역에 업로드할 수 없습니다.\"}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            sleep_ms(20);
            w5500_disconnect_socket(0);
            return;
        }

        if (total_size > 0 && total_size <= FLASH_RTD_MAX_SIZE) {
            g_rtd_busy = true;
            s_chunk_expected_size = total_size;
            s_chunk_received_bytes = 0;
            s_chunk_total_sum = 0;

            printf("⚡ [Chunked Upload 시작] 파일: %s, 용량: %lu Bytes\n",
                s_chunk_filename, (unsigned long)total_size);

            const char *resp = "{\"success\":true}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));

            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            w5500_disconnect_socket(0);
            return;
        } else {
            g_rtd_busy = false;
            const char *resp = "{\"success\":false,\"error\":\"Invalid file size\"}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            sleep_ms(20);
            w5500_disconnect_socket(0);
            return;
        }
    }

    if (strstr((const char*)request_buf, "POST /upload_chunk") != NULL) {
        uint32_t offset = parse_query_uint32((const char*)request_buf, "offset");
        uint32_t chunk_len = parse_content_length((const char*)request_buf);
        if (chunk_len == 0) chunk_len = parse_query_uint32((const char*)request_buf, "size");

        if (chunk_len > 0 && offset + chunk_len <= FLASH_RTD_MAX_SIZE) {
            static uint8_t chunk_buf[65536];
            uint32_t received = 0;

            if (body_start && body_in_req_buf > 0) {
                uint32_t copy_len = (body_in_req_buf > chunk_len) ? chunk_len : body_in_req_buf;
                memcpy(chunk_buf, body_start, copy_len);
                received = copy_len;
            }

            uint64_t start_t = time_us_64();
            while (received < chunk_len) {
                uint16_t avail = w5500_rx_bytes_available(0);
                if (avail > 0) {
                    uint16_t to_read = ((chunk_len - received) > 2048) ? 2048 : (chunk_len - received);
                    uint16_t n = w5500_read_rx_data(0, chunk_buf + received, to_read);
                    if (n > 0) {
                        received += n;
                        start_t = time_us_64();
                    }
                } else {
                    uint8_t status = w5500_get_socket_status(0);
                    if (status == SOCK_CLOSED || status == SOCK_CLOSE_WAIT) {
                        if (w5500_rx_bytes_available(0) == 0) break;
                    }
                    if (time_us_64() - start_t > 5000000ULL) break; // 5s chunk timeout
                    sleep_us(50);
                }
            }

            if (received == chunk_len) {
                // 🛡️ Inspect first chunk (offset 0) to ensure it's not a Pico 2 ARM / UF2 binary
                if (offset == 0 && chunk_len >= 8) {
                    uint32_t magic = (uint32_t)chunk_buf[0] | ((uint32_t)chunk_buf[1] << 8) | ((uint32_t)chunk_buf[2] << 16) | ((uint32_t)chunk_buf[3] << 24);
                    bool is_uf2 = (magic == 0x0A324655ULL);
                    bool is_arm_sp = (magic == 0x4D535052ULL || magic == 0xFFFFEDACULL || (magic >= 0x20000000ULL && magic <= 0x200B0000ULL));
                    if (is_uf2 || is_arm_sp) {
                        g_rtd_busy = false;
                        printf("❌ [Realtek 거부] 첫 청크에서 Pico 2 ARM 바이너리 감지! (Magic: 0x%08X)\n", (unsigned int)magic);
                        const char *resp = "{\"success\":false,\"error\":\"Pico 2 ARM 실행 바이너리는 Realtek 스케일러 영역에 업로드할 수 없습니다.\"}";
                        char header[256];
                        int header_len = snprintf(header, sizeof(header),
                            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                            (int)strlen(resp));
                        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
                        w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
                        sleep_ms(20);
                        w5500_disconnect_socket(0);
                        return;
                    }
                }

                // Erase this 32KB chunk area before programming
                uint32_t erase_len = (chunk_len + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
                safe_flash_erase(FLASH_RTD_BIN_OFFSET + offset, erase_len);

                // Program chunk to flash in 256B pages
                for (uint32_t p = 0; p < chunk_len; p += FLASH_PAGE_SIZE) {
                    uint32_t page_len = ((chunk_len - p) >= FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : (chunk_len - p);
                    static uint8_t page_buf[FLASH_PAGE_SIZE];
                    memset(page_buf, 0xFF, FLASH_PAGE_SIZE);
                    memcpy(page_buf, chunk_buf + p, page_len);
                    safe_flash_program(FLASH_RTD_BIN_OFFSET + offset + p, page_buf, FLASH_PAGE_SIZE);
                }

                for (uint32_t i = 0; i < chunk_len; i++) {
                    s_chunk_total_sum += chunk_buf[i];
                }
                s_chunk_received_bytes += chunk_len;

                // Visual feedback: toggle LED
                gpio_put(PIN_LED, (offset >> 14) & 1);

                char resp[128];
                snprintf(resp, sizeof(resp), "{\"success\":true,\"offset\":%lu,\"written\":%lu}", (unsigned long)offset, (unsigned long)chunk_len);
                char header[256];
                int header_len = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                    (int)strlen(resp));

                w5500_send_tx_data(0, (const uint8_t*)header, header_len);
                w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
                w5500_disconnect_socket(0);
                return;
            }
        }

        g_rtd_busy = false;
        const char *resp = "{\"success\":false,\"error\":\"Chunk write failed\"}";
        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            (int)strlen(resp));
        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
        sleep_ms(20);
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "POST /upload_finish") != NULL) {
        g_rtd_busy = false;
        parse_upload_filename((const char*)request_buf, s_chunk_filename, sizeof(s_chunk_filename));
        g_fw_info.has_firmware = true;
        strncpy(g_fw_info.filename, s_chunk_filename, sizeof(g_fw_info.filename) - 1);
        g_fw_info.size_bytes = s_chunk_received_bytes;
        extract_big_bin_checksum(s_chunk_filename, s_chunk_total_sum, g_fw_info.big_bin_checksum, sizeof(g_fw_info.big_bin_checksum));

        uint16_t h = (s_chunk_total_sum >> 16) & 0xFFFF;
        uint16_t l = s_chunk_total_sum & 0xFFFF;
        snprintf(g_fw_info.sum32_checksum, sizeof(g_fw_info.sum32_checksum), "%04X %04X (0x%08X)", h, l, (unsigned int)s_chunk_total_sum);

        save_firmware_info_to_flash();

        char resp_msg[512];
        snprintf(resp_msg, sizeof(resp_msg),
            "{\"success\":true,\"filename\":\"%s\",\"size\":%lu,\"size_kb\":\"%.1f\",\"big_bin\":\"%s\",\"sum32\":\"%s\"}",
            s_chunk_filename, (unsigned long)g_fw_info.size_bytes, (float)g_fw_info.size_bytes / 1024.0f,
            g_fw_info.big_bin_checksum, g_fw_info.sum32_checksum);

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            (int)strlen(resp_msg));

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
        w5500_disconnect_socket(0);
        return;
    }

    if (strstr((const char*)request_buf, "POST /upload") != NULL) {
        if (g_pico_ota_busy) {
            printf("⚠️ [Realtek 업로드 거부] Pico 2 이더넷 OTA 진행 중!\n");
            const char *resp = "{\"success\":false,\"error\":\"⚠️ Pico 2 이더넷 OTA가 진행 중입니다. 완료 후 다시 시도해주세요.\"}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 409 Conflict\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            sleep_ms(20);
            w5500_disconnect_socket(0);
            return;
        }

        if (g_rtd_busy) {
            printf("⚠️ [Realtek 업로드 거부] 이미 다른 업로드 작업 진행 중!\n");
            const char *resp = "{\"success\":false,\"error\":\"⚠️ Realtek 펌웨어 업로드 작업이 이미 진행 중입니다.\"}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 409 Conflict\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            sleep_ms(20);
            w5500_disconnect_socket(0);
            return;
        }

        g_rtd_busy = true;

        // High-Speed POST firmware upload handler
        uint32_t content_length = parse_content_length((const char*)request_buf);
        char orig_filename[128];
        parse_upload_filename((const char*)request_buf, orig_filename, sizeof(orig_filename));

        // 🛡️ Block Pico 2 / ethernet chipset binaries
        if (ci_strstr(orig_filename, "pico2") != NULL ||
            ci_strstr(orig_filename, "w5500") != NULL ||
            ci_strstr(orig_filename, "w6300") != NULL ||
            ci_strstr(orig_filename, ".uf2") != NULL) {
            g_rtd_busy = false;
            printf("❌ [Realtek 거부] Pico 2 보드 전용 파일 감지! (%s)\n", orig_filename);
            const char *resp = "{\"success\":false,\"error\":\"Pico 2 메인보드 전용 펌웨어는 Realtek 스케일러 영역에 업로드할 수 없습니다.\"}";
            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp));
            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
            sleep_ms(20);
            w5500_disconnect_socket(0);
            return;
        }

        // 🛡️ Inspect first bytes in body_start for UF2 or ARM stack pointer
        if (body_start && body_in_req_buf >= 8) {
            uint32_t magic = (uint32_t)body_start[0] | ((uint32_t)body_start[1] << 8) | ((uint32_t)body_start[2] << 16) | ((uint32_t)body_start[3] << 24);
            bool is_uf2 = (magic == 0x0A324655ULL);
            bool is_arm_sp = (magic == 0x4D535052ULL || magic == 0xFFFFEDACULL || (magic >= 0x20000000ULL && magic <= 0x200B0000ULL));
            if (is_uf2 || is_arm_sp) {
                g_rtd_busy = false;
                printf("❌ [Realtek 거부] Pico 2 ARM 바이너리 감지! (Magic: 0x%08X)\n", (unsigned int)magic);
                const char *resp = "{\"success\":false,\"error\":\"Pico 2 ARM 실행 바이너리는 Realtek 스케일러 영역에 업로드할 수 없습니다.\"}";
                char header[256];
                int header_len = snprintf(header, sizeof(header),
                    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                    (int)strlen(resp));
                w5500_send_tx_data(0, (const uint8_t*)header, header_len);
                w5500_send_tx_data(0, (const uint8_t*)resp, strlen(resp));
                sleep_ms(20);
                w5500_disconnect_socket(0);
                return;
            }
        }

        uint32_t total_sum = 0;
        uint32_t received = 0;
        bool upload_success = false;

        reset_rtd_sector_erase_tracker();

        if (content_length > 0 && content_length <= FLASH_RTD_MAX_SIZE) {
            static uint8_t page_write_buf[FLASH_PAGE_SIZE];
            uint32_t page_buf_idx = 0;
            uint32_t flash_write_offset = FLASH_RTD_BIN_OFFSET;
            uint32_t last_log_kb = 0;

            printf("⚡ [Realtek FW 초고속 스트리밍 시작] %s (%lu Bytes / %.1f KB)\n",
                orig_filename, (unsigned long)content_length, (float)content_length / 1024.0f);

            // Process initial binary payload bytes already in request_buf
            if (body_start && body_in_req_buf > 0) {
                uint32_t copy_len = (body_in_req_buf > content_length) ? content_length : body_in_req_buf;
                for (uint32_t i = 0; i < copy_len; i++) {
                    uint8_t b = (uint8_t)body_start[i];
                    total_sum += b;
                    page_write_buf[page_buf_idx++] = b;
                    if (page_buf_idx >= FLASH_PAGE_SIZE) {
                        if (flash_write_offset + FLASH_PAGE_SIZE <= FLASH_RTD_BIN_OFFSET + FLASH_RTD_MAX_SIZE) {
                            ensure_rtd_sector_erased(flash_write_offset);
                            safe_flash_program(flash_write_offset, page_write_buf, FLASH_PAGE_SIZE);
                            flash_write_offset += FLASH_PAGE_SIZE;
                        }
                        page_buf_idx = 0;
                    }
                }
                received = copy_len;
            }

            static uint8_t rx_chunk[4096];
            uint64_t start_t = time_us_64();
            while (received < content_length) {
                uint16_t avail = w5500_rx_bytes_available(0);
                if (avail > 0) {
                    uint16_t to_read = (avail > sizeof(rx_chunk)) ? sizeof(rx_chunk) : avail;
                    uint16_t n = w5500_read_rx_data(0, rx_chunk, to_read);
                    if (n > 0) {
                        for (uint16_t i = 0; i < n; i++) {
                            uint8_t b = rx_chunk[i];
                            total_sum += b;
                            page_write_buf[page_buf_idx++] = b;
                            if (page_buf_idx >= FLASH_PAGE_SIZE) {
                                if (flash_write_offset + FLASH_PAGE_SIZE <= FLASH_RTD_BIN_OFFSET + FLASH_RTD_MAX_SIZE) {
                                    ensure_rtd_sector_erased(flash_write_offset);
                                    safe_flash_program(flash_write_offset, page_write_buf, FLASH_PAGE_SIZE);
                                    flash_write_offset += FLASH_PAGE_SIZE;
                                    page_buf_idx = 0;
                                    sleep_us(100);
                                }
                            }
                        }
                        received += n;
                        start_t = time_us_64();

                        // Visual feedback: toggle LED every 32KB received
                        gpio_put(PIN_LED, (received >> 15) & 1);

                        // UART Progress feedback every 128KB
                        uint32_t curr_kb = received / 1024;
                        if (curr_kb >= last_log_kb + 128 || received >= content_length) {
                            last_log_kb = curr_kb;
                            printf("   ⚡ Realtek FW 전송 진행: %lu / %lu Bytes (%lu%%)\n",
                                (unsigned long)received, (unsigned long)content_length,
                                (unsigned long)((uint64_t)received * 100 / content_length));
                        }
                    }
                } else {
                    uint8_t status = w5500_get_socket_status(0);
                    if (status == SOCK_CLOSED) break;
                    if (status == SOCK_CLOSE_WAIT) {
                        sleep_ms(10);
                        if (w5500_rx_bytes_available(0) == 0) break;
                    }
                    if (time_us_64() - start_t > 30000000ULL) break;
                    sleep_us(50);
                }
            }

            if (page_buf_idx > 0) {
                memset(page_write_buf + page_buf_idx, 0xFF, FLASH_PAGE_SIZE - page_buf_idx);
                if (flash_write_offset + FLASH_PAGE_SIZE <= FLASH_RTD_BIN_OFFSET + FLASH_RTD_MAX_SIZE) {
                    ensure_rtd_sector_erased(flash_write_offset);
                    safe_flash_program(flash_write_offset, page_write_buf, FLASH_PAGE_SIZE);
                }
            }

            // Drain any remaining trailing bytes in W5500 RX hardware FIFO
            uint64_t drain_start = time_us_64();
            while (time_us_64() - drain_start < 300000ULL) {
                uint16_t avail = w5500_rx_bytes_available(0);
                if (avail == 0) break;
                w5500_read_rx_data(0, rx_chunk, sizeof(rx_chunk));
                sleep_us(100);
            }

            if (received >= content_length) {
                upload_success = true;
            }
        }

        g_rtd_busy = false;

        if (upload_success) {
            g_fw_info.has_firmware = true;
            strncpy(g_fw_info.filename, orig_filename, sizeof(g_fw_info.filename) - 1);
            g_fw_info.size_bytes = received;
            extract_big_bin_checksum(orig_filename, total_sum, g_fw_info.big_bin_checksum, sizeof(g_fw_info.big_bin_checksum));

            uint16_t h = (total_sum >> 16) & 0xFFFF;
            uint16_t l = total_sum & 0xFFFF;
            snprintf(g_fw_info.sum32_checksum, sizeof(g_fw_info.sum32_checksum), "%04X %04X (0x%08X)", h, l, (unsigned int)total_sum);

            // Save metadata permanently to RP2350 Onboard Flash Memory
            save_firmware_info_to_flash();

            char resp_msg[512];
            snprintf(resp_msg, sizeof(resp_msg),
                "{\"success\":true,\"filename\":\"%s\",\"size\":%lu,\"size_kb\":\"%.1f\",\"big_bin\":\"%s\",\"sum32\":\"%s\"}",
                orig_filename, (unsigned long)received, (float)received/1024.0f, g_fw_info.big_bin_checksum, g_fw_info.sum32_checksum);

            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp_msg));

            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
        } else {
            char resp_msg[256];
            snprintf(resp_msg, sizeof(resp_msg),
                "{\"success\":false,\"error\":\"Upload failed or timeout\",\"received\":%lu,\"expected\":%lu}",
                (unsigned long)received, (unsigned long)content_length);

            char header[256];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(resp_msg));

            w5500_send_tx_data(0, (const uint8_t*)header, header_len);
            w5500_send_tx_data(0, (const uint8_t*)resp_msg, strlen(resp_msg));
            w5500_disconnect_socket(0);
        }
    } else {
        static char page_buf[61440];
        memset(page_buf, 0, sizeof(page_buf));
        uint32_t uptime_sec = (uint32_t)(time_us_64() / 1000000ULL);
        build_html_page(page_buf, sizeof(page_buf), cpu_temp, *led_state, uptime_sec);

        size_t page_len = strlen(page_buf);
        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            (int)page_len);

        w5500_send_tx_data(0, (const uint8_t*)header, header_len);
        w5500_send_tx_data(0, (const uint8_t*)page_buf, page_len);
        w5500_disconnect_socket(0);
    }
}
