@echo off
setlocal
rem Path to Realtek firmware binary (adjust as needed)
set RTD_FW=%~dp0realtek_firmware.bin
rem Path to Realtek ISP tool (adjust if located elsewhere)
set RTD_TOOL=%~dp0rtd_isp.exe

if not exist "%RTD_FW%" (
    echo [ERROR] Realtek firmware binary not found: %RTD_FW%
    exit /b 1
)
if not exist "%RTD_TOOL%" (
    echo [ERROR] Realtek ISP tool not found: %RTD_TOOL%
    exit /b 1
)

rem Optional: detect scaler via I2C before flashing
echo Detecting Realtek scaler (I2C address 0x4A)...
%RTD_TOOL% --detect
if %errorlevel% neq 0 (
    echo [ERROR] Realtek scaler not detected. Check wiring/power.
    exit /b 1
)

rem Flash the firmware
echo Flashing Realtek firmware %RTD_FW%...
%RTD_TOOL% --flash "%RTD_FW%"
if %errorlevel% neq 0 (
    echo [ERROR] Realtek firmware flashing failed.
    exit /b 1
)

echo [SUCCESS] Realtek firmware flashing completed.
exit /b 0
