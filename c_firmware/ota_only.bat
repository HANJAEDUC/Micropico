@echo off
chcp 65001 >nul
setlocal
rem ota_only.bat - Flash existing binary via Ethernet OTA without rebuilding or incrementing version

set /p FW_NUM=<version.txt
set FW_VER=V0.%FW_NUM%
if %FW_NUM% LSS 100 set FW_VER=V0.0%FW_NUM%
if %FW_NUM% LSS 10 set FW_VER=V0.00%FW_NUM%
for /f "usebackq tokens=3 delims= " %%v in (`findstr "FIRMWARE_VERSION" version.h`) do set FW_VER=%%~v

echo ============================================================
echo Flashing Pico 2 (%FW_VER%) via W5500 Ethernet [192.168.10.177] (OTA Only)
echo ============================================================

if not exist "build\w5500_pico2_firmware.bin" (
    echo [ERROR] Firmware binary not found! Please run update.bat or update_eth.bat first.
    exit /b 1
)

curl -X POST --data-binary "@build\w5500_pico2_firmware.bin" "http://192.168.10.177/upload_pico_fw?name=w5500_pico2_firmware.bin"
echo.

echo ============================================================
echo [SUCCESS] Pico 2 Firmware (%FW_VER%) Ethernet OTA Sent!
echo [INFO] Web Dashboard: http://192.168.10.177
echo ============================================================
