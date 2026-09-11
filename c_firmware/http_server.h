#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LED_MODE_BLINK = 0,    // Auto heartbeat blink
    LED_MODE_MANUAL_ON = 1, // Constant ON
    LED_MODE_MANUAL_OFF = 2,// Constant OFF
    LED_MODE_SOS = 3,       // SOS Morse code pattern
    LED_MODE_BEACON = 4     // Fast burst flash (identify beacon)
} led_mode_t;

extern volatile led_mode_t g_led_mode;
extern volatile uint32_t g_led_blink_interval_ms;
extern volatile bool g_led_state;

void http_server_init(void);
void http_server_process_request(uint8_t *request_buf, uint16_t req_len, float cpu_temp, bool *led_state);
void apply_ota_firmware_now(void);

#endif // HTTP_SERVER_H

