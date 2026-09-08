# Raspberry Pi Pico 2 (RP2350) MicroPython Project

이 프로젝트는 **Raspberry Pi Pico 2 (RP2350)** 보드와 PC를 연결하여 하드웨어 제어, 온보드 ADC 칩 온도 센서 측정, 그리고 PC와의 실시간 시리얼(JSON) 통신을 구동하는 스타터 킷입니다.

---

## 📁 프로젝트 구조 (Project Architecture)

- `pico_hardware.py`: Pico 2 전용 하드웨어 제어 라이브러리
  - `PicoLED`: 온보드 LED 제어 (On/Off/Toggle/Blink)
  - `PicoTempSensor`: RP2350 내부 칩 온도 측정 (섭씨/화씨)
  - `PicoSystemInfo`: CPU 클럭, 램(RAM) 잔여량, 업타임 모니터링
- `main.py`: Pico 2에서 2초 간격 텔레메트리 전송 및 시리얼 명령어(`LED_TOGGLE`, `GET_TEMP`, `GET_INFO`, `PING`) 응답 서버
- `pc_monitor.py`: PC에서 실행되어 COM9(시리얼 포트)로 접속하여 실시간 센서 데이터를 수신하고 모니터링/제어하는 Python 스크립트
- `.vscode/settings.json`: VS Code 개발 환경 자동 설정

---

## 🔌 연결 확인 및 검증 현황

- **연결 포트**: `COM9`
- **보드 정보**: `Raspberry Pi Pico2 with RP2350` (MicroPython v1.28.0)
- **온보드 LED**: Normal functioning (`Pin('LED')`)
- **내부 온도 센서**: Normal functioning (`ADC(4)`)

---

## 🚀 빠른 시작 가이드 (Quick Start Guide)

### 1. Pico 2로 코드 전송 (Upload)

`mpremote` CLI 툴을 통해 Pico 2 내부 플래시 메모리로 작성된 라이브러리와 메인 코드를 업로드합니다.

```bash
# 1) 라이브러리 및 main.py 전송
mpremote connect COM9 cp pico_hardware.py :pico_hardware.py
mpremote connect COM9 cp main.py :main.py

# 2) Pico 2 상의 파일 목록 확인
mpremote connect COM9 ls
```

### 2. Pico 2 독립 실행 (Run on Board)

Pico 2 전원이 켜질 때 `main.py`가 자동 실행되도록 하려면 `main.py`가 보드 로트에 존재하면 됩니다.

실시간 런타임 테스트:
```bash
mpremote connect COM9 run main.py
```

### 3. PC 컨트롤러 실행 (PC Side Monitor)

PC에서 Pico 2와 연동하여 실시간 데이터 및 제어 메뉴를 확인합니다:

```bash
python pc_monitor.py COM9
```

---

## 🛠️ 확장 아이디어 (Extension Guide)

1. **외부 센서 추가 (I2C/SPI)**:
   - DHT11 / DHT22 온습도 센서, MPU6050 자이로 센서, BMP280 기압 센서 연결
   - `pico_hardware.py`에 센서 클래스 추가
2. **디스플레이 연동**:
   - 0.96인치 OLED (SSD1306) 디스플레이 추가로 온도/상태 표출
3. **모터 & 액추에이터**:
   - PWM 핀을 이용한 SG90 서보모터 / 스텝모터 제어

---
