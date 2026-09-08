import time

class SimpleSNMPAgent:
    """
    W5500-EVB-Pico2 경량화 SNMP v1/v2c Agent
    UDP 161 포트로 수신된 SNMP GET (0xA0), GETNEXT (0xA1), SET (0xA3), GETBULK (0xA5) 요청에 응답합니다.
    """
    # MIB OID 테이블 (정렬 순서 유지)
    SUPPORTED_OIDS = [
        (".1.3.6.1.2.1.1.1.0", "OCTET_STRING", "W5500-EVB-Pico2 RP2350 SNMP Agent"),
        (".1.3.6.1.2.1.1.3.0", "TIMETICKS", None), # Dynamic Uptime
        (".1.3.6.1.2.1.1.5.0", "OCTET_STRING", "Pico2-W5500-Industrial-Device"),
        (".1.3.6.1.4.1.99999.1.1.0", "INTEGER_TEMP", None), # Dynamic Temp x10
        (".1.3.6.1.4.1.99999.1.2.0", "INTEGER_LED", None),  # Dynamic LED Status
    ]

    def __init__(self, community="public", temp_func=None, led_pin=None):
        self.community = community
        self.temp_func = temp_func
        self.led_pin = led_pin
        try:
            self.start_time = time.ticks_ms()
        except AttributeError:
            self.start_time = int(time.time() * 1000)

    def _get_uptime_cs(self):
        try:
            return time.ticks_diff(time.ticks_ms(), self.start_time) // 10
        except AttributeError:
            return int((time.time() * 1000 - self.start_time) / 10)

    def _encode_length(self, length):
        if length < 0x80:
            return bytes([length])
        elif length <= 0xFF:
            return bytes([0x81, length])
        else:
            return bytes([0x82, (length >> 8) & 0xFF, length & 0xFF])

    def _encode_tlv(self, tag, value):
        return bytes([tag]) + self._encode_length(len(value)) + value

    def _encode_integer(self, val):
        if val == 0:
            return self._encode_tlv(0x02, bytes([0]))
        res = []
        neg = val < 0
        if neg:
            val = (1 << 32) + val
        while val > 0:
            res.insert(0, val & 0xFF)
            val >>= 8
        if not neg and res and (res[0] & 0x80):
            res.insert(0, 0)
        return self._encode_tlv(0x02, bytes(res))

    def _encode_string(self, text):
        return self._encode_tlv(0x04, text.encode('utf-8') if isinstance(text, str) else text)

    def _encode_oid(self, oid_str):
        parts = [int(p) for p in oid_str.strip('.').split('.')]
        res = bytearray()
        res.append(parts[0] * 40 + parts[1])
        for val in parts[2:]:
            if val == 0:
                res.append(0)
            else:
                buf = []
                while val > 0:
                    buf.insert(0, (val & 0x7F) | 0x80 if len(buf) > 0 else (val & 0x7F))
                    val >>= 7
                res.extend(bytes(buf))
        return self._encode_tlv(0x06, bytes(res))

    def _parse_ber_length(self, data, offset):
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

    def _parse_ber_tlv(self, data, offset):
        if offset >= len(data):
            return None, None, offset
        tag = data[offset]
        offset += 1
        length, offset = self._parse_ber_length(data, offset)
        value = data[offset:offset+length]
        offset += length
        return tag, value, offset

    def parse_oid_from_raw(self, oid_bytes):
        if len(oid_bytes) == 0:
            return ""
        parts = [str(oid_bytes[0] // 40), str(oid_bytes[0] % 40)]
        val = 0
        for b in oid_bytes[1:]:
            val = (val << 7) | (b & 0x7F)
            if not (b & 0x80):
                parts.append(str(val))
                val = 0
        return "." + ".".join(parts)

    def _oid_to_tuple(self, oid_str):
        return tuple(int(x) for x in oid_str.strip('.').split('.'))

    def get_oid_value(self, oid_str):
        if not oid_str.startswith('.'):
            oid_str = '.' + oid_str

        for target_oid, val_type, static_val in self.SUPPORTED_OIDS:
            if target_oid == oid_str:
                return self._resolve_val(target_oid, val_type, static_val)

        return "NOSUCHOBJECT", None

    def get_next_oid_value(self, requested_oid_str):
        if not requested_oid_str.startswith('.'):
            requested_oid_str = '.' + requested_oid_str

        try:
            req_tuple = self._oid_to_tuple(requested_oid_str)
        except Exception:
            req_tuple = (0,)

        for target_oid, val_type, static_val in self.SUPPORTED_OIDS:
            target_tuple = self._oid_to_tuple(target_oid)
            if target_tuple > req_tuple:
                vtype, val = self._resolve_val(target_oid, val_type, static_val)
                return target_oid, vtype, val

        return None, "NOSUCHOBJECT", None

    def _resolve_val(self, oid_str, val_type, static_val):
        if val_type == "OCTET_STRING":
            return "OCTET_STRING", static_val
        elif val_type == "TIMETICKS":
            return "TIMETICKS", self._get_uptime_cs()
        elif val_type == "INTEGER_TEMP":
            temp = self.temp_func() if self.temp_func else 0
            return "INTEGER", int(temp * 10)
        elif val_type == "INTEGER_LED":
            led_val = self.led_pin.value() if self.led_pin else 0
            return "INTEGER", led_val
        return "NOSUCHOBJECT", None

    def set_oid_value(self, oid_str, set_val):
        if not oid_str.startswith('.'):
            oid_str = '.' + oid_str

        if oid_str == ".1.3.6.1.4.1.99999.1.2.0":
            if self.led_pin is not None and set_val is not None:
                new_state = 1 if set_val != 0 else 0
                self.led_pin.value(new_state)
                print(f"💡 [SNMP SET] LED Pin State -> {'ON (1)' if new_state else 'OFF (0)'}")
                return "INTEGER", new_state
        return "NOSUCHOBJECT", None

    def process_snmp_request(self, payload):
        try:
            if len(payload) < 10 or payload[0] != 0x30:
                return None

            # 1. Parse outer sequence (Message)
            seq_tag, seq_val, _ = self._parse_ber_tlv(payload, 0)
            if seq_tag != 0x30 or seq_val is None:
                return None

            # 2. Parse Version
            ver_tag, ver_val, ver_offset = self._parse_ber_tlv(seq_val, 0)
            if ver_tag != 0x02 or not ver_val:
                return None
            req_version = ver_val # preserve bytes for response (e.g. b'\x00' or b'\x01')

            # 3. Parse Community String
            comm_tag, comm_val, comm_offset = self._parse_ber_tlv(seq_val, ver_offset)
            if comm_tag != 0x04:
                return None

            # 4. Parse PDU Tag (0xA0: Get, 0xA1: GetNext, 0xA3: Set, 0xA5: GetBulk)
            pdu_tag, pdu_val, _ = self._parse_ber_tlv(seq_val, comm_offset)
            if pdu_tag not in (0xA0, 0xA1, 0xA3, 0xA5) or pdu_val is None:
                return None

            # 5. Parse PDU Header (Request ID, Error-Status / Non-repeaters, Error-Index / Max-repetitions)
            req_id_tag, req_id_val, req_id_offset = self._parse_ber_tlv(pdu_val, 0)
            err_stat_tag, err_stat_val, err_stat_offset = self._parse_ber_tlv(pdu_val, req_id_offset)
            err_idx_tag, err_idx_val, err_idx_offset = self._parse_ber_tlv(pdu_val, err_stat_offset)

            # 6. Parse Varbind List
            varbind_list_tag, varbind_list_val, _ = self._parse_ber_tlv(pdu_val, err_idx_offset)
            if varbind_list_tag != 0x30 or varbind_list_val is None:
                return None

            # 7. Parse First Varbind
            varbind_tag, varbind_val, _ = self._parse_ber_tlv(varbind_list_val, 0)
            if varbind_tag != 0x30 or varbind_val is None:
                return None

            oid_tag, oid_bytes, oid_offset = self._parse_ber_tlv(varbind_val, 0)
            if oid_tag != 0x06:
                return None

            oid_str = self.parse_oid_from_raw(oid_bytes)

            # Check if SET value exists
            set_val = None
            if pdu_tag == 0xA3 and oid_offset < len(varbind_val):
                v_tag, v_val, _ = self._parse_ber_tlv(varbind_val, oid_offset)
                if v_tag == 0x02 and v_val: # INTEGER
                    set_val = 0
                    for b in v_val:
                        set_val = (set_val << 8) | b

            # Execute Request
            res_oid_str = oid_str
            if pdu_tag == 0xA0: # GetRequest
                val_type, val = self.get_oid_value(oid_str)
            elif pdu_tag in (0xA1, 0xA5): # GetNextRequest or GetBulkRequest
                next_oid, val_type, val = self.get_next_oid_value(oid_str)
                if next_oid:
                    res_oid_str = next_oid
                else:
                    res_oid_str = oid_str
                    val_type, val = "NOSUCHOBJECT", None
            elif pdu_tag == 0xA3: # SetRequest
                val_type, val = self.set_oid_value(oid_str, set_val)
            else:
                val_type, val = "NOSUCHOBJECT", None

            # 8. Encode Response Varbind
            enc_oid = self._encode_oid(res_oid_str)
            if val_type == "OCTET_STRING":
                enc_val = self._encode_string(val)
            elif val_type == "TIMETICKS":
                enc_val = self._encode_tlv(0x43, self._encode_integer(val)[2:])
            elif val_type == "INTEGER":
                enc_val = self._encode_integer(val)
            else:
                enc_val = bytes([0x80, 0x00]) # NoSuchObject

            res_varbind = self._encode_tlv(0x30, enc_oid + enc_val)
            res_varbind_list = self._encode_tlv(0x30, res_varbind)

            # 9. Encode PDU Response (0xA2: GetResponse)
            pdu_resp_data = (
                self._encode_tlv(0x02, req_id_val) +
                self._encode_integer(0) +
                self._encode_integer(0) +
                res_varbind_list
            )
            pdu_resp = self._encode_tlv(0xA2, pdu_resp_data)

            # 10. Encode Outer SNMP Message
            version_enc = self._encode_tlv(0x02, req_version)
            comm_enc = self._encode_tlv(0x04, comm_val)

            snmp_resp = self._encode_tlv(0x30, version_enc + comm_enc + pdu_resp)
            return snmp_resp

        except Exception as e:
            print("⚠️ SNMP Exception:", e)
            return None


