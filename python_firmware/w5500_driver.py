import machine
import time

class W5500:
    """
    W5500-EVB-Pico2 전용 순수 MicroPython 드라이버 (TCP + UDP 멀티 소켓 지원)
    Socket 0: TCP Web Server (Port 80)
    Socket 1: UDP SNMP Agent (Port 161)
    """
    # Block Select Control Bytes for S0 and S1
    COMMON_REG_R = 0x00
    COMMON_REG_W = 0x04
    
    # Socket 0 Control Bytes (BSB 1)
    S0_REG_R     = 0x08
    S0_REG_W     = 0x0C
    S0_TX_W      = 0x14
    S0_RX_R      = 0x18

    # Socket 1 Control Bytes (BSB 2)
    S1_REG_R     = 0x28
    S1_REG_W     = 0x2C
    S1_TX_W      = 0x34
    S1_RX_R      = 0x38

    # Commands (Sn_CR)
    CR_OPEN      = 0x01
    CR_LISTEN    = 0x02
    CR_CONNECT   = 0x04
    CR_DISCON    = 0x08
    CR_CLOSE     = 0x10
    CR_SEND      = 0x20
    CR_RECV      = 0x40

    # Status (Sn_SR)
    SOCK_CLOSED      = 0x00
    SOCK_INIT        = 0x13
    SOCK_LISTEN      = 0x14
    SOCK_ESTABLISHED = 0x17
    SOCK_CLOSE_WAIT  = 0x1C
    SOCK_UDP         = 0x22

    def __init__(self, spi, cs_pin, rst_pin=20):
        self.spi = spi
        self.cs = machine.Pin(cs_pin, machine.Pin.OUT)
        self.cs.value(1)
        
        if rst_pin is not None:
            rst = machine.Pin(rst_pin, machine.Pin.OUT)
            rst.value(0)
            time.sleep_ms(50)
            rst.value(1)
            time.sleep_ms(100)

    def _write_common(self, addr, data):
        self.cs.value(0)
        buf = bytearray([(addr >> 8) & 0xFF, addr & 0xFF, self.COMMON_REG_W])
        buf.extend(data)
        self.spi.write(buf)
        self.cs.value(1)

    def _read_common(self, addr, length):
        self.cs.value(0)
        buf = bytearray([(addr >> 8) & 0xFF, addr & 0xFF, self.COMMON_REG_R])
        self.spi.write(buf)
        res = self.spi.read(length)
        self.cs.value(1)
        return res

    def _write_sn(self, sn, addr, data):
        reg_w = self.S0_REG_W if sn == 0 else self.S1_REG_W
        self.cs.value(0)
        buf = bytearray([(addr >> 8) & 0xFF, addr & 0xFF, reg_w])
        buf.extend(data)
        self.spi.write(buf)
        self.cs.value(1)

    def _read_sn(self, sn, addr, length):
        reg_r = self.S0_REG_R if sn == 0 else self.S1_REG_R
        self.cs.value(0)
        buf = bytearray([(addr >> 8) & 0xFF, addr & 0xFF, reg_r])
        self.spi.write(buf)
        res = self.spi.read(length)
        self.cs.value(1)
        return res

    def _cmd_sn(self, sn, cmd):
        self._write_sn(sn, 0x0001, bytes([cmd]))
        for _ in range(100):
            res = self._read_sn(sn, 0x0001, 1)
            if res and len(res) > 0 and res[0] == 0:
                return
            time.sleep_us(10)

    def setup_network(self, ip, subnet="255.255.255.0", gateway="192.168.10.1", mac="00:08:DC:23:50:0A"):
        """네트워크 고정 IP 및 MAC 주소 설정"""
        try:
            mac_bytes = bytes([int(x, 16) for x in mac.split(':')])
            ip_bytes = bytes([int(x) for x in ip.split('.')])
            sn_bytes = bytes([int(x) for x in subnet.split('.')])
            gw_bytes = bytes([int(x) for x in gateway.split('.')])

            self._write_common(0x0001, gw_bytes)   # Gateway
            self._write_common(0x0005, sn_bytes)   # Subnet
            self._write_common(0x0009, mac_bytes)  # MAC
            self._write_common(0x000F, ip_bytes)   # Source IP

            # RTR (Retransmission Retry Time): 10,000 * 100us = 1.0초 (0x2710) - Flash 기록 중 TCP 윈도우 대기 보장
            self._write_common(0x0019, bytes([0x27, 0x10]))
            # RTY (Retry Count): 15회 (0x0F)
            self._write_common(0x001B, bytes([0x0F]))
            
            time.sleep_ms(10)
        except Exception as e:
            print("⚠️ W5500 네트워크 설정 예외:", e)

    def get_ip(self):
        try:
            res = self._read_common(0x000F, 4)
            if res and len(res) == 4:
                return ".".join(str(b) for b in res)
        except Exception:
            pass
        return "0.0.0.0"

    def is_link_up(self):
        """W5500 PHY 레지스터(0x002E) Bit 0 (LNK) 확인하여 케이블 물리적 연결 여부 반환"""
        try:
            res = self._read_common(0x002E, 1)
            if res and len(res) > 0:
                return bool(res[0] & 0x01)
        except Exception:
            pass
        return False


    # ----------------------------------------------------
    # Socket 0: TCP Server Methods (Web Server)
    # ----------------------------------------------------
    def close_socket(self, sn=0):
        """소켓 강제 닫기 (CR_CLOSE)"""
        try:
            self._cmd_sn(sn, self.CR_CLOSE)
            time.sleep_ms(5)
        except Exception:
            pass

    def listen_server(self, port=80):
        try:
            res = self._read_sn(0, 0x0003, 1)
            status = res[0] if res and len(res) > 0 else self.SOCK_CLOSED
            # 이미 LISTEN 또는 ESTABLISHED 인 경우는 유지하고, 그 외에는 닫고 재오픈
            if status != self.SOCK_LISTEN and status != self.SOCK_ESTABLISHED:
                self.close_socket(0)
                self._write_sn(0, 0x001E, bytes([0x0C])) # Socket 0 RX Buffer: 12KB (대용량 TCP 수신 버퍼)
                self._write_sn(0, 0x001F, bytes([0x02])) # Socket 0 TX Buffer: 2KB
                self._write_sn(0, 0x0000, bytes([0x01])) # TCP mode
                self._write_sn(0, 0x0004, bytes([(port >> 8) & 0xFF, port & 0xFF]))
                self._cmd_sn(0, self.CR_OPEN)
                time.sleep_ms(5)
                self._cmd_sn(0, self.CR_LISTEN)
                time.sleep_ms(5)
        except Exception as e:
            print("⚠️ listen_server 오류:", e)


    def get_socket_status(self, sn=0):
        try:
            res = self._read_sn(sn, 0x0003, 1)
            if res and len(res) > 0:
                return res[0]
        except Exception:
            pass
        return self.SOCK_CLOSED

    def rx_bytes_available(self, sn=0):
        try:
            res = self._read_sn(sn, 0x0026, 2)
            if res and len(res) >= 2:
                return (res[0] << 8) | res[1]
        except Exception:
            pass
        return 0

    def read_rx_data(self, sn=0):
        rx_size = self.rx_bytes_available(sn)
        if rx_size == 0:
            return b""
        
        try:
            rd_ptr_buf = self._read_sn(sn, 0x0028, 2)
            if not rd_ptr_buf or len(rd_ptr_buf) < 2:
                return b""
            rd_ptr = (rd_ptr_buf[0] << 8) | rd_ptr_buf[1]
            
            rx_r_cb = self.S0_RX_R if sn == 0 else self.S1_RX_R
            self.cs.value(0)
            buf = bytearray([(rd_ptr >> 8) & 0xFF, rd_ptr & 0xFF, rx_r_cb])
            self.spi.write(buf)
            data = self.spi.read(rx_size)
            self.cs.value(1)
            
            new_rd = (rd_ptr + rx_size) & 0xFFFF
            self._write_sn(sn, 0x0028, bytes([(new_rd >> 8) & 0xFF, new_rd & 0xFF]))
            self._cmd_sn(sn, self.CR_RECV)
            return data
        except Exception as e:
            self.cs.value(1)
            print("⚠️ read_rx_data 오류:", e)
            return b""

    def get_tx_free_size(self, sn=0):
        try:
            res = self._read_sn(sn, 0x0020, 2)
            if res and len(res) >= 2:
                return (res[0] << 8) | res[1]
        except Exception:
            pass
        return 0

    def send_tx_data(self, data, sn=0):
        data_len = len(data)
        if data_len == 0:
            return
            
        try:
            offset = 0
            chunk_size = 1024
            
            while offset < data_len:
                chunk = data[offset : offset + chunk_size]
                chunk_len = len(chunk)
                
                # TX Free Size 가 chunk_len 이상이 될 때까지 대기 (최대 100ms)
                for _ in range(50):
                    if self.get_tx_free_size(sn) >= chunk_len:
                        break
                    time.sleep_ms(2)
                
                wr_ptr_buf = self._read_sn(sn, 0x0024, 2)
                if not wr_ptr_buf or len(wr_ptr_buf) < 2:
                    return
                wr_ptr = (wr_ptr_buf[0] << 8) | wr_ptr_buf[1]
                
                tx_w_cb = self.S0_TX_W if sn == 0 else self.S1_TX_W
                self.cs.value(0)
                buf = bytearray([(wr_ptr >> 8) & 0xFF, wr_ptr & 0xFF, tx_w_cb])
                buf.extend(chunk)
                self.spi.write(buf)
                self.cs.value(1)
                
                new_wr = (wr_ptr + chunk_len) & 0xFFFF
                self._write_sn(sn, 0x0024, bytes([(new_wr >> 8) & 0xFF, new_wr & 0xFF]))
                self._cmd_sn(sn, self.CR_SEND)
                time.sleep_ms(5)
                
                offset += chunk_len
        except Exception as e:
            self.cs.value(1)
            print("⚠️ send_tx_data 오류:", e)

    def disconnect_socket(self, sn=0):
        try:
            self._cmd_sn(sn, self.CR_DISCON)
            time.sleep_ms(5)
            self._cmd_sn(sn, self.CR_CLOSE)
        except Exception:
            pass

    # ----------------------------------------------------
    # Socket 1: UDP Listener Methods (SNMP Agent - Port 161)
    # ----------------------------------------------------
    def open_udp_socket(self, sn=1, port=161):
        try:
            res = self._read_sn(sn, 0x0003, 1)
            status = res[0] if res and len(res) > 0 else self.SOCK_CLOSED
            if status != self.SOCK_UDP:
                self._cmd_sn(sn, self.CR_CLOSE)
                time.sleep_ms(2)
                self._write_sn(sn, 0x0000, bytes([0x02])) # UDP mode (0x02)
                self._write_sn(sn, 0x0004, bytes([(port >> 8) & 0xFF, port & 0xFF]))
                self._cmd_sn(sn, self.CR_OPEN)
                time.sleep_ms(5)
        except Exception as e:
            print("⚠️ open_udp_socket 오류:", e)

    def recv_udp_packet(self, sn=1):
        """W5500 UDP 헤더 [Remote IP 4B, Remote Port 2B, Data Len 2B] + Data 읽기"""
        rx_size = self.rx_bytes_available(sn)
        if rx_size < 8:
            return None, None, b""
            
        raw_data = self.read_rx_data(sn)
        if len(raw_data) < 8:
            return None, None, b""
            
        remote_ip = ".".join(str(b) for b in raw_data[0:4])
        remote_port = (raw_data[4] << 8) | raw_data[5]
        data_len = (raw_data[6] << 8) | raw_data[7]
        payload = raw_data[8:8+data_len]
        
        return remote_ip, remote_port, payload

    def send_udp_packet(self, remote_ip, remote_port, payload, sn=1):
        """지정한 상대방 IP 및 포트로 UDP 패킷 전송"""
        try:
            ip_bytes = bytes([int(x) for x in remote_ip.split('.')])
            self._write_sn(sn, 0x000C, ip_bytes) # Destination IP
            self._write_sn(sn, 0x0010, bytes([(remote_port >> 8) & 0xFF, remote_port & 0xFF])) # Destination Port
            self.send_tx_data(payload, sn=sn)
        except Exception as e:
            print("⚠️ send_udp_packet 오류:", e)

