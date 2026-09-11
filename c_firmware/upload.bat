@echo off
set PICO_SDK_PATH=C:\Users\JHAN\.pico-sdk\sdk\2.3.0
set PICO_TOOLCHAIN_PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1
set PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\JHAN\.pico-sdk\picotool\2.3.0\picotool;C:\Users\JHAN\.pico-sdk\cmake\v4.3.4\bin;C:\Users\JHAN\.pico-sdk\ninja\v1.13.2;%PATH%

echo ============================================================
echo [0/2] Auto-Incrementing Firmware Version...
echo ============================================================
python inc_version.py

for /f "usebackq tokens=3 delims= " %%v in (`findstr "FIRMWARE_VERSION" version.h`) do set FW_VER=%%~v

echo.
echo ============================================================
echo [1/2] Rebuilding Pico 2 Firmware (%FW_VER%) with Ninja...
echo ============================================================
ninja -C build

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build Failed! Check C code syntax.
    exit /b %ERRORLEVEL%
)

findstr /C:"TARGET_ETH_CHIP CHIP_W6300" config.h >nul
if %errorlevel% equ 0 (
    set TARGET_UF2=build\w6300_pico2_firmware.uf2
    set ETH_NAME=W6300
) else (
    set TARGET_UF2=build\w5500_pico2_firmware.uf2
    set ETH_NAME=W5500
)

echo.
echo ============================================================
echo [2/2] Auto-flashing %ETH_NAME% (%FW_VER%) via Picotool / Drive Copy...
echo Target File: %TARGET_UF2%
echo ============================================================
picotool load -fx %TARGET_UF2% 2>nul

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Flashing Complete via Picotool! Pico 2 Rebooted.
    exit /b 0
)

echo [NOTICE] Picotool direct flash bypassed. Searching for BOOTSEL drive (RPI-RP2 / RP2350)...
set PICO_DRIVE=
for /f "usebackq tokens=*" %%d in (`powershell -NoProfile -Command "Get-Volume | Where-Object { $_.FileSystemLabel -match 'RPI-RP2|RP2350' } | Select-Object -ExpandProperty DriveLetter"`) do set PICO_DRIVE=%%d:

if defined PICO_DRIVE (
    echo Found BOOTSEL Drive at %PICO_DRIVE%\
    echo Copying %TARGET_UF2% to %PICO_DRIVE%\ ...
    copy /Y %TARGET_UF2% %PICO_DRIVE%\
    if %ERRORLEVEL% EQU 0 (
        echo.
        echo [SUCCESS] Firmware copied successfully! Pico 2 is rebooting.
    ) else (
        echo.
        echo [ERROR] Failed to copy UF2 file to %PICO_DRIVE%\.
    )
) else (
    echo.
    echo [WARNING] Could not detect RPI-RP2 / RP2350 USB Drive.
    echo Please hold BOOTSEL button while reconnecting USB, then run upload.bat again,
    echo or manually copy %TARGET_UF2% to the RPI-RP2 drive.
)
