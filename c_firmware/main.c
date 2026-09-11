#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/structs/psm.h"
#include "hardware/regs/psm.h"
#include "hardware/regs/watchdog.h"
#include "hardware/regs/m33.h"
#include "config.h"
#include "w5500_driver.h"
#include "http_server.h"
#include "snmp_agent.h"
#include "dhcp_client.h"
#include "rtd_isp.h"
#include "rs232_handler.h"

// ----------------------------------------------------
// Pico 2 이더넷 OTA 부팅 시 자동 복구 및 갱신 부트로더
// ----------------------------------------------------
typedef struct {
    uint32_t magic;          // FLASH_OTA_MAGIC (0x4F544131 = "OTA1")
    uint32_t fw_size;        // Size of new firmware binary in bytes
    uint32_t checksum_sum32; // 32-bit checksum
    char filename[128];      // Name of uploaded file
} pico_ota_header_t;

// 🛡️ 고유 하드웨어 칩셋 서명 (W5500 vs W6300 OTA 교차 업로드 원천 차단)
const char g_firmware_target_chip_sig[] __attribute__((used)) = CHIP_SIGNATURE_TAG;

// ARM Cortex-M33 Vector Table RAM 재배치 테이블 (RP2350 68개 벡터, 512B 정렬)
static uint32_t ram_vector_table[68] __attribute__((aligned(512)));

static void relocate_vtor_to_ram(void) {
    const uint32_t *vtor = (const uint32_t*)scb_hw->vtor;
    for (int i = 0; i < 68; i++) {
        ram_vector_table[i] = vtor[i];
    }
    scb_hw->vtor = (uint32_t)ram_vector_table;
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
}

// 256KB SRAM 전용 펌웨어 스테이징 사전 로딩 버퍼
static uint8_t ram_fw_staging[256 * 1024] __attribute__((aligned(4)));
// 256B 정렬 4KB SRAM RAM 전용 하드웨어 안전 섹터 버퍼
static uint8_t sector_ram_buf[4096] __attribute__((aligned(256)));

// SRAM(RAM) 영역에서만 100% 실행되는 하드웨어 절대 안전 플래시 덮어쓰기 루틴 (XIP 플래시 접근 절대 금지)
static void __no_inline_not_in_flash_func(copy_ota_staging_to_active)(uint32_t fw_size, uint32_t erase_size, const uint8_t *ram_src) {
    // 1. SysTick 타이머 및 NVIC 인터럽트 완전 해제 (Flash 0 소거 중 IRQ 진입 예방)
    systick_hw->csr = 0;
    nvic_hw->icer[0] = 0xFFFFFFFF;
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
    __asm__ volatile ("cpsid i" ::: "memory");

    // 2. ROM/RAM 직접 호출 함수 포인터 (Flash 영역 Linker Stub / Veneer 생성 원천 차단)
    void (* volatile p_erase)(uint32_t, size_t) = flash_range_erase;
    void (* volatile p_prog)(uint32_t, const uint8_t *, size_t) = flash_range_program;

    // 3. Active Bank (0x10000000) 플래시 섹터 소거
    p_erase(0, erase_size);

    // 4. SRAM RAM 100% 버퍼에서 Active Bank (0MB)로 안전 플래시 복사 (XIP 플래시 접근 절대 금지)
    for (uint32_t offset = 0; offset < erase_size; offset += FLASH_SECTOR_SIZE) {
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            uint32_t src_idx = offset + i;
            if (src_idx < fw_size) {
                sector_ram_buf[i] = ram_src[src_idx];
            } else {
                sector_ram_buf[i] = 0xFF;
            }
        }
        p_prog(offset, sector_ram_buf, FLASH_SECTOR_SIZE);
    }

    // 5. OTA 메타데이터 헤더 소거
    p_erase(FLASH_OTA_META_OFFSET, FLASH_SECTOR_SIZE);

    // 6. RP2350 (Pico 2) 공식 SDK 표준 Watchdog 하드웨어 즉시 자동 재부팅
    watchdog_reboot(0, 0, 0);

    while (1) {
        __asm__ volatile ("nop");
    }
}

