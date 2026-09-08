#ifndef DHCP_CLIENT_H
#define DHCP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

// Generate unique MAC address based on RP2350 hardware board ID
void get_unique_mac_address(uint8_t mac[6]);

// Run W5500 DHCP Client on Socket 2 (UDP Port 68/67)
// Returns true if IP is assigned by router/DHCP server within timeout_ms
// Returns false on timeout or no DHCP server (allowing automatic fallback to static IP)
bool w5500_dhcp_run(uint8_t assigned_ip[4], uint8_t assigned_sn[4], uint8_t assigned_gw[4], const uint8_t mac[6], uint32_t timeout_ms);

#endif // DHCP_CLIENT_H
