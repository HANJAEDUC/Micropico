#include "config.h"

#if (TARGET_ETH_CHIP == CHIP_W5500)

#include "w5500_driver.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

#define COMMON_REG_R 0x00
#define COMMON_REG_W 0x04

#define CR_OPEN      0x01
#define CR_LISTEN    0x02
#define CR_DISCON    0x08
#define CR_CLOSE     0x10
#define CR_SEND      0x20
#define CR_RECV      0x40
#define CR_SEND_MAC  0x21

static inline uint8_t get_sn_reg_r(uint8_t sn) { return ((sn * 4 + 1) << 3) | 0x00; }
static inline uint8_t get_sn_reg_w(uint8_t sn) { return ((sn * 4 + 1) << 3) | 0x04; }
static inline uint8_t get_sn_tx_w(uint8_t sn)  { return ((sn * 4 + 2) << 3) | 0x04; }
static inline uint8_t get_sn_rx_r(uint8_t sn)  { return ((sn * 4 + 3) << 3) | 0x00; }

static inline void cs_select(void) {
    while (spi_is_readable(SPI_PORT)) {
        uint8_t dummy;
        spi_read_blocking(SPI_PORT, 0, &dummy, 1);
    }
    gpio_put(PIN_CS, 0);
}

static inline void cs_deselect(void) {
    gpio_put(PIN_CS, 1);
}

static void write_common(uint16_t addr, const uint8_t *data, uint16_t len) {
    cs_select();
    uint8_t hdr[3] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), COMMON_REG_W };
    spi_write_blocking(SPI_PORT, hdr, 3);
    spi_write_blocking(SPI_PORT, data, len);
    cs_deselect();
}

static void read_common(uint16_t addr, uint8_t *buf, uint16_t len) {
    cs_select();
    uint8_t hdr[3] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), COMMON_REG_R };
    spi_write_blocking(SPI_PORT, hdr, 3);
    spi_read_blocking(SPI_PORT, 0x00, buf, len);
    cs_deselect();
}

static void write_sn(uint8_t sn, uint16_t addr, const uint8_t *data, uint16_t len) {
    uint8_t reg_w = get_sn_reg_w(sn);
    cs_select();
    uint8_t hdr[3] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), reg_w };
    spi_write_blocking(SPI_PORT, hdr, 3);
    spi_write_blocking(SPI_PORT, data, len);
    cs_deselect();
}

static void read_sn(uint8_t sn, uint16_t addr, uint8_t *buf, uint16_t len) {
    uint8_t reg_r = get_sn_reg_r(sn);
    cs_select();
    uint8_t hdr[3] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), reg_r };
    spi_write_blocking(SPI_PORT, hdr, 3);
    spi_read_blocking(SPI_PORT, 0x00, buf, len);
    cs_deselect();
}

static void cmd_sn(uint8_t sn, uint8_t cmd) {
    write_sn(sn, 0x0001, &cmd, 1);
    uint64_t start_t = time_us_64();
    while (time_us_64() - start_t < 100000ULL) {
        uint8_t reg_val = 0;
        read_sn(sn, 0x0001, &reg_val, 1);
        if (reg_val == 0) break;
        sleep_us(50);
    }
}

static void configure_socket_memory(void) {
    // Socket 0 (HTTP): 8KB TX, 8KB RX
    uint8_t s0_tx = 8, s0_rx = 8;
    write_sn(0, 0x001E, &s0_tx, 1);
    write_sn(0, 0x001F, &s0_rx, 1);

    // Socket 1 (SNMP): 4KB TX, 4KB RX
    uint8_t s1_tx = 4, s1_rx = 4;
    write_sn(1, 0x001E, &s1_tx, 1);
    write_sn(1, 0x001F, &s1_rx, 1);

    // Sockets 2-3: 2KB TX, 2KB RX (Socket 2: 2KB, Socket 3: 2KB -> Total sum 8+4+2+2 = 16KB)
    for (uint8_t sn = 2; sn <= 3; sn++) {
        uint8_t tx = 2, rx = 2;
        write_sn(sn, 0x001E, &tx, 1);
        write_sn(sn, 0x001F, &rx, 1);
    }

    // Sockets 4-7: 0KB TX, 0KB RX
    for (uint8_t sn = 4; sn < 8; sn++) {
        uint8_t tx = 0, rx = 0;
        write_sn(sn, 0x001E, &tx, 1);
        write_sn(sn, 0x001F, &rx, 1);
    }
}

