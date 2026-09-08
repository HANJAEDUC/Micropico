@echo off
chcp 65001 >nul
setlocal
set PICO_SDK_PATH=C:\Users\JHAN\.pico-sdk\sdk\2.3.0
set PICO_TOOLCHAIN_PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1
set PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\JHAN\.pico-sdk\picotool\2.3.0\picotool;C:\Users\JHAN\.pico-sdk\cmake\v4.3.4\bin;C:\Users\JHAN\.pico-sdk\ninja\v1.13.2;%PATH%

echo ============================================================
echo [0/2] Auto-Incrementing Firmware Version for Ethernet OTA...
echo ============================================================
python inc_version.py

set /p FW_NUM=<version.txt
set FW_VER=V0.%FW_NUM%
if %FW_NUM% LSS 100 set FW_VER=V0.0%FW_NUM%
if %FW_NUM% LSS 10 set FW_VER=V0.00%FW_NUM%
for /f "usebackq tokens=3 delims= " %%v in (`findstr "FIRMWARE_VERSION" version.h`) do set FW_VER=%%~v

echo ============================================================
echo [1/2] Rebuilding Pico 2 Firmware (%FW_VER%) with Ninja...
echo ============================================================
ninja -C build
if errorlevel 1 (
    echo [ERROR] Build Failed! Check C code syntax.
    exit /b 1
)

echo ============================================================
echo [2/2] Flashing Pico 2 (%FW_VER%) via W5500 Ethernet [192.168.10.177]
echo ============================================================
curl -X POST --data-binary "@build\w5500_pico2_firmware.bin" "http://192.168.10.177/upload_pico_fw?name=w5500_pico2_firmware.bin"
echo.

echo ============================================================
echo [SUCCESS] Pico 2 Firmware (%FW_VER%) Ethernet OTA Sent!
echo [INFO] Web Dashboard: http://192.168.10.177
echo ============================================================
