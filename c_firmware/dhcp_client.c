#include "dhcp_client.h"
#include "w5500_driver.h"
#include "config.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include <string.h>
#include <stdio.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define DHCP_MAGIC_COOKIE 0x63825363

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[308];
} dhcp_pkt_t;

void get_unique_mac_address(uint8_t mac[6]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    mac[0] = 0x00;
    mac[1] = 0x08; // WIZnet OUI
    mac[2] = 0xDC; // WIZnet OUI
    mac[3] = id.id[5];
    mac[4] = id.id[6];
    mac[5] = id.id[7];
}

static uint32_t get_dhcp_xid(const uint8_t mac[6]) {
    return 0x39A80000ULL | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
}

static void send_dhcp_discover(uint8_t sn, const uint8_t mac[6], uint32_t xid) {
    dhcp_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = 1; // BOOTREQUEST
    pkt.htype = 1; // Ethernet
    pkt.hlen = 6;
    pkt.xid = __builtin_bswap32(xid);
    pkt.flags = __builtin_bswap16(0x8000); // Broadcast flag
    memcpy(pkt.chaddr, mac, 6);
    pkt.magic_cookie = __builtin_bswap32(DHCP_MAGIC_COOKIE);

    uint8_t *opt = pkt.options;
    // Option 53: DHCP Message Type = DISCOVER
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_DISCOVER;
    // Option 61: Client Identifier (Type 1 Ethernet + 6 bytes MAC)
    *opt++ = 61; *opt++ = 7; *opt++ = 1; memcpy(opt, mac, 6); opt += 6;
    // Option 12: Host Name ("Pico2-DisplayHub")
    const char *hostname = "Pico2-DisplayHub";
    uint8_t hlen = (uint8_t)strlen(hostname);
    *opt++ = 12; *opt++ = hlen; memcpy(opt, hostname, hlen); opt += hlen;
    // Option 55: Parameter Request List (1 = Subnet, 3 = Router, 6 = DNS)
    *opt++ = 55; *opt++ = 3; *opt++ = 1; *opt++ = 3; *opt++ = 6;
    // Option 255: End
    *opt++ = 255;

    uint16_t pkt_len = sizeof(dhcp_pkt_t) - sizeof(pkt.options) + (opt - pkt.options);
    const uint8_t broadcast_ip[4] = {255, 255, 255, 255};
    w5500_send_udp_packet(sn, broadcast_ip, DHCP_SERVER_PORT, (const uint8_t*)&pkt, pkt_len);
}

static void send_dhcp_request(uint8_t sn, const uint8_t mac[6], uint32_t xid, const uint8_t requested_ip[4], const uint8_t server_ip[4]) {
    dhcp_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = 1;
    pkt.htype = 1;
    pkt.hlen = 6;
    pkt.xid = __builtin_bswap32(xid);
    pkt.flags = __builtin_bswap16(0x8000);
    memcpy(pkt.chaddr, mac, 6);
    pkt.magic_cookie = __builtin_bswap32(DHCP_MAGIC_COOKIE);

    uint8_t *opt = pkt.options;
    // Option 53: Message Type = REQUEST
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_REQUEST;
    // Option 61: Client Identifier (Type 1 Ethernet + 6 bytes MAC)
    *opt++ = 61; *opt++ = 7; *opt++ = 1; memcpy(opt, mac, 6); opt += 6;
    // Option 12: Host Name ("Pico2-DisplayHub")
    const char *hostname = "Pico2-DisplayHub";
    uint8_t hlen = (uint8_t)strlen(hostname);
    *opt++ = 12; *opt++ = hlen; memcpy(opt, hostname, hlen); opt += hlen;
    // Option 50: Requested IP Address
    *opt++ = 50; *opt++ = 4; memcpy(opt, requested_ip, 4); opt += 4;
    // Option 54: Server Identifier
    if (server_ip[0] != 0) {
        *opt++ = 54; *opt++ = 4; memcpy(opt, server_ip, 4); opt += 4;
    }
    // Option 55: Parameter Request List
    *opt++ = 55; *opt++ = 3; *opt++ = 1; *opt++ = 3; *opt++ = 6;
    // Option 255: End
    *opt++ = 255;

    uint16_t pkt_len = sizeof(dhcp_pkt_t) - sizeof(pkt.options) + (opt - pkt.options);
    const uint8_t broadcast_ip[4] = {255, 255, 255, 255};
    w5500_send_udp_packet(sn, broadcast_ip, DHCP_SERVER_PORT, (const uint8_t*)&pkt, pkt_len);
}