void w5500_init(void) {
    spi_init(SPI_PORT, 25000000); // 25MHz High-Speed SPI clock
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    cs_deselect();

    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 0);
    sleep_ms(50);
    gpio_put(PIN_RST, 1);
    sleep_ms(100);

    // Issue W5500 Mode Register Software Reset (0x80) to clean internal registers
    uint8_t mr_rst = 0x80;
    write_common(0x0000, &mr_rst, 1);
    sleep_ms(50);

    uint8_t ver = 0;
    read_common(0x0039, &ver, 1);
    printf("📡 W5500 Version Register (0x0039): 0x%02X\n", ver);
    if (ver == 0x04) {
        printf("✅ W5500 Hardware Chip Detected Successfully! (ID: 0x04)\n");
    } else {
        printf("⚠️ W5500 Chip Read Warning: Expected 0x04, Got 0x%02X\n", ver);
    }

    configure_socket_memory();
}

void w5500_setup_network(const uint8_t ip[4], const uint8_t sn[4], const uint8_t gw[4], const uint8_t mac[6]) {
    write_common(0x0001, gw, 4);
    write_common(0x0005, sn, 4);
    write_common(0x0009, mac, 6);
    write_common(0x000F, ip, 4);

    // RTR: 1.0s (0x2710)
    uint8_t rtr[2] = { 0x27, 0x10 };
    write_common(0x0019, rtr, 2);
    // RTY: 15
    uint8_t rty = 0x0F;
    write_common(0x001B, &rty, 1);
    sleep_ms(10);
}

bool w5500_is_link_up(void) {
    uint8_t phy = 0;
    read_common(0x002E, &phy, 1);
    return (phy & 0x01) != 0;
}

void w5500_close_socket(uint8_t sn) {
    cmd_sn(sn, CR_CLOSE);
    sleep_us(100);
}

void w5500_listen_server(uint16_t port) {
    uint8_t status = w5500_get_socket_status(0);
    if (status != SOCK_LISTEN && status != SOCK_ESTABLISHED) {
        w5500_close_socket(0);
        uint8_t mode = 0x01; // TCP
        write_sn(0, 0x0000, &mode, 1);
        uint8_t port_buf[2] = { (uint8_t)(port >> 8), (uint8_t)(port & 0xFF) };
        write_sn(0, 0x0004, port_buf, 2);

        cmd_sn(0, CR_OPEN);
        for (int i = 0; i < 100; i++) {
            if (w5500_get_socket_status(0) == SOCK_INIT) break;
            sleep_us(100);
        }

        cmd_sn(0, CR_LISTEN);
        for (int i = 0; i < 100; i++) {
            if (w5500_get_socket_status(0) == SOCK_LISTEN) break;
            sleep_us(100);
        }
    }
}

uint8_t w5500_get_socket_status(uint8_t sn) {
    uint8_t status = SOCK_CLOSED;
    read_sn(sn, 0x0003, &status, 1);
    return status;
}

uint16_t w5500_rx_bytes_available(uint8_t sn) {
    uint16_t val1 = 0, val2 = 0;
    int retry = 50;
    do {
        uint8_t buf1[2] = {0}, buf2[2] = {0};
        read_sn(sn, 0x0026, buf1, 2);
        read_sn(sn, 0x0026, buf2, 2);
        val1 = (uint16_t)((buf1[0] << 8) | buf1[1]);
        val2 = (uint16_t)((buf2[0] << 8) | buf2[1]);
        if (--retry <= 0) break;
    } while (val1 != val2);
    return val1;
}

