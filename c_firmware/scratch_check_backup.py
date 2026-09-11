with open('backup_c_firmware_V0.212_20260902_180305/http_server.c', 'r', encoding='utf-8', errors='ignore') as f:
    text = f.read()

import re
matches = [m.start() for m in re.finditer('led_ctrl', text, re.I)]
print("Matches in backup:", len(matches))
import sys
for i, m in enumerate(matches):
    sys.stdout.buffer.write(f"\n--- MATCH {i+1} ---\n".encode('utf-8'))
    sys.stdout.buffer.write(text[m-30:m+250].encode('utf-8'))
    sys.stdout.buffer.write(b'\n')