bool w5500_dhcp_run(uint8_t assigned_ip[4], uint8_t assigned_sn[4], uint8_t assigned_gw[4], const uint8_t mac[6], uint32_t timeout_ms) {
    uint8_t sn = 2; // Use Socket 2 for DHCP Client
    uint32_t xid = get_dhcp_xid(mac);

    // Temp 0.0.0.0 IP configuration for DHCP discovery
    const uint8_t zero_ip[4] = {0, 0, 0, 0};
    w5500_setup_network(zero_ip, zero_ip, zero_ip, mac);
    w5500_open_udp_socket(sn, DHCP_CLIENT_PORT);

    printf("📡 [DHCP 클라이언트] 공유기(DHCP 서버)에 IP 자동 할당 요청 중 (DISCOVER)...\n");
    send_dhcp_discover(sn, mac, xid);

    uint8_t offered_ip[4] = {0};
    uint8_t dhcp_server_ip[4] = {0};
    uint8_t rx_buf[576];
    uint8_t remote_ip[4];
    uint16_t remote_port;

    bool got_offer = false;
    uint64_t start_t = time_us_64();
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
    uint64_t last_discover_t = start_t;

    // Step 1: Wait for DHCP OFFER with periodic retry (every 1.0s)
    while (time_us_64() - start_t < timeout_us) {
        if (!got_offer && (time_us_64() - last_discover_t > 1000000ULL)) {
            send_dhcp_discover(sn, mac, xid);
            last_discover_t = time_us_64();
        }
        uint16_t n = w5500_recv_udp_packet(sn, remote_ip, &remote_port, rx_buf, sizeof(rx_buf));
        if (n >= 240) {
            dhcp_pkt_t *pkt = (dhcp_pkt_t*)rx_buf;
            if (pkt->op == 2 && __builtin_bswap32(pkt->xid) == xid && pkt->magic_cookie == __builtin_bswap32(DHCP_MAGIC_COOKIE)) {
                // Parse Options
                uint8_t msg_type = 0;
                uint8_t *opt = pkt->options;
                uint8_t *opt_end = rx_buf + n;
                while (opt < opt_end && *opt != 255) {
                    uint8_t code = *opt++;
                    if (code == 0) continue;
                    uint8_t len = *opt++;
                    if (code == 53 && len >= 1) msg_type = opt[0];
                    else if (code == 1 && len >= 4) memcpy(assigned_sn, opt, 4);
                    else if (code == 3 && len >= 4) memcpy(assigned_gw, opt, 4);
                    else if (code == 54 && len >= 4) memcpy(dhcp_server_ip, opt, 4);
                    opt += len;
                }

                if (msg_type == DHCP_OFFER) {
                    memcpy(offered_ip, pkt->yiaddr, 4);
                    got_offer = true;
                    printf("📩 [DHCP OFFER 수신] 제안된 IP: %d.%d.%d.%d\n", offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3]);
                    break;
                }
            }
        }
        sleep_ms(10);
    }

    if (!got_offer) {
        w5500_close_socket(sn);
        return false; // DHCP Offer Timeout -> Fallback to static IP
    }

    // Step 2: Send DHCP REQUEST
    send_dhcp_request(sn, mac, xid, offered_ip, dhcp_server_ip);

    // Step 3: Wait for DHCP ACK
    start_t = time_us_64();
    bool got_ack = false;
    while (time_us_64() - start_t < 1500000ULL) { // 1.5s timeout for ACK
        uint16_t n = w5500_recv_udp_packet(sn, remote_ip, &remote_port, rx_buf, sizeof(rx_buf));
        if (n >= 240) {
            dhcp_pkt_t *pkt = (dhcp_pkt_t*)rx_buf;
            if (pkt->op == 2 && __builtin_bswap32(pkt->xid) == xid && pkt->magic_cookie == __builtin_bswap32(DHCP_MAGIC_COOKIE)) {
                uint8_t msg_type = 0;
                uint8_t *opt = pkt->options;
                uint8_t *opt_end = rx_buf + n;
                while (opt < opt_end && *opt != 255) {
                    uint8_t code = *opt++;
                    if (code == 0) continue;
                    uint8_t len = *opt++;
                    if (code == 53 && len >= 1) msg_type = opt[0];
                    else if (code == 1 && len >= 4) memcpy(assigned_sn, opt, 4);
                    else if (code == 3 && len >= 4) memcpy(assigned_gw, opt, 4);
                    opt += len;
                }

                if (msg_type == DHCP_ACK) {
                    memcpy(assigned_ip, pkt->yiaddr, 4);
                    got_ack = true;
                    printf("🎉 [DHCP ACK 수신 완료] 최종 승인 IP: %d.%d.%d.%d\n", assigned_ip[0], assigned_ip[1], assigned_ip[2], assigned_ip[3]);
                    break;
                }
            }
        }
        sleep_ms(10);
    }

    w5500_close_socket(sn);
    return got_ack;
}
