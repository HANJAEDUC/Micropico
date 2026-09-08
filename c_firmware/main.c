#include <stdio.h>
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

// ARM Cortex-M33 Vector Table RAM 재배치 테이블 (128B 정렬)
static uint32_t ram_vector_table[48] __attribute__((aligned(128)));

static void relocate_vtor_to_ram(void) {
    const uint32_t *vtor = (const uint32_t*)scb_hw->vtor;
    for (int i = 0; i < 48; i++) {
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

    save_and_disable_interrupts();

    // 2. Active Bank (0x10000000) 플래시 섹터 소거
    flash_range_erase(0, erase_size);

    // 3. SRAM RAM 100% 버퍼에서 Active Bank (0MB)로 안전 플래시 복사 (XIP 플래시 접근 절대 금지)
    for (uint32_t offset = 0; offset < erase_size; offset += FLASH_SECTOR_SIZE) {
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            uint32_t src_idx = offset + i;
            if (src_idx < fw_size) {
                sector_ram_buf[i] = ram_src[src_idx];
            } else {
                sector_ram_buf[i] = 0xFF;
            }
        }
        flash_range_program(offset, sector_ram_buf, FLASH_SECTOR_SIZE);
    }

    // 4. OTA 메타데이터 헤더 소거
    flash_range_erase(FLASH_OTA_META_OFFSET, FLASH_SECTOR_SIZE);

    // 5. RP2350 Watchdog Scratch 레지스터 완전 초기화 (BOOTSEL 잔재 제거)
    for (int i = 0; i < 8; i++) {
        watchdog_hw->scratch[i] = 0;
    }

    // 6. RP2350 ARM Cortex-M33 SCB AIRCR System Reset (100% 인라인 하드웨어 콜드 리셋)
    scb_hw->aircr = (0x05FA << 16) | (1 << 2);
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");

    while (1) { __asm__ volatile ("nop"); }
}

static void check_and_apply_ethernet_ota(void) {
    // RP2350 XIP_BASE (0x10000000) 표준 QSPI 플래시 직독
    const pico_ota_header_t *ota_hdr = (const pico_ota_header_t *)(XIP_BASE + FLASH_OTA_META_OFFSET);
    if (ota_hdr->magic == FLASH_OTA_MAGIC && ota_hdr->fw_size > 0 && ota_hdr->fw_size <= FLASH_OTA_MAX_SIZE) {
        printf("============================================================\n");
        printf("🚀 [이더넷 OTA 부트로더] 새 Pico 2 펌웨어 요청 감지! (파일: %s, %lu Bytes)\n", ota_hdr->filename, (unsigned long)ota_hdr->fw_size);

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

            // 손상된 헤더만 소거 후 기존 펌웨어로 부팅
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(FLASH_OTA_META_OFFSET, FLASH_SECTOR_SIZE);
            restore_interrupts(ints);
            return;
        }

        printf("   ├─► Staging Bank(0x10080000) -> SRAM RAM 버퍼(86KB) -> Active Bank(0x10000000) 100%% SRAM 안전 복사...\n");

        uint32_t erase_size = (ota_hdr->fw_size + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
        
        // 100% SRAM RAM 버퍼를 통해 충돌 없이 안전하게 플래시 덮어쓰기 및 자동 재부팅 실행
        copy_ota_staging_to_active(ota_hdr->fw_size, erase_size, ram_fw_staging);
    }
}

static float read_cpu_temperature(void) {
    adc_select_input(4); // RP2350 internal CPU temp sensor
    uint16_t raw = adc_read();
    float voltage = raw * (3.3f / 4095.0f);
    float temperature = 27.0f - (voltage - 0.706f) / 0.001721f;
    return temperature;
}

