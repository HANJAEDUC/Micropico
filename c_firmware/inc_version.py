import os

version_file = "version.txt"
header_file = "version.h"

build_num = 1
if os.path.exists(version_file):
    try:
        with open(version_file, "r") as f:
            content = f.read().strip()
            if content.isdigit():
                build_num = int(content) + 1
    except Exception as e:
        build_num = 1

with open(version_file, "w") as f:
    f.write(str(build_num))

version_str = f"V0.{build_num:03d}"

with open(header_file, "w", encoding="utf-8") as f:
    f.write(f'#ifndef VERSION_H\n#define VERSION_H\n#define FIRMWARE_VERSION "{version_str}"\n#endif\n')

print("============================================================")
print(f"[AUTO-VERSION] Target Firmware Version: {version_str}")
print("============================================================")
