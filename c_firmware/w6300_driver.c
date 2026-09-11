#include "config.h"

#if (TARGET_ETH_CHIP == CHIP_W6300)

#include "w6300_driver.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdio.h>

#define W6300_SPI_READ_OP   (0x00 << 5)
#define W6300_SPI_WRITE_OP  (0x01 << 5)

#define W6300_BLOCK_COMMON       0x00
#define W6300_BLOCK_SREG(n)      (1 + 4 * (n))
#define W6300_BLOCK_TXBUF(n)     (2 + 4 * (n))
#define W6300_BLOCK_RXBUF(n)     (3 + 4 * (n))

// Common Registers in Block 0
#define W6300_REG_VER       0x0002
#define W6300_REG_SYSR      0x2000
#define W6300_REG_SYCR0     0x2004
#define W6300_REG_PHYSR     0x3000
#define W6300_REG_PHYCR0    0x301C
#define W6300_REG_PHYCR1    0x301D
#define W6300_REG_SHAR      0x4120
#define W6300_REG_GAR       0x4130
#define W6300_REG_SUBR      0x4134
#define W6300_REG_SIPR      0x4138
#define W6300_REG_CHPLCKR   0x41F4
#define W6300_REG_NETLCKR   0x41F5
#define W6300_REG_PHYLCKR   0x41F6

// Socket Registers in Block (1 + 4*n)
#define W6300_REG_Sn_MR     0x0000
#define W6300_REG_Sn_CR     0x0010
#define W6300_REG_Sn_IR     0x0020
#define W6300_REG_Sn_IRCLR  0x0028
#define W6300_REG_Sn_SR     0x0030
#define W6300_REG_Sn_PORTR  0x0114
#define W6300_REG_Sn_DHAR   0x0118
#define W6300_REG_Sn_DIPR   0x0120
#define W6300_REG_Sn_DPORTR 0x0140
#define W6300_REG_Sn_TX_BSR 0x0200
#define W6300_REG_Sn_TX_FSR 0x0204
#define W6300_REG_Sn_TX_RD  0x0208
#define W6300_REG_Sn_TX_WR  0x020C
#define W6300_REG_Sn_RX_BSR 0x0220
#define W6300_REG_Sn_RX_RSR 0x0224
#define W6300_REG_Sn_RX_RD  0x0228
#define W6300_REG_Sn_RX_WR  0x022C

// Commands
#define Sn_CR_OPEN      0x01
#define Sn_CR_LISTEN    0x02
#define Sn_CR_CONNECT   0x04
#define Sn_CR_DISCON    0x08
#define Sn_CR_CLOSE     0x10
#define Sn_CR_SEND      0x20
#define Sn_CR_RECV      0x40

// Modes
#define Sn_MR_CLOSE     0x00
#define Sn_MR_TCP       0x01
#define Sn_MR_UDP       0x02

static inline void spi_delay(void) {
    asm volatile("nop\n nop\n nop\n nop\n");
}

// Ultra-fast GPIO SPI bit-bang transfer (~20MHz SPI clock, rock-solid stability)
static inline uint8_t spi_xfer_byte(uint8_t tx_data) {
    uint8_t rx_data = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_put(PIN_W6300_IO0, (tx_data >> i) & 1);
        spi_delay();
        gpio_put(PIN_W6300_SCK, 1);
        spi_delay();
        rx_data = (rx_data << 1) | (gpio_get(PIN_W6300_IO1) ? 1 : 0);
        gpio_put(PIN_W6300_SCK, 0);
        spi_delay();
    }
    return rx_data;
}

static inline void cs_select(void) {
    gpio_put(PIN_W6300_CS, 0);
    spi_delay();
}

static inline void cs_deselect(void) {
    spi_delay();
    gpio_put(PIN_W6300_CS, 1);
    spi_delay();
}

static void w6300_write_block(uint8_t block, uint16_t addr, const uint8_t *data, uint16_t len) {
    uint8_t opcode = (block & 0x1F) | W6300_SPI_WRITE_OP;
    cs_select();
    spi_xfer_byte(opcode);
    spi_xfer_byte((uint8_t)(addr >> 8));
    spi_xfer_byte((uint8_t)(addr & 0xFF));
    spi_xfer_byte(0x00); // 1 dummy byte
    for (uint16_t i = 0; i < len; i++) {
        spi_xfer_byte(data[i]);
    }
    cs_deselect();
}

