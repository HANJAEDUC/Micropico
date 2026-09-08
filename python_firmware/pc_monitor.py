import serial
import serial.tools.list_ports
import json
import threading
import time
import sys

def find_pico_port():
    ports = serial.tools.list_ports.comports()
    for port in ports:
        # Pico 2 usually shows up as USB Serial Device
        if "Pico" in port.description or "Serial" in port.description or "COM9" in port.device:
            return port.device
    if ports:
        return ports[0].device
    return "COM9"

class PicoMonitor:
    def __init__(self, port=None, baudrate=115200):
        self.port = port or find_pico_port()
        self.baudrate = baudrate
        self.ser = None
        self.running = False
        self.thread = None

    def connect(self):
        print(f"[PC Monitor] Connecting to Pico 2 on port {self.port} at {self.baudrate} baud...")
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)
            # Send CTRL-C + CTRL-D to reset MicroPython into main.py
            self.ser.write(b"\x03\x04")
            time.sleep(0.5)
            self.running = True
            self.thread = threading.Thread(target=self._listen_loop, daemon=True)
            self.thread.start()
            print(f"[PC Monitor] Connected successfully! (Soft-rebooted Pico 2 into main.py)")
            return True
        except Exception as e:
            print(f"[PC Monitor] Connection failed: {e}")
            return False

    def send_command(self, cmd_str):
        if self.ser and self.ser.is_open:
            full_cmd = cmd_str.strip() + "\r\n"
            self.ser.write(full_cmd.encode('utf-8'))
            print(f" -> Sent to Pico 2: '{cmd_str}'")

    def _listen_loop(self):
        while self.running and self.ser and self.ser.is_open:
            try:
                line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    if line.startswith("{") and line.endswith("}"):
                        try:
                            data = json.loads(line)
                            data_type = data.get("type", "unknown")
                            if data_type == "telemetry":
                                print(f"\r[Pico Telemetry #{data.get('id')}] Temp: {data.get('temp_c')}°C ({data.get('temp_f')}°F) | LED: {'ON' if data.get('led') else 'OFF'} | Free RAM: {data.get('free_mem')} bytes | Uptime: {data.get('uptime')}s")
                            elif data_type == "response":
                                print(f"\r[Pico Response] {json.dumps(data, indent=2)}")
                            else:
                                print(f"\r[Pico Raw Data] {data}")
                        except json.JSONDecodeError:
                            print(f"\r[Pico Raw] {line}")
                    else:
                        print(f"\r[Pico Output] {line}")
            except Exception as e:
                if self.running:
                    print(f"\n[PC Monitor Error] {e}")
                break

    def disconnect(self):
        self.running = False
        if self.ser and self.ser.is_open:
            self.ser.close()
        print("\n[PC Monitor] Disconnected.")

def main():
    print("=" * 60)
    print("      Raspberry Pi Pico 2 (RP2350) PC Controller & Telemetry      ")
    print("=" * 60)

    port = find_pico_port()
    if len(sys.argv) > 1:
        port = sys.argv[1]

    monitor = PicoMonitor(port=port)
    if not monitor.connect():
        sys.exit(1)

    time.sleep(1)

    print("\n--- Available Commands ---")
    print(" [1] Toggle Onboard LED")
    print(" [2] Request Temperature")
    print(" [3] Request Pico 2 System Info")
    print(" [4] Send Ping")
    print(" [q] Quit\n")

    try:
        while True:
            choice = input("\nEnter choice (1-4, q): ").strip().lower()
            if choice == '1':
                monitor.send_command("LED_TOGGLE")
            elif choice == '2':
                monitor.send_command("GET_TEMP")
            elif choice == '3':
                monitor.send_command("GET_INFO")
            elif choice == '4':
                monitor.send_command("PING")
            elif choice == 'q':
                break
            else:
                print("Unknown choice. Please press 1, 2, 3, 4, or q.")
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        monitor.disconnect()

if __name__ == "__main__":
    main()
