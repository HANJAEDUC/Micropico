@echo off
setlocal
set PICO_SDK_PATH=C:\Users\JHAN\.pico-sdk\sdk\2.3.0
set PICO_TOOLCHAIN_PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1
set PATH=C:\Users\JHAN\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\JHAN\.pico-sdk\picotool\2.3.0\picotool;C:\Users\JHAN\.pico-sdk\cmake\v4.3.4\bin;C:\Users\JHAN\.pico-sdk\ninja\v1.13.2;%PATH%

echo ============================================================
echo [1/2] Auto-Incrementing Firmware Version...
echo ============================================================
python inc_version.py

for /f "usebackq tokens=3 delims= " %%v in (`findstr "FIRMWARE_VERSION" version.h`) do set FW_VER=%%~v

echo ============================================================
echo [2/2] Compiling Pico 2 Firmware (%FW_VER%) with Ninja...
echo ============================================================
ninja -C build
if errorlevel 1 (
    echo [ERROR] Build Failed! Check C code syntax.
    exit /b 1
)

copy /y build\w5500_pico2_firmware.uf2 ..\w5500_pico2_firmware.uf2 >nul
copy /y build\w6300_pico2_firmware.uf2 ..\w6300_pico2_firmware.uf2 >nul

echo ============================================================
echo [SUCCESS] Compilation Complete! (Target: %FW_VER%)
echo [W5500]  UF2: build\w5500_pico2_firmware.uf2
echo          BIN: build\w5500_pico2_firmware.bin
echo [W6300]  UF2: build\w6300_pico2_firmware.uf2
echo          BIN: build\w6300_pico2_firmware.bin
echo Tip: Both UF2 files are also updated in the project root!
echo ============================================================

