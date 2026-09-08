"""
W5500-EVB-Pico2 전용 PC SNMP CLI 도구 (Net-SNMP 표준 문법 + 간편 문법 동시 지원)
"""
import sys
import socket
import time

if hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

DEFAULT_TARGET_IP = "192.168.10.177"
DEFAULT_TARGET_PORT = 161
DEFAULT_COMMUNITY = "public"

SHORTCUT_OIDS = {
    "temp": ".1.3.6.1.4.1.99999.1.1.0",
    "led": ".1.3.6.1.4.1.99999.1.2.0",
    "descr": ".1.3.6.1.2.1.1.1.0",
    "uptime": ".1.3.6.1.2.1.1.3.0",
    "name": ".1.3.6.1.2.1.1.5.0",
}

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
        enc_val = bytes([0x05, 0x00]) # Null
        
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
        msg_tag, msg_val, _ = parse_ber_tlv(resp, 0)
        if msg_tag != 0x30 or not msg_val:
            return None, None
        ver_tag, ver_val, offset = parse_ber_tlv(msg_val, 0)
        comm_tag, comm_val, offset = parse_ber_tlv(msg_val, offset)
        pdu_tag, pdu_val, _ = parse_ber_tlv(msg_val, offset)
        if pdu_tag != 0xA2 or not pdu_val:
            return None, None

        _, _, pdu_off = parse_ber_tlv(pdu_val, 0)
        _, _, pdu_off = parse_ber_tlv(pdu_val, pdu_off)
        _, _, pdu_off = parse_ber_tlv(pdu_val, pdu_off)

        vlist_tag, vlist_val, _ = parse_ber_tlv(pdu_val, pdu_off)
        vbind_tag, vbind_val, _ = parse_ber_tlv(vlist_val, 0)

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
            return oid_str, f"{val / 100.0:.2f}s (TimeTicks: {val})"
        else:
            return oid_str, f"Tag(0x{val_tag:02X})"
    except Exception:
        return None, None

def query_snmp(pdu_type, oid_str, val=None, target_ip=DEFAULT_TARGET_IP, port=DEFAULT_TARGET_PORT, community=DEFAULT_COMMUNITY):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)
    pkt = build_snmp_packet(pdu_type, int(time.time()) & 0xFFFF, oid_str, val, community)
    try:
        sock.sendto(pkt, (target_ip, port))
        resp, _ = sock.recvfrom(1024)
        return parse_snmp_response(resp)
    except socket.timeout:
        return None, None
    finally:
        sock.close()

def parse_args_and_execute(args, default_action=None):
    # args parsing
    # Handles:
    # 1) snmpset/snmpget/snmpwalk standard syntax: -v2c -c public 192.168.10.177 <OID> [i/s] [val]
    # 2) shortcut syntax: get temp, set led 1, walk
    
    action = default_action
    ip = DEFAULT_TARGET_IP
    community = DEFAULT_COMMUNITY
    oid = None
    val = None

    pos_args = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-v1", "-v2c", "-v3"):
            i += 1
        elif a == "-c" and i + 1 < len(args):
            community = args[i+1]
            i += 2
        elif a.startswith("-"):
            i += 1
        else:
            pos_args.append(a)
            i += 1

    if not action and pos_args:
        first = pos_args[0].lower()
        if first in ("get", "set", "walk"):
            action = first
            pos_args = pos_args[1:]

    # Parse positional arguments
    if pos_args:
        # Check if first pos_arg looks like an IP
        if pos_args[0].count('.') == 3 and not pos_args[0].startswith('.'):
            ip = pos_args[0]
            pos_args = pos_args[1:]

    if pos_args:
        oid = pos_args[0]
        pos_args = pos_args[1:]

    if pos_args:
        # For SET: type flag ('i', 's', 'u') or direct value
        if pos_args[0].lower() in ('i', 'integer', 's', 'str', 'string', 'u', 'uint'):
            if len(pos_args) > 1:
                val = int(pos_args[1]) if pos_args[1].isdigit() else pos_args[1]
        else:
            val = int(pos_args[0]) if pos_args[0].isdigit() else pos_args[0]

    # Resolve shortcut OID
    if oid:
        oid = SHORTCUT_OIDS.get(oid.lower(), oid)
    else:
        if action == "walk":
            oid = ".1.3.6.1.2.1.1.1.0"
        elif action == "get":
            oid = SHORTCUT_OIDS["temp"]
        elif action == "set":
            oid = SHORTCUT_OIDS["led"]
            if val is None:
                val = 1

    if not action:
        action = "walk"

    # Execution
    if action == "walk":
        print(f"📡 [SNMP WALK] {ip} (Community: {community})")
        print("=" * 60)
        curr_oid = oid or ".1.3.6.1.2.1.1.1.0"
        seen = set()
        while curr_oid and curr_oid not in seen:
            seen.add(curr_oid)
            ret_oid, res_val = query_snmp(0xA0, curr_oid, target_ip=ip, community=community)
            if ret_oid:
                if ret_oid == ".1.3.6.1.4.1.99999.1.1.0" and isinstance(res_val, int):
                    print(f"{ret_oid} = INTEGER: {res_val / 10.0} °C (Raw: {res_val})")
                elif ret_oid == ".1.3.6.1.4.1.99999.1.2.0" and isinstance(res_val, int):
                    print(f"{ret_oid} = INTEGER: {'ON (1)' if res_val == 1 else 'OFF (0)'}")
                else:
                    print(f"{ret_oid} = {res_val}")
            next_oid, _ = query_snmp(0xA1, curr_oid, target_ip=ip, community=community)
            if not next_oid or next_oid == curr_oid:
                break
            curr_oid = next_oid
        print("=" * 60)

    elif action == "get":
        ret_oid, res_val = query_snmp(0xA0, oid, target_ip=ip, community=community)
        if ret_oid:
            if oid == SHORTCUT_OIDS["temp"] and isinstance(res_val, int):
                print(f"{ret_oid} = INTEGER: {res_val / 10.0} °C")
            elif oid == SHORTCUT_OIDS["led"] and isinstance(res_val, int):
                print(f"{ret_oid} = INTEGER: {'ON (1)' if res_val == 1 else 'OFF (0)'}")
            else:
                print(f"{ret_oid} = {res_val}")
        else:
            print(f"❌ [SNMP Timeout] {ip} 응답 없음")

    elif action == "set":
        if val is None:
            print("❌ 설정할 값을 입력하세요. (예: snmpset ... i 0)")
            return
        ret_oid, res_val = query_snmp(0xA3, oid, val=int(val), target_ip=ip, community=community)
        if ret_oid:
            print(f"{ret_oid} = INTEGER: {res_val}")
            print(f"✅ [SNMP SET 완료] {ret_oid} => {'ON (1)' if res_val == 1 else 'OFF (0)'}")
        else:
            print(f"❌ [SNMP Timeout] {ip} 응답 없음")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("사용법:")
        print("  snmpset -v2c -c public 192.168.10.177 .1.3.6.1.4.1.99999.1.2.0 i 0")
        print("  snmpget -v2c -c public 192.168.10.177 .1.3.6.1.4.1.99999.1.1.0")
        print("  snmpwalk -v2c -c public 192.168.10.177")
        print("  또는: snmp set led 0 / snmp get temp / snmp walk")
    else:
        parse_args_and_execute(sys.argv[1:])
