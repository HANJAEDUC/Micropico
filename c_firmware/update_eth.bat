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

findstr /C:"TARGET_ETH_CHIP CHIP_W6300" config.h >nul
if %errorlevel% equ 0 (
    set BIN_NAME=w6300_pico2_firmware.bin
    set ETH_NAME=W6300
) else (
    set BIN_NAME=w5500_pico2_firmware.bin
    set ETH_NAME=W5500
)

echo ============================================================
echo [2/2] Flashing Pico 2 (%FW_VER%) via %ETH_NAME% Ethernet [192.168.10.177]
echo Target Binary: build\%BIN_NAME%
echo ============================================================
curl.exe -X POST --data-binary "@build\%BIN_NAME%" "http://192.168.10.177/upload_pico_fw?name=%BIN_NAME%"
echo.

echo ============================================================
echo [SUCCESS] Pico 2 Firmware (%FW_VER%) Ethernet OTA Sent!
echo [INFO] Web Dashboard: http://192.168.10.177
echo ============================================================
