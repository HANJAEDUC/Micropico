import machine
import time
import gc
import os

class PicoLED:
    """Raspberry Pi Pico 2 Onboard LED Controller"""
    def __init__(self, pin_identifier="LED"):
        try:
            self.pin = machine.Pin(pin_identifier, machine.Pin.OUT)
            self.pwm = None
        except Exception as e:
            print("LED init warning:", e)
            self.pin = machine.Pin(25, machine.Pin.OUT)
        self.on()


    def on(self):
        self.pin.value(1)

    def off(self):
        self.pin.value(0)

    def toggle(self):
        self.pin.value(not self.pin.value())

    def state(self):
        return self.pin.value()

    def blink(self, count=3, delay_ms=100):
        for _ in range(count):
            self.on()
            time.sleep_ms(delay_ms)
            self.off()
            time.sleep_ms(delay_ms)


class PicoTempSensor:
    """RP2350 Internal Temperature Sensor ADC Reader"""
    def __init__(self, adc_channel=4):
        self.adc = machine.ADC(adc_channel)

    def read_voltage(self, samples=5):
        raw_sum = 0
        for _ in range(samples):
            raw_sum += self.adc.read_u16()
            time.sleep_us(100)
        avg_raw = raw_sum / samples
        voltage = avg_raw * 3.3 / 65535.0
        return voltage

    def read_celsius(self, samples=5):
        voltage = self.read_voltage(samples)
        # Standard RP2 temp conversion formula
        temp_c = 27.0 - (voltage - 0.706) / 0.001721
        return round(temp_c, 2)

    def read_fahrenheit(self, samples=5):
        celsius = self.read_celsius(samples)
        return round(celsius * 9 / 5 + 32, 2)


class PicoSystemInfo:
    """System Utilities & Telemetry for Pico 2 (RP2350)"""
    @staticmethod
    def get_info():
        gc.collect()
        uname = os.uname()
        return {
            "board": "Raspberry Pi Pico 2 (RP2350)",
            "sysname": uname.sysname,
            "release": uname.release,
            "version": uname.version,
            "cpu_freq_mhz": machine.freq() // 1_000_000,
            "mem_free_bytes": gc.mem_free(),
            "mem_alloc_bytes": gc.mem_alloc(),
            "uptime_sec": time.ticks_ms() // 1000
        }
