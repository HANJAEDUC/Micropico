#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void http_server_init(void);
void http_server_process_request(uint8_t *request_buf, uint16_t req_len, float cpu_temp, bool *led_state);

#endif // HTTP_SERVER_H