uint16_t w5500_read_rx_data(uint8_t sn, uint8_t *buf, uint16_t max_len) {
    uint16_t avail = w5500_rx_bytes_available(sn);
    if (avail == 0) return 0;

    uint16_t read_len = (avail > max_len) ? max_len : avail;
    uint8_t ptr_buf[2] = {0};
    read_sn(sn, 0x0028, ptr_buf, 2);
    uint16_t rd_ptr = (uint16_t)((ptr_buf[0] << 8) | ptr_buf[1]);

    // Socket buffer size mask (Socket 0: 8KB = 0x1FFF, Socket 1: 4KB = 0x0FFF, etc.)
    uint16_t buf_size = (sn == 0) ? 8192 : ((sn == 1) ? 4096 : 2048);
    uint16_t buf_mask = buf_size - 1;
    uint16_t offset = rd_ptr & buf_mask;
    uint8_t rx_r_cb = get_sn_rx_r(sn);

    if (offset + read_len <= buf_size) {
        // Continuous block read
        cs_select();
        uint8_t hdr[3] = { (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF), rx_r_cb };
        spi_write_blocking(SPI_PORT, hdr, 3);
        spi_read_blocking(SPI_PORT, 0x00, buf, read_len);
        cs_deselect();
    } else {
        // Wrap-around split read across 8KB circular ring boundary
        uint16_t size1 = buf_size - offset;
        uint16_t size2 = read_len - size1;

        cs_select();
        uint8_t hdr1[3] = { (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF), rx_r_cb };
        spi_write_blocking(SPI_PORT, hdr1, 3);
        spi_read_blocking(SPI_PORT, 0x00, buf, size1);
        cs_deselect();

        cs_select();
        uint8_t hdr2[3] = { 0x00, 0x00, rx_r_cb };
        spi_write_blocking(SPI_PORT, hdr2, 3);
        spi_read_blocking(SPI_PORT, 0x00, buf + size1, size2);
        cs_deselect();
    }

    uint16_t new_rd = rd_ptr + read_len;
    uint8_t new_rd_buf[2] = { (uint8_t)(new_rd >> 8), (uint8_t)(new_rd & 0xFF) };
    write_sn(sn, 0x0028, new_rd_buf, 2);
    cmd_sn(sn, CR_RECV);

    return read_len;
}

static uint16_t get_tx_free_size(uint8_t sn) {
    uint16_t val1 = 0, val2 = 0;
    int retry = 50;
    do {
        uint8_t buf1[2] = {0}, buf2[2] = {0};
        read_sn(sn, 0x0020, buf1, 2);
        read_sn(sn, 0x0020, buf2, 2);
        val1 = (uint16_t)((buf1[0] << 8) | buf1[1]);
        val2 = (uint16_t)((buf2[0] << 8) | buf2[1]);
        if (--retry <= 0) break;
    } while (val1 != val2);
    return val1;
}

static bool is_tx_buffer_empty(uint8_t sn) {
    uint8_t rd_buf1[2] = {0}, rd_buf2[2] = {0};
    uint8_t wr_buf1[2] = {0}, wr_buf2[2] = {0};
    int retry = 50;

    do {
        read_sn(sn, 0x0022, rd_buf1, 2);
        read_sn(sn, 0x0022, rd_buf2, 2);
        if (--retry <= 0) break;
    } while (rd_buf1[0] != rd_buf2[0] || rd_buf1[1] != rd_buf2[1]);

    retry = 50;
    do {
        read_sn(sn, 0x0024, wr_buf1, 2);
        read_sn(sn, 0x0024, wr_buf2, 2);
        if (--retry <= 0) break;
    } while (wr_buf1[0] != wr_buf2[0] || wr_buf1[1] != wr_buf2[1]);

    uint16_t rd = (uint16_t)((rd_buf1[0] << 8) | rd_buf1[1]);
    uint16_t wr = (uint16_t)((wr_buf1[0] << 8) | wr_buf1[1]);

    return (rd == wr);
}

