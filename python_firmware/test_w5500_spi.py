import machine
import time

# W5500-EVB-Pico2 온보드 SPI0 및 핀 정의
MISO_PIN = 16
CS_PIN = 17
SCK_PIN = 18
MOSI_PIN = 19
RST_PIN = 20

# 1. Hardware Reset
rst = machine.Pin(RST_PIN, machine.Pin.OUT)
rst.value(0)
time.sleep_ms(50)
rst.value(1)
time.sleep_ms(100)

# 2. SPI0 및 CS 핀 초기화
cs = machine.Pin(CS_PIN, machine.Pin.OUT)
cs.value(1)

spi = machine.SPI(0, baudrate=8_000_000, polarity=0, phase=0,
                  sck=machine.Pin(SCK_PIN),
                  mosi=machine.Pin(MOSI_PIN),
                  miso=machine.Pin(MISO_PIN))

# 3. W5500 VERSIONR 레지스터 (Address: 0x0039, Control: 0x00) 읽기
# SPI 프레임: [Addr_H(0x00), Addr_L(0x39), Control(0x00), Dummy(0x00)]
def read_w5500_version():
    cs.value(0)
    cmd = bytes([0x00, 0x39, 0x00]) # 0x0039, Read Common Register
    spi.write(cmd)
    val = spi.read(1)
    cs.value(1)
    return val[0]

try:
    version = read_w5500_version()
    print("=" * 50)
    print(f"📡 W5500-EVB-Pico2 SPI 통신 검증 결과")
    print(f"-> Read Version Register (0x0039): 0x{version:02X}")
    if version == 0x04:
        print("✅ 성공: W5500 하드웨어 칩이 정상적으로 응답했습니다! (Chip ID: 0x04)")
    else:
        print(f"⚠️ 경고: 예상을 벗어난 응답 값입니다 (expected 0x04, got 0x{version:02X})")
    print("=" * 50)
except Exception as e:
    print("❌ 오류 발생:", e)
