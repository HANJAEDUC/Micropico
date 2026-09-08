"""
PC에서 Pico 2 (192.168.10.177)로 SNMP GET, GETNEXT, SET(LED ON/OFF) 패킷을 전송하는 자동 테스트 스크립트입니다.
파이썬 기본 socket 모듈만 사용하여 바로 실행 가능합니다.
"""
import socket
import time

TARGET_IP = "192.168.10.177"
TARGET_PORT = 161

def encode_length(length):
    if length < 0x80:
        return bytes([length])
    elif length <= 0xFF:
        return bytes([0x81, length])
    else:
        return bytes([0x82, (length >> 8) & 0xFF, length & 0xFF])

def encode_tlv(tag, value):
    return bytes([tag]) + encode_length(len(value)) + value

def encode_integer(val):
    if val == 0:
        return encode_tlv(0x02, bytes([0]))
    res = bytearray()
    neg = val < 0
    if neg:
        val = (1 << 32) + val
    while val > 0:
        res.insert(0, val & 0xFF)
        val >>= 8
    if not neg and res[0] & 0x80:
        res.insert(0, 0)
    return encode_tlv(0x02, bytes(res))

def encode_string(text):
    return encode_tlv(0x04, text.encode('utf-8') if isinstance(text, str) else text)

def encode_oid(oid_str):
    parts = [int(p) for p in oid_str.strip('.').split('.')]
    res = bytearray()
    res.append(parts[0] * 40 + parts[1])
    for val in parts[2:]:
        if val == 0:
            res.append(0)
        else:
            buf = bytearray()
            while val > 0:
                buf.insert(0, (val & 0x7F) | 0x80 if len(buf) > 0 else (val & 0x7F))
                val >>= 7
            res.extend(buf)
    return encode_tlv(0x06, bytes(res))

def build_snmp_packet(pdu_type, req_id, oid_str, val_integer=None, community="public"):
    enc_oid = encode_oid(oid_str)
    if val_integer is not None:
        enc_val = encode_integer(val_integer)
    else:
        enc_val = bytes([0x05, 0x00]) # Null for GET / GETNEXT
        
    varbind = encode_tlv(0x30, enc_oid + enc_val)
    varbind_list = encode_tlv(0x30, varbind)
    
    pdu = encode_tlv(pdu_type, encode_integer(req_id) + encode_integer(0) + encode_integer(0) + varbind_list)
    packet = encode_tlv(0x30, encode_integer(1) + encode_string(community) + pdu)
    return packet

def parse_ber_length(data, offset):
    if offset >= len(data):
        return 0, offset
    b = data[offset]
    offset += 1
    if b < 0x80:
        return b, offset
    else:
        num_bytes = b & 0x7F
        length = 0
        for _ in range(num_bytes):
            if offset < len(data):
                length = (length << 8) | data[offset]
                offset += 1
        return length, offset

def parse_ber_tlv(data, offset):
    if offset >= len(data):
        return None, None, offset
    tag = data[offset]
    offset += 1
    length, offset = parse_ber_length(data, offset)
    value = data[offset:offset+length]
    offset += length
    return tag, value, offset

