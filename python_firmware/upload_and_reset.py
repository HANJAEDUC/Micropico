"""
Raspberry Pi Pico 2 (RP2350) 자동 파일 업로드 및 하드웨어/소프트 리셋 스크립트

사용법:
  python upload_and_reset.py [COM_PORT]
"""
import sys
import subprocess
import time

if hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

FILES_TO_UPLOAD = [
    "config.py",
    "pico_hardware.py",
    "w5500_driver.py",
    "snmp_agent.py",
    "webserver.py",
    "main.py",
]

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
    print("=" * 60)
    print(f"[UPLOAD] Pico 2 File Upload & Auto-Reset (Target: {port})")
    print("=" * 60)

    # 각 파일별 resume(Ctrl+C 인터럽트 보장) 후 루트(:)로 복사
    for f in FILES_TO_UPLOAD:
        cmd = ["mpremote", "connect", port, "resume", "cp", f, ":"]
        print(f"-> [Upload] {f} -> Pico2:{f} ... ", end="", flush=True)
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            print("OK")
        else:
            print("FAIL")
            print(f"\n[mpremote Error Output]:\n{res.stderr.strip()}")
            if "failed to access" in res.stderr.lower() or "permission" in res.stderr.lower() or "device or resource busy" in res.stderr.lower():
                print("\n" + "!" * 60)
                print("[NOTICE] VS Code MicroPico 확장이 COM 포트를 잡고 있습니다!")
                print(" 방법 A: VS Code 화면 하단 상태바의 'MicroPico: Connected'를 클릭하여 끊어주세요.")
                print(" 방법 B: Ctrl + Shift + P -> 'MicroPico: Upload project to Pico' 선택")
                print("!" * 60)
            sys.exit(1)

    print("-" * 60)
    print("[RESET] Pico 2 하드웨어 리셋 중...")
    subprocess.run(["mpremote", "connect", port, "resume", "reset"], capture_output=True, text=True)
    print("✅ [SUCCESS] Pico 2 전송 및 리셋 완료!")
    print("=" * 60)
    print("[COMPLETE] Upload & Reset Finished Successfully.")
    print("=" * 60)

if __name__ == "__main__":
    main()