void w5500_send_tx_data(uint8_t sn, const uint8_t *data, uint16_t len) {
    if (len == 0 || data == NULL) return;

    uint8_t tx_w_cb = get_sn_tx_w(sn);
    uint16_t sent = 0;
    uint16_t buf_size = (sn == 0) ? 8192 : ((sn == 1) ? 4096 : 2048);
    uint16_t buf_mask = buf_size - 1;

    while (sent < len) {
        uint16_t free_size = 0;
        uint64_t start_t = time_us_64();
        while (1) {
            free_size = get_tx_free_size(sn);
            if (free_size > 0) break;
            if (time_us_64() - start_t > 3000000ULL) {
                printf("⚠️ [W5500] Socket %d TX buffer timeout\n", sn);
                return;
            }
            sleep_us(100);
        }

        uint16_t remaining = len - sent;
        uint16_t chunk_size = (remaining > free_size) ? free_size : remaining;

        uint8_t ptr_buf[2] = {0};
        read_sn(sn, 0x0024, ptr_buf, 2); // Read current Sn_TX_WR
        uint16_t wr_ptr = (uint16_t)((ptr_buf[0] << 8) | ptr_buf[1]);
        uint16_t offset = wr_ptr & buf_mask;

        if (offset + chunk_size <= buf_size) {
            cs_select();
            uint8_t hdr[3] = { (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF), tx_w_cb };
            spi_write_blocking(SPI_PORT, hdr, 3);
            spi_write_blocking(SPI_PORT, data + sent, chunk_size);
            cs_deselect();
        } else {
            uint16_t size1 = buf_size - offset;
            uint16_t size2 = chunk_size - size1;

            cs_select();
            uint8_t hdr1[3] = { (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF), tx_w_cb };
            spi_write_blocking(SPI_PORT, hdr1, 3);
            spi_write_blocking(SPI_PORT, data + sent, size1);
            cs_deselect();

            cs_select();
            uint8_t hdr2[3] = { 0x00, 0x00, tx_w_cb };
            spi_write_blocking(SPI_PORT, hdr2, 3);
            spi_write_blocking(SPI_PORT, data + sent + size1, size2);
            cs_deselect();
        }

        uint16_t new_wr = wr_ptr + chunk_size;
        uint8_t new_wr_buf[2] = { (uint8_t)(new_wr >> 8), (uint8_t)(new_wr & 0xFF) };
        write_sn(sn, 0x0024, new_wr_buf, 2);

        cmd_sn(sn, CR_SEND);

        start_t = time_us_64();
        while (1) {
            uint8_t cmd_reg = 0;
            read_sn(sn, 0x0001, &cmd_reg, 1);
            if (cmd_reg == 0) break;
            if (time_us_64() - start_t > 100000ULL) break;
            sleep_us(50);
        }

        sent += chunk_size;
    }
}

void w5500_disconnect_socket(uint8_t sn) {
    // 1. Check Sn_TX_RD == Sn_TX_WR to verify all TX bytes sent over network (up to 1000ms wait)
    uint64_t start_t = time_us_64();
    while (time_us_64() - start_t < 1000000ULL) {
        if (is_tx_buffer_empty(sn)) break;
        sleep_us(100);
    }
    sleep_ms(10);

    // 2. Issue graceful DISCON (TCP FIN) so browser cleanly receives 200 OK
    cmd_sn(sn, CR_DISCON);
}

void w5500_open_udp_socket(uint8_t sn, uint16_t port) {
    uint8_t status = w5500_get_socket_status(sn);
    if (status != SOCK_UDP) {
        w5500_close_socket(sn);
        uint8_t mode = 0x02; // UDP
        write_sn(sn, 0x0000, &mode, 1);
        uint8_t port_buf[2] = { (uint8_t)(port >> 8), (uint8_t)(port & 0xFF) };
        write_sn(sn, 0x0004, port_buf, 2);

        cmd_sn(sn, CR_OPEN);
        uint64_t start_t = time_us_64();
        while (time_us_64() - start_t < 100000ULL) {
            if (w5500_get_socket_status(sn) == SOCK_UDP) break;
            sleep_us(50);
        }
    }
}

uint16_t w5500_recv_udp_packet(uint8_t sn, uint8_t remote_ip[4], uint16_t *remote_port, uint8_t *buf, uint16_t max_len) {
    uint16_t avail = w5500_rx_bytes_available(sn);
    if (avail < 8) return 0; // UDP Header = IP(4) + Port(2) + Length(2)

    uint8_t header[8] = {0};
    w5500_read_rx_data(sn, header, 8);

    memcpy(remote_ip, header, 4);
    *remote_port = (uint16_t)((header[4] << 8) | header[5]);
    uint16_t payload_len = (uint16_t)((header[6] << 8) | header[7]);

    uint16_t read_len = (payload_len > max_len) ? max_len : payload_len;
    if (read_len > 0) {
        w5500_read_rx_data(sn, buf, read_len);
    }
    return read_len;
}

void w5500_send_udp_packet(uint8_t sn, const uint8_t remote_ip[4], uint16_t remote_port, const uint8_t *data, uint16_t len) {
    if (remote_ip[0] == 255 && remote_ip[1] == 255 && remote_ip[2] == 255 && remote_ip[3] == 255) {
        static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        write_sn(sn, 0x0006, bcast_mac, 6);
    }
    write_sn(sn, 0x000C, remote_ip, 4);
    uint8_t port_buf[2] = { (uint8_t)(remote_port >> 8), (uint8_t)(remote_port & 0xFF) };
    write_sn(sn, 0x0010, port_buf, 2);
    w5500_send_tx_data(sn, data, len);
}

#endif // (TARGET_ETH_CHIP == CHIP_W5500)
