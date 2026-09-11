with open('http_server.c', 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

for i, l in enumerate(lines):
    if "Realtek FW" in l and "257" in str(i):
        start_idx = i - 2
        break

# Find while loop start and end
# Line 2544 is while (received < content_length) {
new_lines = []
skip = False
for i, line in enumerate(lines):
    if i+1 == 2580:
        new_lines.append("                } else {\n")
        new_lines.append("                    uint8_t status = w5500_get_socket_status(0);\n")
        new_lines.append("                    if (status == SOCK_CLOSED) break;\n")
        new_lines.append("                    if (status == SOCK_CLOSE_WAIT) {\n")
        new_lines.append("                        sleep_ms(10);\n")
        new_lines.append("                        if (w5500_rx_bytes_available(0) == 0) break;\n")
        new_lines.append("                    }\n")
        new_lines.append("                    if (time_us_64() - start_t > 30000000ULL) break;\n")
        new_lines.append("                    sleep_us(50);\n")
        new_lines.append("                }\n")
        new_lines.append("            }\n")
        skip = True
    elif i+1 > 2580 and i+1 <= 2597:
        continue
    else:
        if i+1 == 2598:
            skip = False
        new_lines.append(line)

with open('http_server.c', 'w', encoding='utf-8') as f:
    f.writelines(new_lines)
print("REPLACED SUCCESSFULLY")
