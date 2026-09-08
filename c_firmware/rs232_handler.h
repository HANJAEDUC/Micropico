// rs232_handler.h
#ifndef RS232_HANDLER_H
#define RS232_HANDLER_H

#include "hardware/uart.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define RS232_RX_BUFFER_SIZE 512

/** Initialize UART for RS-232 communication */
void rs232_init(uart_inst_t *uart, uint baud_rate, uint tx_pin, uint rx_pin);

/** Reset UART error status and drain FIFO */
void rs232_reset(uart_inst_t *uart);

/** Send a command and receive response.
 *  Returns true if a response was received before timeout.
 *  The received data is placed in out_buf (max out_buf_len).
 *  out_len receives number of bytes received.
 *  latency_ms receives round‑trip time in ms.
 */
bool rs232_send_command(uart_inst_t *uart,
                        const char *cmd,
                        const char *ending, // "NONE", "CRLF", "CR", "LF"
                        uint32_t timeout_ms,
                        uint8_t *out_buf,
                        size_t out_buf_len,
                        size_t *out_len,
                        uint32_t *latency_ms);

#endif // RS232_HANDLER_H
