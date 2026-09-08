import time
import machine

print("=" * 60)
print("🚀 W5500-EVB-Pico2 (RP2350) 자동 서비스 부팅 준비...")
print("=" * 60)

# 안전한 시리얼/USB 대기 (2초 대기하여 mpremote / REPL 인터럽트 보장)
time.sleep(2)

try:
    led = machine.Pin("LED", machine.Pin.OUT)
except Exception:
    try:
        led = machine.Pin(25, machine.Pin.OUT)
    except Exception:
        led = None

# 부팅 알림 LED 점멸 (4회 빠른 깜빡임 후 기본 ON 유지)
if led:
    for _ in range(4):
        led.value(1)
        time.sleep_ms(100)
        led.value(0)
        time.sleep_ms(100)
    led.value(1)  # 전원 인가 시 기본 LED 켜짐


print("⚡ 전원 인가 완료! 웹 서버 & SNMP 서비스를 시작합니다.")
print("=" * 60)

# Web Server 가동 및 에러 안전망
try:
    import webserver
    webserver.run_server()
except KeyboardInterrupt:
    print("\n⚠️ IDE/시리얼(Ctrl+C) 중단 요청으로 대화형 REPL(>>>)에 성공적으로 진입했습니다.")
    print("💡 웹서버 재시작: `import webserver; webserver.run_server()` 입력")
except Exception as e:
    print(f"\n❌ [시스템 에러] webserver 실행 도중 예외가 발생했습니다: {e}")
    print("🛡️ 부트 루프(Boot Loop) 방지를 위해 REPL 대기 모드로 전환합니다.")
    if led:
        for _ in range(10):
            led.value(1)
            time.sleep_ms(50)
            led.value(0)
            time.sleep_ms(50)

