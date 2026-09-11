#ifndef W6300_DRIVER_H
#define W6300_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// Socket Status Definitions (Common across W5500 & W6300 TOE)
#ifndef SOCK_CLOSED
#define SOCK_CLOSED       0x00
#define SOCK_INIT         0x13
#define SOCK_LISTEN       0x14
#define SOCK_SYNSENT      0x15
#define SOCK_SYNRECV      0x16
#define SOCK_ESTABLISHED  0x17
#define SOCK_FIN_WAIT     0x18
#define SOCK_CLOSING      0x1A
#define SOCK_TIME_WAIT    0x1B
#define SOCK_CLOSE_WAIT   0x1C
#define SOCK_LAST_ACK     0x1D
#define SOCK_UDP          0x22
#endif

// Initialize W6300 Hardware via PIO QSPI/SPI on RP2350
void w6300_init(void);

// Setup Network Information (IP, Subnet, Gateway, MAC)
void w6300_setup_network(const uint8_t ip[4], const uint8_t sn[4], const uint8_t gw[4], const uint8_t mac[6]);

// Check Ethernet Physical Link Status
bool w6300_is_link_up(void);

// Socket TCP Server & Client Methods
void w6300_close_socket(uint8_t sn);
void w6300_listen_server(uint16_t port);
uint8_t w6300_get_socket_status(uint8_t sn);
uint16_t w6300_rx_bytes_available(uint8_t sn);
uint16_t w6300_read_rx_data(uint8_t sn, uint8_t *buf, uint16_t max_len);
void w6300_send_tx_data(uint8_t sn, const uint8_t *data, uint16_t len);
void w6300_disconnect_socket(uint8_t sn);

// Socket UDP Methods (SNMP / DHCP)
void w6300_open_udp_socket(uint8_t sn, uint16_t port);
uint16_t w6300_recv_udp_packet(uint8_t sn, uint8_t remote_ip[4], uint16_t *remote_port, uint8_t *buf, uint16_t max_len);
void w6300_send_udp_packet(uint8_t sn, const uint8_t remote_ip[4], uint16_t remote_port, const uint8_t *data, uint16_t len);

#endif // W6300_DRIVER_H
