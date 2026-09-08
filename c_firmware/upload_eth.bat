@echo off
setlocal
set PICO_SDK_PATH=C:\Users\JHAN\.pico-sdk\sdk\2.3.0
set PICO_TOOLCHAIN_PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1
set PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\JHAN\.pico-sdk\picotool\2.3.0\picotool;C:\Users\JHAN\.pico-sdk\cmake\v4.3.4\bin;C:\Users\JHAN\.pico-sdk\ninja\v1.13.2;%PATH%

echo ============================================================
echo [0/3] Flashing Realtek Firmware via ISP...
echo ============================================================
call flash_realtek.bat || exit /b 1

echo ============================================================
echo [1/3] Auto-Incrementing Firmware Version for Ethernet OTA...
echo ============================================================
python inc_version.py

echo ============================================================
echo [2/3] Rebuilding Pico 2 Firmware (Ninja)...
echo ============================================================
ninja -C build
if %errorlevel% neq 0 (
    echo [ERROR] Build Failed! Check C code syntax.
    exit /b 1
)

echo ============================================================
echo [3/3] Flashing Pico 2 via W5500 Ethernet [IP: 192.168.10.177]
echo ============================================================
curl -X POST --data-binary "@build\w5500_pico2_firmware.bin" "http://192.168.10.177/upload_pico_fw?name=w5500_pico2_firmware.bin"
echo.

echo ============================================================
echo [SUCCESS] Ethernet Firmware Flashing Sent!
echo ============================================================