def parse_oid(oid_bytes):
    if not oid_bytes:
        return ""
    parts = [str(oid_bytes[0] // 40), str(oid_bytes[0] % 40)]
    val = 0
    for b in oid_bytes[1:]:
        val = (val << 7) | (b & 0x7F)
        if not (b & 0x80):
            parts.append(str(val))
            val = 0
    return "." + ".".join(parts)

def parse_snmp_response(resp):
    try:
        # 1. Outer Sequence
        msg_tag, msg_val, _ = parse_ber_tlv(resp, 0)
        if msg_tag != 0x30 or not msg_val:
            return None, None

        # 2. Version & Community
        ver_tag, ver_val, offset = parse_ber_tlv(msg_val, 0)
        comm_tag, comm_val, offset = parse_ber_tlv(msg_val, offset)

        # 3. PDU (0xA2)
        pdu_tag, pdu_val, _ = parse_ber_tlv(msg_val, offset)
        if pdu_tag != 0xA2 or not pdu_val:
            return None, None

        # 4. Skip ReqID, ErrStat, ErrIdx
        _, _, pdu_off = parse_ber_tlv(pdu_val, 0)
        _, _, pdu_off = parse_ber_tlv(pdu_val, pdu_off)
        _, _, pdu_off = parse_ber_tlv(pdu_val, pdu_off)

        # 5. Varbind List -> First Varbind
        vlist_tag, vlist_val, _ = parse_ber_tlv(pdu_val, pdu_off)
        vbind_tag, vbind_val, _ = parse_ber_tlv(vlist_val, 0)

        # 6. OID & Value
        oid_tag, oid_bytes, vbind_off = parse_ber_tlv(vbind_val, 0)
        oid_str = parse_oid(oid_bytes)

        val_tag, val_bytes, _ = parse_ber_tlv(vbind_val, vbind_off)

        if val_tag == 0x02: # INTEGER
            val = 0
            for b in val_bytes:
                val = (val << 8) | b
            return oid_str, val
        elif val_tag == 0x04: # STRING
            return oid_str, val_bytes.decode('utf-8', 'ignore')
        elif val_tag == 0x43: # TIMETICKS
            val = 0
            for b in val_bytes:
                val = (val << 8) | b
            return oid_str, val
        else:
            return oid_str, f"Tag(0x{val_tag:02X})"

    except Exception as e:
        return None, None

def send_snmp(sock, req_bytes):
    sock.sendto(req_bytes, (TARGET_IP, TARGET_PORT))
    sock.settimeout(2.0)
    try:
        data, _ = sock.recvfrom(1024)
        return data
    except socket.timeout:
        return None

def main():
    print("=" * 60)
    print(f"[SNMP TEST] W5500-EVB-Pico2 ({TARGET_IP}) SNMP Auto Test")
    print("=" * 60)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # 1. [GET] CPU Temp (.1.3.6.1.4.1.99999.1.1.0)
    print("\n1. [SNMP GET 0xA0] CPU Temp (.1.3.6.1.4.1.99999.1.1.0)...")
    pkt = build_snmp_packet(0xA0, 1, ".1.3.6.1.4.1.99999.1.1.0")
    resp = send_snmp(sock, pkt)
    if resp:
        ret_oid, val = parse_snmp_response(resp)
        if isinstance(val, (int, float)):
            print(f"   => Response OK! [{ret_oid}] CPU Temp: {val / 10.0} °C (Raw: {val})")
        else:
            print(f"   => Response OK! [{ret_oid}] Payload: {val}")
    else:
        print("   -> No Response (Please check Pico 2 IP / network link)")

    time.sleep(0.5)

    # 2. [GETNEXT] Walk from sysDescr (.1.3.6.1.2.1.1.1.0)
    print("\n2. [SNMP GETNEXT 0xA1] From sysDescr (.1.3.6.1.2.1.1.1.0)...")
    pkt = build_snmp_packet(0xA1, 2, ".1.3.6.1.2.1.1.1.0")
    resp = send_snmp(sock, pkt)
    if resp:
        ret_oid, val = parse_snmp_response(resp)
        print(f"   => Response OK! Next OID [{ret_oid}] = {val}")
    else:
        print("   -> No Response")

    time.sleep(0.5)

    # 3. [GET] LED Status (.1.3.6.1.4.1.99999.1.2.0)
    print("\n3. [SNMP GET 0xA0] LED Status (.1.3.6.1.4.1.99999.1.2.0)...")
    pkt = build_snmp_packet(0xA0, 3, ".1.3.6.1.4.1.99999.1.2.0")
    resp = send_snmp(sock, pkt)
    if resp:
        ret_oid, val = parse_snmp_response(resp)
        print(f"   => Current LED Status [{ret_oid}]: {'ON (1)' if val == 1 else 'OFF (0)'}")

    time.sleep(0.5)

    # 4. [SET] LED ON (Send 1)
    print("\n4. [SNMP SET 0xA3] Turn LED ON (Send 1)...")
    pkt = build_snmp_packet(0xA3, 4, ".1.3.6.1.4.1.99999.1.2.0", val_integer=1)
    resp = send_snmp(sock, pkt)
    if resp:
        ret_oid, val = parse_snmp_response(resp)
        print(f"   => Pico 2 Response [{ret_oid}]: LED State changed to {'ON (1)' if val == 1 else 'OFF (0)'}!")

    time.sleep(1)

    # 5. [SET] LED OFF (Send 0)
    print("\n5. [SNMP SET 0xA3] Turn LED OFF (Send 0)...")
    pkt = build_snmp_packet(0xA3, 5, ".1.3.6.1.4.1.99999.1.2.0", val_integer=0)
    resp = send_snmp(sock, pkt)
    if resp:
        ret_oid, val = parse_snmp_response(resp)
        print(f"   => Pico 2 Response [{ret_oid}]: LED State changed to {'ON (1)' if val == 1 else 'OFF (0)'}!")

    print("\n" + "=" * 60)
    print("[SUCCESS] SNMP Test Finished!")
    print("=" * 60)
    sock.close()

if __name__ == "__main__":
    main()