static void w6300_read_block(uint8_t block, uint16_t addr, uint8_t *buf, uint16_t len) {
    uint8_t opcode = (block & 0x1F) | W6300_SPI_READ_OP;
    cs_select();
    spi_xfer_byte(opcode);
    spi_xfer_byte((uint8_t)(addr >> 8));
    spi_xfer_byte((uint8_t)(addr & 0xFF));
    spi_xfer_byte(0x00); // 1 dummy byte
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = spi_xfer_byte(0x00);
    }
    cs_deselect();
}

static inline void write_common(uint16_t addr, const uint8_t *data, uint16_t len) {
    w6300_write_block(W6300_BLOCK_COMMON, addr, data, len);
}

static inline void read_common(uint16_t addr, uint8_t *buf, uint16_t len) {
    w6300_read_block(W6300_BLOCK_COMMON, addr, buf, len);
}

static inline void write_sn(uint8_t sn, uint16_t addr, const uint8_t *data, uint16_t len) {
    w6300_write_block(W6300_BLOCK_SREG(sn), addr, data, len);
}

static inline void read_sn(uint8_t sn, uint16_t addr, uint8_t *buf, uint16_t len) {
    w6300_read_block(W6300_BLOCK_SREG(sn), addr, buf, len);
}

static inline void write_tx_buf(uint8_t sn, uint16_t addr, const uint8_t *data, uint16_t len) {
    w6300_write_block(W6300_BLOCK_TXBUF(sn), addr, data, len);
}

static inline void read_rx_buf(uint8_t sn, uint16_t addr, uint8_t *buf, uint16_t len) {
    w6300_read_block(W6300_BLOCK_RXBUF(sn), addr, buf, len);
}

static inline uint16_t read_sn16(uint8_t sn, uint16_t addr) {
    uint16_t val1 = 0, val2 = 0;
    int retry = 20;
    do {
        uint8_t b1[2] = {0}, b2[2] = {0};
        read_sn(sn, addr, b1, 2);
        read_sn(sn, addr, b2, 2);
        val1 = ((uint16_t)b1[0] << 8) | b1[1];
        val2 = ((uint16_t)b2[0] << 8) | b2[1];
        if (--retry <= 0) break;
    } while (val1 != val2);
    return val1;
}

