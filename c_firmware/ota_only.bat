@echo off
chcp 65001 >nul
setlocal
rem ota_only.bat - Flash existing binary via Ethernet OTA without rebuilding or incrementing version

set /p FW_NUM=<version.txt
set FW_VER=V0.%FW_NUM%
if %FW_NUM% LSS 100 set FW_VER=V0.0%FW_NUM%
if %FW_NUM% LSS 10 set FW_VER=V0.00%FW_NUM%
for /f "usebackq tokens=3 delims= " %%v in (`findstr "FIRMWARE_VERSION" version.h`) do set FW_VER=%%~v

findstr /C:"TARGET_ETH_CHIP CHIP_W6300" config.h >nul
if %errorlevel% equ 0 (
    set BIN_NAME=w6300_pico2_firmware.bin
    set ETH_NAME=W6300
) else (
    set BIN_NAME=w5500_pico2_firmware.bin
    set ETH_NAME=W5500
)

set TARGET_IP=192.168.2.106
if not "%~1"=="" set TARGET_IP=%~1
curl.exe -s --connect-timeout 1 http://%TARGET_IP%/api/status >nul 2>&1
if %errorlevel% neq 0 (
    set TARGET_IP=192.168.10.177
)

echo ============================================================
echo Flashing Pico 2 (%FW_VER%) via %ETH_NAME% Ethernet [%TARGET_IP%] (OTA Only)
echo Target Binary: build\%BIN_NAME%
echo ============================================================

if not exist "build\%BIN_NAME%" (
    echo [ERROR] Firmware binary not found! Please run compile.bat first.
    exit /b 1
)

curl.exe -X POST --data-binary "@build\%BIN_NAME%" "http://%TARGET_IP%/upload_pico_fw?name=%BIN_NAME%"
echo.

echo ============================================================
echo [SUCCESS] Pico 2 Firmware (%FW_VER%) Ethernet OTA Sent!
echo [INFO] Web Dashboard: http://%TARGET_IP%
echo ============================================================
