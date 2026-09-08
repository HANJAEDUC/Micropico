#ifndef SNMP_AGENT_H
#define SNMP_AGENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void snmp_agent_init(void);
uint16_t snmp_agent_process_packet(const uint8_t *request, uint16_t req_len, uint8_t *response, uint16_t max_resp_len, float cpu_temp, bool led_state);

#endif // SNMP_AGENT_H