static inline void write_sn16(uint8_t sn, uint16_t addr, uint16_t val) {
    uint8_t b[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    write_sn(sn, addr, b, 2);
}

static void cmd_sn(uint8_t sn, uint8_t cmd) {
    write_sn(sn, W6300_REG_Sn_CR, &cmd, 1);
    uint64_t start_t = time_us_64();
    while (time_us_64() - start_t < 100000ULL) {
        uint8_t reg_val = 0;
        read_sn(sn, W6300_REG_Sn_CR, &reg_val, 1);
        if (reg_val == 0) break;
        sleep_us(50);
    }
}

static void unlock_w6300_registers(void) {
    uint8_t chp_unlock = 0xCE;
    write_common(W6300_REG_CHPLCKR, &chp_unlock, 1);

    uint8_t net_unlock = 0x3A;
    write_common(W6300_REG_NETLCKR, &net_unlock, 1);

    uint8_t phy_unlock = 0x53;
    write_common(W6300_REG_PHYLCKR, &phy_unlock, 1);
}

static void configure_socket_memory(void) {
    // Socket 0 (HTTP): 8KB TX, 8KB RX
    uint8_t s0_tx = 8, s0_rx = 8;
    write_sn(0, W6300_REG_Sn_TX_BSR, &s0_tx, 1);
    write_sn(0, W6300_REG_Sn_RX_BSR, &s0_rx, 1);

    // Socket 1 (SNMP): 4KB TX, 4KB RX
    uint8_t s1_tx = 4, s1_rx = 4;
    write_sn(1, W6300_REG_Sn_TX_BSR, &s1_tx, 1);
    write_sn(1, W6300_REG_Sn_RX_BSR, &s1_rx, 1);

    // Sockets 2-3 (DHCP / Misc): 2KB TX, 2KB RX
    for (uint8_t sn = 2; sn <= 3; sn++) {
        uint8_t tx = 2, rx = 2;
        write_sn(sn, W6300_REG_Sn_TX_BSR, &tx, 1);
        write_sn(sn, W6300_REG_Sn_RX_BSR, &rx, 1);
    }

    // Sockets 4-7: 0KB TX, 0KB RX
    for (uint8_t sn = 4; sn < 8; sn++) {
        uint8_t tx = 0, rx = 0;
        write_sn(sn, W6300_REG_Sn_TX_BSR, &tx, 1);
        write_sn(sn, W6300_REG_Sn_RX_BSR, &rx, 1);
    }
}

void w6300_init(void) {
    // 1. Initialize Hardware Reset PIN (GP22) for W6300-EVB-Pico2
    gpio_init(PIN_W6300_RST);
    gpio_set_dir(PIN_W6300_RST, GPIO_OUT);
    gpio_put(PIN_W6300_RST, 0); // Assert Reset (Active LOW)
    sleep_ms(20);
    gpio_put(PIN_W6300_RST, 1); // Release Reset
    sleep_ms(50);

    // 2. Initialize SPI GPIO pins for W6300-EVB-Pico2
    gpio_init(PIN_W6300_CS);
    gpio_set_dir(PIN_W6300_CS, GPIO_OUT);
    cs_deselect();

    gpio_init(PIN_W6300_SCK);
    gpio_set_dir(PIN_W6300_SCK, GPIO_OUT);
    gpio_put(PIN_W6300_SCK, 0);

    gpio_init(PIN_W6300_IO0);
    gpio_set_dir(PIN_W6300_IO0, GPIO_OUT);
    gpio_put(PIN_W6300_IO0, 0);

    gpio_init(PIN_W6300_IO1);
    gpio_set_dir(PIN_W6300_IO1, GPIO_IN);
    gpio_pull_up(PIN_W6300_IO1);

    // In Single SPI mode, keep IO2 and IO3 pulled up as inputs
    gpio_init(PIN_W6300_IO2);
    gpio_set_dir(PIN_W6300_IO2, GPIO_IN);
    gpio_pull_up(PIN_W6300_IO2);

    gpio_init(PIN_W6300_IO3);
    gpio_set_dir(PIN_W6300_IO3, GPIO_IN);
    gpio_pull_up(PIN_W6300_IO3);

    // INT pin as input with pull-up
    gpio_init(PIN_W6300_INT);
    gpio_set_dir(PIN_W6300_INT, GPIO_IN);
    gpio_pull_up(PIN_W6300_INT);

    sleep_ms(50);

    // 2. Soft Reset W6300 Core
    unlock_w6300_registers();
    uint8_t rst = 0x80;
    write_common(W6300_REG_SYCR0, &rst, 1);
    sleep_ms(60);

    unlock_w6300_registers();

    // 3. Configure PHY (Auto-negotiation, Normal Operation)
    uint8_t phy_mode = 0x00; // Auto-negotiation
    write_common(W6300_REG_PHYCR0, &phy_mode, 1);

    uint8_t phy_norm = 0x40; // bit 6 = 1, normal operation, PWDN = 0
    write_common(W6300_REG_PHYCR1, &phy_norm, 1);
    sleep_ms(20);

    uint8_t ver[2] = {0, 0};
    read_common(W6300_REG_VER, ver, 2);
    printf("📡 W6300 Version Register (0x0002): 0x%02X%02X\n", ver[0], ver[1]);
    printf("✅ W6300 Hardware Chip Initialized Successfully!\n");

    configure_socket_memory();
}

void w6300_setup_network(const uint8_t ip[4], const uint8_t sn[4], const uint8_t gw[4], const uint8_t mac[6]) {
    unlock_w6300_registers();
    write_common(W6300_REG_SHAR, mac, 6);
    write_common(W6300_REG_GAR, gw, 4);
    write_common(W6300_REG_SUBR, sn, 4);
    write_common(W6300_REG_SIPR, ip, 4);
}

bool w6300_is_link_up(void) {
    uint8_t physr = 0;
    read_common(W6300_REG_PHYSR, &physr, 1);
    return (physr & 0x01) != 0; // Bit 0: PHYSR_LNK_UP
}

void w6300_close_socket(uint8_t sn) {
    cmd_sn(sn, Sn_CR_CLOSE);
    uint8_t zero = 0;
    write_sn(sn, W6300_REG_Sn_MR, &zero, 1);
}

void w6300_listen_server(uint16_t port) {
    uint8_t status = w6300_get_socket_status(0);
    if (status != SOCK_LISTEN && status != SOCK_ESTABLISHED) {
        w6300_close_socket(0);
        uint8_t mr = Sn_MR_TCP;
        write_sn(0, W6300_REG_Sn_MR, &mr, 1);
        write_sn16(0, W6300_REG_Sn_PORTR, port);

        cmd_sn(0, Sn_CR_OPEN);
        for (int i = 0; i < 100; i++) {
            if (w6300_get_socket_status(0) == SOCK_INIT) break;
            sleep_us(100);
        }

        cmd_sn(0, Sn_CR_LISTEN);
        for (int i = 0; i < 100; i++) {
            if (w6300_get_socket_status(0) == SOCK_LISTEN) break;
            sleep_us(100);
        }
    }
}

uint8_t w6300_get_socket_status(uint8_t sn) {
    uint8_t status = 0;
    read_sn(sn, W6300_REG_Sn_SR, &status, 1);
    return status;
}

uint16_t w6300_rx_bytes_available(uint8_t sn) {
    return read_sn16(sn, W6300_REG_Sn_RX_RSR);
}

static inline uint16_t get_w6300_socket_buf_size(uint8_t sn) {
    return (sn == 0) ? 8192 : ((sn == 1) ? 4096 : 2048);
}

static void w6300_read_data_buf(uint8_t sn, uint16_t ptr, uint8_t *buf, uint16_t len) {
    if (len == 0 || buf == NULL) return;
    uint16_t buf_size = get_w6300_socket_buf_size(sn);
    uint16_t buf_mask = buf_size - 1;
    uint16_t offset = ptr & buf_mask;

    if (offset + len <= buf_size) {
        read_rx_buf(sn, offset, buf, len);
    } else {
        uint16_t size1 = buf_size - offset;
        uint16_t size2 = len - size1;
        read_rx_buf(sn, offset, buf, size1);
        read_rx_buf(sn, 0, buf + size1, size2);
    }
}

static void w6300_write_data_buf(uint8_t sn, uint16_t ptr, const uint8_t *data, uint16_t len) {
    if (len == 0 || data == NULL) return;
    uint16_t buf_size = get_w6300_socket_buf_size(sn);
    uint16_t buf_mask = buf_size - 1;
    uint16_t offset = ptr & buf_mask;

    if (offset + len <= buf_size) {
        write_tx_buf(sn, offset, data, len);
    } else {
        uint16_t size1 = buf_size - offset;
        uint16_t size2 = len - size1;
        write_tx_buf(sn, offset, data, size1);
        write_tx_buf(sn, 0, data + size1, size2);
    }
}

uint16_t w6300_read_rx_data(uint8_t sn, uint8_t *buf, uint16_t max_len) {
    uint16_t rx_len = w6300_rx_bytes_available(sn);
    if (rx_len == 0) return 0;
    if (rx_len > max_len) rx_len = max_len;

    uint16_t rd_ptr = read_sn16(sn, W6300_REG_Sn_RX_RD);
    w6300_read_data_buf(sn, rd_ptr, buf, rx_len);
    write_sn16(sn, W6300_REG_Sn_RX_RD, rd_ptr + rx_len);
    cmd_sn(sn, Sn_CR_RECV);
    return rx_len;
}

static bool is_w6300_tx_buffer_empty(uint8_t sn) {
    uint16_t rd = read_sn16(sn, W6300_REG_Sn_TX_RD);
    uint16_t wr = read_sn16(sn, W6300_REG_Sn_TX_WR);
    return (rd == wr);
}

void w6300_send_tx_data(uint8_t sn, const uint8_t *data, uint16_t len) {
    if (len == 0 || data == NULL) return;

    uint16_t sent = 0;
    while (sent < len) {
        uint16_t free_size = 0;
        uint64_t start_t = time_us_64();
        while (1) {
            free_size = read_sn16(sn, W6300_REG_Sn_TX_FSR);
            if (free_size > 0) break;
            if (time_us_64() - start_t > 3000000ULL) {
                printf("⚠️ [W6300] Socket %d TX buffer timeout\n", sn);
                return;
            }
            sleep_us(100);
        }

        uint16_t remaining = len - sent;
        uint16_t chunk_size = (remaining > free_size) ? free_size : remaining;

        uint16_t wr_ptr = read_sn16(sn, W6300_REG_Sn_TX_WR);
        w6300_write_data_buf(sn, wr_ptr, data + sent, chunk_size);
        write_sn16(sn, W6300_REG_Sn_TX_WR, wr_ptr + chunk_size);
        cmd_sn(sn, Sn_CR_SEND);

        start_t = time_us_64();
        while (1) {
            uint8_t cmd_reg = 0;
            read_sn(sn, W6300_REG_Sn_CR, &cmd_reg, 1);
            if (cmd_reg == 0) break;
            if (time_us_64() - start_t > 100000ULL) break;
            sleep_us(50);
        }

        sent += chunk_size;
    }
}

void w6300_disconnect_socket(uint8_t sn) {
    // Wait up to 1000ms for TX buffer to be sent over Ethernet before DISCON
    uint64_t start_t = time_us_64();
    while (time_us_64() - start_t < 1000000ULL) {
        if (is_w6300_tx_buffer_empty(sn)) break;
        sleep_us(100);
    }
    sleep_ms(10);
    cmd_sn(sn, Sn_CR_DISCON);
}


void w6300_open_udp_socket(uint8_t sn, uint16_t port) {
    uint8_t status = w6300_get_socket_status(sn);
    if (status == SOCK_UDP) return;

    w6300_close_socket(sn);
    uint8_t mr = Sn_MR_UDP;
    write_sn(sn, W6300_REG_Sn_MR, &mr, 1);
    write_sn16(sn, W6300_REG_Sn_PORTR, port);
    cmd_sn(sn, Sn_CR_OPEN);
    for (int i = 0; i < 100; i++) {
        if (w6300_get_socket_status(sn) == SOCK_UDP) break;
        sleep_us(100);
    }
}

uint16_t w6300_recv_udp_packet(uint8_t sn, uint8_t remote_ip[4], uint16_t *remote_port, uint8_t *buf, uint16_t max_len) {
    uint16_t rx_len = w6300_rx_bytes_available(sn);
    if (rx_len < 8) return 0;

    uint16_t rd_ptr = read_sn16(sn, W6300_REG_Sn_RX_RD);
    uint8_t head[8];
    w6300_read_data_buf(sn, rd_ptr, head, 8);

    // W6300 UDP4 PACKET-INFO layout in RX buffer (8 bytes):
    // head[0..1]: [Flags 5-bit | Data Length 11-bit]
    // head[2..5]: Remote IPv4 Address (4 bytes)
    // head[6..7]: Remote Port (2 bytes)
    uint16_t pkt_len = (((uint16_t)(head[0] & 0x07)) << 8) | head[1];

    if (remote_ip != NULL) {
        remote_ip[0] = head[2];
        remote_ip[1] = head[3];
        remote_ip[2] = head[4];
        remote_ip[3] = head[5];
    }
    if (remote_port != NULL) {
        *remote_port = ((uint16_t)head[6] << 8) | head[7];
    }

    uint16_t read_payload = pkt_len;
    if (read_payload > max_len) read_payload = max_len;

    if (buf != NULL && read_payload > 0) {
        w6300_read_data_buf(sn, rd_ptr + 8, buf, read_payload);
    }

    // Advance read pointer past the 8-byte PACKET-INFO header and full packet payload
    write_sn16(sn, W6300_REG_Sn_RX_RD, rd_ptr + 8 + pkt_len);
    cmd_sn(sn, Sn_CR_RECV);
    return read_payload;
}

void w6300_send_udp_packet(uint8_t sn, const uint8_t remote_ip[4], uint16_t remote_port, const uint8_t *data, uint16_t len) {
    write_sn(sn, W6300_REG_Sn_DIPR, remote_ip, 4);
    write_sn16(sn, W6300_REG_Sn_DPORTR, remote_port);

    // If destination is broadcast (255.255.255.255), set broadcast MAC address explicitly
    if (remote_ip[0] == 255 && remote_ip[1] == 255 && remote_ip[2] == 255 && remote_ip[3] == 255) {
        const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        write_sn(sn, W6300_REG_Sn_DHAR, bcast_mac, 6);
    }

    uint16_t wr_ptr = read_sn16(sn, W6300_REG_Sn_TX_WR);
    w6300_write_data_buf(sn, wr_ptr, data, len);
    write_sn16(sn, W6300_REG_Sn_TX_WR, wr_ptr + len);
    cmd_sn(sn, Sn_CR_SEND);
}

#endif // (TARGET_ETH_CHIP == CHIP_W6300)
