// rs232_handler.c - Dedicated RS-232 & UART Communication Handler
#include "rs232_handler.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

void rs232_init(uart_inst_t *uart, uint baud_rate, uint tx_pin, uint rx_pin) {
    uart_init(uart, baud_rate);
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);
    gpio_pull_up(tx_pin);
    gpio_pull_up(rx_pin);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart, true);
    rs232_reset(uart);
}

void rs232_reset(uart_inst_t *uart) {
    // Clear hardware error status register (Overrun, Break, Parity, Framing)
    uart_get_hw(uart)->rsr = 0;
    // Drain FIFO completely
    while (uart_is_readable(uart)) {
        (void)uart_getc(uart);
    }
}

static bool rs232_parse_hex(const char *str, uint8_t *out_buf, size_t *out_len, size_t max_len) {
    size_t count = 0;
    const char *p = str;
    while (*p && count < max_len) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!isxdigit((unsigned char)p[0])) break;
        
        char hex_byte[3] = {0};
        hex_byte[0] = p[0];
        p++;
        if (isxdigit((unsigned char)p[0])) {
            hex_byte[1] = p[0];
            p++;
        }
        out_buf[count++] = (uint8_t)strtoul(hex_byte, NULL, 16);
    }
    *out_len = count;
    return (count > 0);
}

bool rs232_send_command(uart_inst_t *uart,
                        const char *cmd,
                        const char *ending,
                        uint32_t timeout_ms,
                        uint8_t *out_buf,
                        size_t out_buf_len,
                        size_t *out_len,
                        uint32_t *latency_ms) {
    // 1. Reset hardware error flags and flush stale RX FIFO
    rs232_reset(uart);
    sleep_ms(2);

    // 2. Transmit command
    if (cmd && cmd[0] != '\0') {
        uint8_t tx_bytes[128];
        size_t tx_len = 0;
        
        // Detect if cmd is formatted as hex bytes (e.g. "69 00 02 FF" or "0x69, 0x00")
        const char *p = cmd;
        while (*p == ' ') p++;
        bool is_hex_pattern = true;
        for (size_t i = 0; p[i] != '\0'; i++) {
            if (!isxdigit((unsigned char)p[i]) && p[i] != ' ' && p[i] != 'x' && p[i] != 'X' && p[i] != ',') {
                is_hex_pattern = false;
                break;
            }
        }

        if (is_hex_pattern && rs232_parse_hex(cmd, tx_bytes, &tx_len, sizeof(tx_bytes)) && tx_len > 0) {
            uart_write_blocking(uart, tx_bytes, tx_len);
        } else {
            size_t slen = strlen(cmd);
            uart_write_blocking(uart, (const uint8_t *)cmd, slen);
            if (ending) {
                if (strcmp(ending, "CRLF") == 0) {
                    uart_write_blocking(uart, (const uint8_t *)"\r\n", 2);
                } else if (strcmp(ending, "CR") == 0) {
                    uart_write_blocking(uart, (const uint8_t *)"\r", 1);
                } else if (strcmp(ending, "LF") == 0) {
                    uart_write_blocking(uart, (const uint8_t *)"\n", 1);
                }
            }
        }
        uart_tx_wait_blocking(uart);
    }

    // 3. Receive response with timeout and inter-byte gap detection
    uint64_t start_us = time_us_64();
    uint64_t last_byte_t = 0;
    bool got_response = false;
    size_t idx = 0;
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;

    while (time_us_64() - start_us < timeout_us) {
        if (uart_is_readable(uart)) {
            if (idx < out_buf_len) {
                out_buf[idx++] = (uint8_t)uart_getc(uart);
            }
            got_response = true;
            last_byte_t = time_us_64();
            if (idx >= out_buf_len) break;
        } else {
            // Gap timeout: 30ms of silence after receiving at least 1 byte indicates packet end
            if (got_response && (time_us_64() - last_byte_t > 60000ULL)) {
                break;
            }
            sleep_us(50);
        }
    }

    uint64_t end_us = time_us_64();
    if (latency_ms) {
        *latency_ms = (uint32_t)((end_us - start_us) / 1000ULL);
    }
    if (out_len) {
        *out_len = idx;
    }

    // 4. Clear any hardware error status flags (e.g. framing errors from invalid baud/commands)
    uart_get_hw(uart)->rsr = 0;

    return got_response;
}