void apply_ota_firmware_now(void) {
    // RP2350 XIP_BASE (0x10000000) 표준 QSPI 플래시 직독
    const pico_ota_header_t *ota_hdr = (const pico_ota_header_t *)(XIP_BASE + FLASH_OTA_META_OFFSET);
    if (ota_hdr->magic == FLASH_OTA_MAGIC && ota_hdr->fw_size > 0 && ota_hdr->fw_size <= FLASH_OTA_MAX_SIZE) {
        printf("============================================================\n");
        printf("🚀 [원스텝 고속 OTA] Staging -> Active Flash 직접 덮어쓰기 시작! (%s, %lu Bytes)\n", ota_hdr->filename, (unsigned long)ota_hdr->fw_size);

        // 🛡️ VTOR 벡터 테이블을 SRAM(RAM)으로 재배치하여 플래시 소거 중 예외 충돌 원천 방지
        relocate_vtor_to_ram();

        // 🛡️ 플래시 Staging Bank(0x10080000) 페이로드를 SRAM 100% RAM 버퍼로 사전 안전 일괄 일독
        const uint8_t *src_staging = (const uint8_t *)(XIP_BASE + FLASH_OTA_STAGING_OFFSET);
        uint32_t calculated_sum = 0;
        for (uint32_t i = 0; i < ota_hdr->fw_size; i++) {
            uint8_t b = src_staging[i];
            ram_fw_staging[i] = b;
            calculated_sum += b;
        }

        if (calculated_sum != ota_hdr->checksum_sum32) {
            printf("⚠️ [이더넷 OTA 롤백 경고] Staging Bank 무결성 첵섬 불일치! (계산: 0x%08X vs 헤더: 0x%08X)\n",
                (unsigned int)calculated_sum, (unsigned int)ota_hdr->checksum_sum32);
            printf("🛡️ [Fail-Safe 자동 복구] 손상된 펌웨어 갱신을 취소하고 기존 메인 펌웨어로 안전하게 부팅합니다.\n");
            printf("============================================================\n");

            // 손상된 헤더만 소거 후 자동 클린 재부팅
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(FLASH_OTA_META_OFFSET, FLASH_SECTOR_SIZE);
            restore_interrupts(ints);
            watchdog_reboot(0, 0, 0);
            return;
        }

        printf("   ├─► Staging Bank(0x10080000) -> SRAM RAM 버퍼 -> Active Bank(0x10000000) 100%% SRAM 안전 복사...\n");

        uint32_t erase_size = (ota_hdr->fw_size + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
        
        // 100% SRAM RAM 버퍼를 통해 충돌 없이 안전하게 플래시 덮어쓰기 및 단일 자동 재부팅 실행
        copy_ota_staging_to_active(ota_hdr->fw_size, erase_size, ram_fw_staging);
    }
}

static void check_and_apply_ethernet_ota(void) {
    const pico_ota_header_t *ota_hdr = (const pico_ota_header_t *)(XIP_BASE + FLASH_OTA_META_OFFSET);
    if (ota_hdr->magic == FLASH_OTA_MAGIC && ota_hdr->fw_size > 0 && ota_hdr->fw_size <= FLASH_OTA_MAX_SIZE) {
        printf("============================================================\n");
        printf("🛡️ [Fail-Safe 부팅 복구] 미완료 이더넷 OTA 펌웨어 감지! 자동 갱신 적용...\n");
        apply_ota_firmware_now();
    }
}

static float read_cpu_temperature(void) {
    adc_select_input(4); // RP2350 internal CPU temp sensor
    uint16_t raw = adc_read();
    float voltage = raw * (3.3f / 4095.0f);
    float temperature = 27.0f - (voltage - 0.706f) / 0.001721f;
    return temperature;
}

volatile led_mode_t g_led_mode = LED_MODE_BLINK;
volatile uint32_t g_led_blink_interval_ms = 500;
volatile bool g_led_state = true;

int main() {
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    // Check if new Ethernet OTA firmware is staged in flash
    check_and_apply_ethernet_ota();

    printf("============================================================\n");
    printf("🚀 %s Engine Booting... (Sig: %s)\n", BOARD_HW_NAME, g_firmware_target_chip_sig);
    printf("============================================================\n");

    // Initialize onboard LED (GP25)
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);
    g_led_state = true;

    // Initialize RS-232 / Realtek Scaler UART 9600 bps (GP0:TX, GP1:RX)
    uart_init(UART_ID, UART_BAUD_RATE);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    printf("🔌 RS-232 UART 9600 bps Initialized (GP0:TX, GP1:RX, 8-N-1)\n");

    // Initialize Realtek RTD ISP Channel 2 (GP2:SCL, GP3:SDA)
    rtd_isp_init();

    // Initialize Internal Temp Sensor ADC
    adc_init();
    adc_set_temp_sensor_enabled(true);

#if (TARGET_ETH_CHIP == CHIP_W6300)
    // Initialize W6300 PIO QSPI / Single SPI
    w6300_init();
#else
    // Initialize W5500 Hardware SPI
    w5500_init();
#endif

    // 1. RP2350 칩 고유 ID 기반 자동 MAC 주소 생성 (00:08:DC:XX:YY:ZZ)
    uint8_t mac[6];
    get_unique_mac_address(mac);
    printf("🏷️ RP2350 칩 고유 ID 기반 MAC 주소 생성: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 2. IP 설정 (ENABLE_DHCP 1: 공유기 자동 할당 요청 / 0: 고정 IP 0.1초 즉시 가동)
    uint8_t ip[4]  = STATIC_IP_ADDR;
    uint8_t sn[4]  = SUBNET_MASK;
    uint8_t gw[4]  = GATEWAY_ADDR;

    // 🔗 물리 링크(PHY Link) 연결 확인 (케이블 자동 협상 완료까지 최대 5.0초 대기)
    printf("⏳ [이더넷 링크] 물리적 케이블 링크(PHY Link) 감지 대기 중...\n");
    uint64_t link_t = time_us_64();
    bool link_up = false;
    while (time_us_64() - link_t < 5000000ULL) {
#if (TARGET_ETH_CHIP == CHIP_W6300)
        if (w6300_is_link_up()) { link_up = true; break; }
#else
        if (w5500_is_link_up()) { link_up = true; break; }
#endif
        sleep_ms(50);
    }
    if (link_up) {
        printf("🔗 [이더넷 링크] 정상 감지 완료! (%llu ms)\n", (time_us_64() - link_t) / 1000);
    } else {
        printf("⚠️ [이더넷 링크] 링크 대기 타임아웃 (네트워크 계속 진행)\n");
    }

#if ENABLE_DHCP
    uint8_t assigned_ip[4] = {0};
    uint8_t assigned_sn[4] = {255, 255, 255, 0};
    uint8_t assigned_gw[4] = {ip[0], ip[1], ip[2], 1};
    printf("🔍 [DHCP 클라이언트] 공유기 IP 자동 할당 요청 중 (최대 5초)...\n");
    bool dhcp_ok;
#if (TARGET_ETH_CHIP == CHIP_W6300)
    dhcp_ok = w6300_dhcp_run(assigned_ip, assigned_sn, assigned_gw, mac, 5000);
#else
    dhcp_ok = w5500_dhcp_run(assigned_ip, assigned_sn, assigned_gw, mac, 5000);
#endif
    if (dhcp_ok) {
        memcpy(ip, assigned_ip, 4);
        memcpy(sn, assigned_sn, 4);
        memcpy(gw, assigned_gw, 4);
        printf("🎉 [DHCP 성공] 공유기 할당 IP(%d.%d.%d.%d) 적용 완료\n", ip[0], ip[1], ip[2], ip[3]);
    } else {
        printf("⚠️ [DHCP 타임아웃] 공유기 응답 없음 -> 고정 IP(%d.%d.%d.%d)로 자동 전환\n", ip[0], ip[1], ip[2], ip[3]);
    }
#else
    printf("⚡ [초고속 부팅] 고정 IP(%d.%d.%d.%d) 설정 완료 (지연 0초)\n", ip[0], ip[1], ip[2], ip[3]);
#endif

#if (TARGET_ETH_CHIP == CHIP_W6300)
    w6300_setup_network(ip, sn, gw, mac);
    snmp_agent_init();
    http_server_init();

    // Initialize UART0 (GP0/GP1) and UART1 (GP4/GP5) cleanly at 9600 bps on boot
    rs232_init(uart0, 9600, 0, 1);
    rs232_init(uart1, 9600, 4, 5);

    w6300_listen_server(HTTP_PORT);
    w6300_open_udp_socket(1, SNMP_PORT);
#else
    w5500_setup_network(ip, sn, gw, mac);
    snmp_agent_init();
    http_server_init();

    // Initialize UART0 (GP0/GP1) and UART1 (GP4/GP5) cleanly at 9600 bps on boot
    rs232_init(uart0, 9600, 0, 1);
    rs232_init(uart1, 9600, 4, 5);

    w5500_listen_server(HTTP_PORT);
    w5500_open_udp_socket(1, SNMP_PORT);
#endif

    printf("============================================================\n");
    printf("🌐 C Web Server: http://%d.%d.%d.%d:%d (TCP %d)\n", ip[0], ip[1], ip[2], ip[3], HTTP_PORT, HTTP_PORT);
    printf("📡 C SNMP Agent: udp://%d.%d.%d.%d:%d (UDP %d)\n", ip[0], ip[1], ip[2], ip[3], SNMP_PORT, SNMP_PORT);
    printf("============================================================\n");

    static uint8_t http_req_buf[4096];
    static uint8_t udp_req_buf[1024];
    static uint8_t udp_resp_buf[1024];

    while (1) {
        // 1. HTTP Server Processing (Socket 0)
#if (TARGET_ETH_CHIP == CHIP_W6300)
        uint8_t status = w6300_get_socket_status(0);
        if (status == SOCK_ESTABLISHED) {
            uint16_t req_len = w6300_read_rx_data(0, http_req_buf, sizeof(http_req_buf) - 1);
            if (req_len > 0) {
                http_req_buf[req_len] = '\0';
                float current_temp = read_cpu_temperature();
                http_server_process_request(http_req_buf, req_len, current_temp, (bool*)&g_led_state);
            }
        } else if (status == SOCK_CLOSED || status == SOCK_CLOSE_WAIT || status == SOCK_TIME_WAIT || status == SOCK_LAST_ACK || status == SOCK_FIN_WAIT) {
            w6300_close_socket(0);
            w6300_listen_server(HTTP_PORT);
        } else if (status != SOCK_LISTEN && status != SOCK_SYNSENT && status != SOCK_SYNRECV) {
            w6300_listen_server(HTTP_PORT);
        }

        // 2. SNMP Agent Processing (Socket 1)
        uint8_t udp_status = w6300_get_socket_status(1);
        if (udp_status == SOCK_UDP) {
            uint8_t remote_ip[4];
            uint16_t remote_port;
            uint16_t n = w6300_recv_udp_packet(1, remote_ip, &remote_port, udp_req_buf, sizeof(udp_req_buf));
            if (n > 0) {
                float current_temp = read_cpu_temperature();
                uint16_t resp_len = snmp_agent_process_packet(udp_req_buf, n, udp_resp_buf, sizeof(udp_resp_buf), current_temp, g_led_state);
                if (resp_len > 0) {
                    w6300_send_udp_packet(1, remote_ip, remote_port, udp_resp_buf, resp_len);
                }
            }
        } else {
            w6300_open_udp_socket(1, SNMP_PORT);
        }
#else
        uint8_t status = w5500_get_socket_status(0);
        if (status == SOCK_ESTABLISHED) {
            uint16_t req_len = w5500_read_rx_data(0, http_req_buf, sizeof(http_req_buf) - 1);
            if (req_len > 0) {
                http_req_buf[req_len] = '\0';
                float current_temp = read_cpu_temperature();
                http_server_process_request(http_req_buf, req_len, current_temp, (bool*)&g_led_state);
            }
        } else if (status == SOCK_CLOSED || status == SOCK_CLOSE_WAIT || status == SOCK_TIME_WAIT || status == SOCK_LAST_ACK || status == SOCK_FIN_WAIT) {
            w5500_close_socket(0);
            w5500_listen_server(HTTP_PORT);
        } else if (status != SOCK_LISTEN && status != SOCK_INIT && status != SOCK_SYNSENT && status != SOCK_SYNRECV) {
            w5500_listen_server(HTTP_PORT);
        }

        // 2. SNMP Agent Processing (Socket 1)
        uint8_t udp_status = w5500_get_socket_status(1);
        if (udp_status == SOCK_UDP) {
            uint8_t remote_ip[4];
            uint16_t remote_port;
            uint16_t n = w5500_recv_udp_packet(1, remote_ip, &remote_port, udp_req_buf, sizeof(udp_req_buf));
            if (n > 0) {
                float current_temp = read_cpu_temperature();
                uint16_t resp_len = snmp_agent_process_packet(udp_req_buf, n, udp_resp_buf, sizeof(udp_resp_buf), current_temp, g_led_state);
                if (resp_len > 0) {
                    w5500_send_udp_packet(1, remote_ip, remote_port, udp_resp_buf, resp_len);
                }
            }
        } else {
            w5500_open_udp_socket(1, SNMP_PORT);
        }
#endif

        // 3. LED Mode Control Engine
        static uint64_t last_led_step_t = 0;
        static uint8_t sos_idx = 0;
        static uint8_t beacon_count = 0;
        uint64_t now_t = time_us_64();

        if (g_led_mode == LED_MODE_MANUAL_ON) {
            g_led_state = true;
            gpio_put(PIN_LED, 1);
        } else if (g_led_mode == LED_MODE_MANUAL_OFF) {
            g_led_state = false;
            gpio_put(PIN_LED, 0);
        } else if (g_led_mode == LED_MODE_BLINK) {
            uint64_t interval_us = (uint64_t)g_led_blink_interval_ms * 1000ULL;
            if (interval_us < 50000ULL) interval_us = 50000ULL;
            if (now_t - last_led_step_t >= interval_us) {
                last_led_step_t = now_t;
                g_led_state = !g_led_state;
                gpio_put(PIN_LED, g_led_state);
            }
        } else if (g_led_mode == LED_MODE_SOS) {
            static const uint16_t sos_times[18] = {
                150, 150, 150, 150, 150, 300,
                450, 150, 450, 150, 450, 300,
                150, 150, 150, 150, 150, 1000
            };
            static const bool sos_levels[18] = {
                1, 0, 1, 0, 1, 0,
                1, 0, 1, 0, 1, 0,
                1, 0, 1, 0, 1, 0
            };
            if (now_t - last_led_step_t >= (uint64_t)sos_times[sos_idx] * 1000ULL) {
                last_led_step_t = now_t;
                sos_idx = (sos_idx + 1) % 18;
                g_led_state = sos_levels[sos_idx];
                gpio_put(PIN_LED, g_led_state);
            }
        } else if (g_led_mode == LED_MODE_BEACON) {
            if (now_t - last_led_step_t >= 80000ULL) {
                last_led_step_t = now_t;
                g_led_state = !g_led_state;
                gpio_put(PIN_LED, g_led_state);
                beacon_count++;
                if (beacon_count >= 12) {
                    beacon_count = 0;
                    g_led_mode = LED_MODE_BLINK;
                }
            }
        }

        sleep_us(50); // Ultra-low latency loop (50us sleep)
    }

    return 0;
}
