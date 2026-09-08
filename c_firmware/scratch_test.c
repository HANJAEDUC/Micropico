#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Mock firmware info
typedef struct {
    bool has_firmware;
    char filename[128];
    uint32_t size_bytes;
    char big_bin_checksum[64];
    char sum32_checksum[64];
} firmware_info_t;

firmware_info_t g_fw_info = { false, "", 0, "", "" };

#include "http_server.c"

int main() {
    static char buf[32768];
    build_html_page(buf, sizeof(buf), 35.5f, true, 120);
    FILE *f = fopen("C:/Users/JHAN/.gemini/antigravity-ide/brain/1e5ef667-10f4-415c-a67b-dbb0683fd89b/scratch/page.html", "w");
    if (f) {
        fputs(buf, f);
        fclose(f);
        printf("HTML output written successfully (%d bytes).\n", (int)strlen(buf));
    } else {
        printf("Failed to write HTML file.\n");
    }
    return 0;
}