int main() {
    stdio_init_all();
    sleep_ms(200);

    // 🛡️ VTOR 벡터 테이블을 부팅 즉시 SRAM(RAM)으로 재배치하여 런타임 플래시 소거/기록 중 예외 충돌 원천 방지
    relocate_vtor_to_ram();

    // 부팅 즉시 이더넷 OTA 펌웨어 업데이트 대기 요청 확인
    check_and_apply_ethernet_ota();

    printf("============================================================\n");
    printf("🚀 RP2350A + W5500 High-Speed C Engine Booting...\n");
    printf("============================================================\n");

    // Initialize all possible onboard LED pins (GP25, GP22, GP2, GP15) for guaranteed visual feedback
    static const uint32_t led_pins[] = {25, 22, 2, 15};
    for (int i = 0; i < 4; i++) {
        gpio_init(led_pins[i]);
        gpio_set_dir(led_pins[i], GPIO_OUT);
        gpio_put(led_pins[i], 1);
    }
    bool led_state = true;

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

    // Initialize W5500 Hardware SPI
    w5500_init();

    // 1. RP2350 칩 고유 ID 기반 자동 MAC 주소 생성 (00:08:DC:XX:YY:ZZ)
    uint8_t mac[6];
    get_unique_mac_address(mac);
    printf("🏷️ RP2350 칩 고유 ID 기반 MAC 주소 생성: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 2. 이더넷 PHY 물리 링크 상태 확인 (최대 5초 대기)
    printf("🔌 이더넷 PHY 물리 링크 확인 중...\n");
    for (int i = 0; i < 50; i++) {
        if (w5500_is_link_up()) {
            printf("✅ 이더넷 링크 연결 확인 완료! (Link UP)\n");
            break;
        }
        sleep_ms(100);
    }

    // 3. W5500 DHCP 클라이언트 실행 (3.5초 타임아웃, 공유기 미연결 시 고정 IP 192.168.10.177 자동 폴백)
    uint8_t ip[4]  = STATIC_IP_ADDR;
    uint8_t sn[4]  = SUBNET_MASK;
    uint8_t gw[4]  = GATEWAY_ADDR;

    if (w5500_dhcp_run(ip, sn, gw, mac, 3500)) {
        printf("🎉 DHCP IP 자동 할당 성공! IP: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
    } else {
        printf("⚠️ DHCP 서버 응답 없음 -> 기본 고정 IP(%d.%d.%d.%d)로 안전 폴백 사용\n",
            ip[0], ip[1], ip[2], ip[3]);
    }

    w5500_setup_network(ip, sn, gw, mac);
    snmp_agent_init();
    http_server_init();

    // Initialize UART0 (GP0/GP1) and UART1 (GP4/GP5) cleanly at 9600 bps on boot
    rs232_init(uart0, 9600, 0, 1);
    rs232_init(uart1, 9600, 4, 5);

    w5500_listen_server(HTTP_PORT);
    w5500_open_udp_socket(1, SNMP_PORT);

    printf("============================================================\n");
    printf("🌐 C Web Server: http://%d.%d.%d.%d:%d (TCP %d)\n", ip[0], ip[1], ip[2], ip[3], HTTP_PORT, HTTP_PORT);
    printf("📡 C SNMP Agent: udp://%d.%d.%d.%d:%d (UDP %d)\n", ip[0], ip[1], ip[2], ip[3], SNMP_PORT, SNMP_PORT);
    printf("============================================================\n");

    static uint8_t http_req_buf[4096];
    static uint8_t udp_req_buf[1024];
    static uint8_t udp_resp_buf[1024];

    while (1) {
        // 1. HTTP Server Processing (Socket 0)
        uint8_t status = w5500_get_socket_status(0);
        if (status == SOCK_ESTABLISHED) {
            uint16_t req_len = w5500_read_rx_data(0, http_req_buf, sizeof(http_req_buf) - 1);
            if (req_len > 0) {
                http_req_buf[req_len] = '\0';
                float current_temp = read_cpu_temperature();
                http_server_process_request(http_req_buf, req_len, current_temp, &led_state);
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
                uint16_t resp_len = snmp_agent_process_packet(udp_req_buf, n, udp_resp_buf, sizeof(udp_resp_buf), current_temp, led_state);
                if (resp_len > 0) {
                    w5500_send_udp_packet(1, remote_ip, remote_port, udp_resp_buf, resp_len);
                }
            }
        } else {
            w5500_open_udp_socket(1, SNMP_PORT);
        }

        // 3. 0.5초 간격 LED 하드웨어 초고속 점멸 (500ms 토글 - 이더넷 OTA 최종 더블체크용)
        static uint64_t last_led_blink_t = 0;
        uint64_t now_t = time_us_64();
        if (now_t - last_led_blink_t >= 500000ULL) {
            last_led_blink_t = now_t;
            led_state = !led_state;
            for (int i = 0; i < 4; i++) {
                gpio_put(led_pins[i], led_state);
            }
        }

        sleep_us(50); // Ultra-low latency loop (50us sleep)
    }

    return 0;
}
