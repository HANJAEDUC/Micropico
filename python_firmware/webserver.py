import machine
import time
import gc
from w5500_driver import W5500
from snmp_agent import SimpleSNMPAgent
import config

# 온보드 하드웨어 제어
led = machine.Pin("LED", machine.Pin.OUT)
temp_adc = machine.ADC(4)

def read_cpu_temp():
    """RP2350 CPU 내부 온도 센서 측정 (섭씨)"""
    reading = temp_adc.read_u16()
    voltage = reading * (3.3 / 65535.0)
    temperature = 27.0 - (voltage - 0.706) / 0.001721
    return round(temperature, 1)

import os
import json

def url_unquote(s):
    if '%' not in s and '+' not in s:
        return s
    res = bytearray()
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '%':
            if i + 2 < n:
                try:
                    res.append(int(s[i+1:i+3], 16))
                    i += 3
                    continue
                except Exception:
                    pass
        elif c == '+':
            res.append(0x20)
            i += 1
            continue
        res.append(ord(c))
        i += 1
    return res.decode('utf-8', 'ignore')

def extract_big_bin_checksum(filename, total_sum=0):
    """
    파일명의 0xXXXXXXXX 패턴 파싱 (예: 0x0524F88B -> 0524 F88B)
    없을 시 32-bit 바이트 합산(total_sum)으로 상위/하위 16비트 계산
    """
    try:
        if filename:
            fn_lower = filename.lower()
            idx = 0
            while True:
                idx = fn_lower.find("0x", idx)
                if idx == -1:
                    break
                if len(filename) >= idx + 10:
                    hex_part = filename[idx+2 : idx+10]
                    if len(hex_part) == 8 and all(c in "0123456789abcdefABCDEF" for c in hex_part):
                        return f"{hex_part[:4].upper()} {hex_part[4:].upper()}"
                idx += 2
    except Exception:
        pass
    
    h = (total_sum >> 16) & 0xFFFF
    l = total_sum & 0xFFFF
    return f"{h:04X} {l:04X}"

def get_firmware_info():
    """Pico 2 플래시 메모리에 저장된 firmware.bin 및 첵섬 메타데이터 정보 확인"""
    try:
        stat = os.stat("firmware.bin")
        size_bytes = stat[6]
        size_kb = round(size_bytes / 1024, 1)
        size_mb = round(size_bytes / (1024 * 1024), 2)
        
        # 메타데이터 파일 확인 (firmware_info.json)
        big_bin_chk = "알수없음"
        sum32_chk = "알수없음"
        fname = "firmware.bin"
        
        try:
            with open("firmware_info.json", "r") as f:
                info = json.load(f)
                big_bin_chk = info.get("big_bin_checksum", "알수없음")
                sum32_chk = info.get("sum32_checksum", "알수없음")
                fname = info.get("filename", "firmware.bin")
        except Exception:
            pass

        return True, {
            "size_bytes": size_bytes,
            "size_kb": size_kb,
            "size_mb": size_mb,
            "filename": fname,
            "big_bin_checksum": big_bin_chk,
            "sum32_checksum": sum32_chk
        }
    except Exception:
        return False, None

# HTML UI 템플릿
def build_html_response():
    temp = read_cpu_temp()
    led_state = "ON 🟢" if led.value() else "OFF 🔴"
    fw_exists, fw_data = get_firmware_info()
    
    if fw_exists:
        fw_status_html = f"""
        <div style="text-align: left; background: #0f172a; padding: 15px; border-radius: 10px; border: 1px solid #334155; font-size: 14px;">
            <div style="margin-bottom: 8px;"><strong>📄 저장된 파일명:</strong> <span style="color: #cbd5e1; word-break: break-all;">{fw_data['filename']}</span></div>
            <div style="margin-bottom: 8px;"><strong>📏 저장된 용량:</strong> <span style="color: #4ade80;">{fw_data['size_bytes']:,} Bytes ({fw_data['size_kb']} KB)</span></div>
            <div style="margin-bottom: 8px;"><strong>🎯 Big Bin Checksum (Realtek):</strong> <span style="color: #38bdf8; font-weight: bold; font-family: monospace; font-size: 16px;">{fw_data['big_bin_checksum']}</span></div>
            <div><strong>🧮 32-bit Byte Sum Checksum:</strong> <span style="color: #facc15; font-weight: bold; font-family: monospace; font-size: 15px;">{fw_data['sum32_checksum']}</span></div>
        </div>"""
    else:
        fw_status_html = '<div style="color: #94a3b8; font-size: 14px; background: #0f172a; padding: 12px; border-radius: 8px; border: 1px solid #334155;">저장된 펌웨어 없음</div>'

    html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>DHUB PICO2</title>
    <style>
        body {{ font-family: 'Segoe UI', Arial, sans-serif; background: #0f172a; color: #f8fafc; text-align: center; padding: 30px 15px; margin: 0; }}
        .container {{ max-width: 540px; margin: 0 auto; display: flex; flex-direction: column; gap: 20px; }}
        .card {{ background: #1e293b; border-radius: 16px; padding: 25px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); border: 1px solid #334155; }}
        h1 {{ color: #38bdf8; font-size: 22px; margin-top: 0; margin-bottom: 15px; }}
        h2 {{ color: #f1f5f9; font-size: 18px; margin-top: 0; margin-bottom: 15px; border-bottom: 1px solid #334155; padding-bottom: 10px; }}
        .stat {{ font-size: 15px; margin: 10px 0; color: #cbd5e1; }}
        .val {{ font-size: 24px; font-weight: bold; color: #4ade80; margin-top: 5px; }}
        .btn-group {{ margin-top: 15px; display: flex; gap: 10px; justify-content: center; }}
        .btn {{ padding: 10px 20px; border: none; border-radius: 8px; font-size: 15px; font-weight: bold; cursor: pointer; transition: 0.2s; text-decoration: none; color: white; }}
        .btn-on {{ background: #22c55e; }}
        .btn-off {{ background: #ef4444; }}
        .btn-upload {{ background: #0284c7; width: 100%; margin-top: 12px; padding: 12px; font-size: 16px; }}
        .btn:hover {{ opacity: 0.85; transform: translateY(-2px); }}
        .file-input {{ background: #0f172a; border: 1px dashed #475569; padding: 12px; border-radius: 8px; width: 100%; color: #94a3b8; box-sizing: border-box; font-size: 14px; cursor: pointer; }}
        #progress-container {{ display: none; margin-top: 12px; background: #0f172a; border-radius: 8px; overflow: hidden; border: 1px solid #334155; height: 20px; position: relative; }}
        #progress-bar {{ width: 0%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); transition: width 0.1s; }}
        #progress-text {{ position: absolute; width: 100%; top: 0; left: 0; line-height: 20px; font-size: 12px; font-weight: bold; color: #ffffff; text-shadow: 0 0 4px #000; }}
        #status-msg {{ margin-top: 10px; font-size: 14px; min-height: 20px; text-align: left; white-space: pre-wrap; }}
        #selected-file-info {{ display: none; margin-top: 12px; }}
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <h1>⚡ Pico 2 (RP2350) 시스템 대시보드</h1>
            <div class="stat">CPU 온도: <span class="val" style="font-size:20px;">🌡️ {temp} °C</span></div>
            <div class="stat">LED 상태: <span class="val" style="font-size:20px;">{led_state}</span></div>
            <div class="btn-group">
                <a href="/led/on" class="btn btn-on">LED ON</a>
                <a href="/led/off" class="btn btn-off">LED OFF</a>
            </div>
        </div>

        <div class="card">
            <h2>📦 Realtek 펌웨어 (.bin) 웹 업로드</h2>
            <div style="margin-bottom: 15px;">
                <div style="font-size: 13px; color: #94a3b8; margin-bottom: 8px; text-align: left;">💾 저장된 펌웨어 정보:</div>
                {fw_status_html}
            </div>
            <div style="margin-top: 15px; text-align: left;">
                <label style="font-size: 13px; color: #94a3b8; display: block; margin-bottom: 6px;">📂 업로드할 펌웨어 파일 선택:</label>
                <input type="file" id="fwFile" class="file-input" accept=".bin,.hex,.uf2">
                <div id="selected-file-info"></div>
                <button onclick="uploadFirmware()" id="uploadBtn" class="btn btn-upload">🚀 Pico 2 플래시 메모리에 업로드</button>
            </div>
            <div id="progress-container">
                <div id="progress-bar"></div>
                <div id="progress-text">0%</div>
            </div>
            <div id="status-msg"></div>
        </div>
    </div>

    <script>
    document.getElementById('fwFile').addEventListener('change', function(e) {{
        const file = e.target.files[0];
        const infoDiv = document.getElementById('selected-file-info');
        if (!file) {{
            infoDiv.style.display = 'none';
            return;
        }}

        const fname = file.name;
        let bigBinFromFn = null;
        const match = fname.match(/0[xX]([0-9a-fA-F]{8})/);
        if (match && match[1]) {{
            const hex = match[1].toUpperCase();
            bigBinFromFn = hex.substring(0, 4) + ' ' + hex.substring(4, 8);
        }}

        infoDiv.style.display = 'block';
        infoDiv.innerHTML = `
            <div style="background: #0f172a; padding: 15px; border-radius: 10px; border: 1px solid #38bdf8; text-align: left; font-size: 14px;">
                <div style="color: #38bdf8; font-weight: bold; margin-bottom: 8px; font-size: 15px;">🔍 선택된 파일 첵섬 분석 중...</div>
                <div style="margin-bottom: 6px;"><strong>📄 파일명:</strong> <span style="color: #cbd5e1; word-break: break-all;">${{fname}}</span></div>
                <div><strong>📏 용량:</strong> <span style="color: #4ade80;">${{file.size.toLocaleString()}} Bytes (${{(file.size/1024).toFixed(1)}} KB)</span></div>
            </div>`;

        const reader = new FileReader();
        reader.onload = function(evt) {{
            const buffer = evt.target.result;
            const bytes = new Uint8Array(buffer);
            let totalSum = 0;
            for (let i = 0; i < bytes.length; i++) {{
                totalSum = (totalSum + bytes[i]) >>> 0;
            }}

            const h = ((totalSum >>> 16) & 0xFFFF).toString(16).padStart(4, '0').toUpperCase();
            const l = (totalSum & 0xFFFF).toString(16).padStart(4, '0').toUpperCase();
            const sumHex = totalSum.toString(16).padStart(8, '0').toUpperCase();
            const sum32Chk = `${{h}} ${{l}} (0x${{sumHex}})`;

            const bigBinDisplay = bigBinFromFn ? `${{bigBinFromFn}} (파일명 식별자 / RTDTool v3.1.3)` : `${{h}} ${{l}} (RTDTool v3.1.3)`;

            infoDiv.innerHTML = `
                <div style="background: #0f172a; padding: 15px; border-radius: 10px; border: 1px solid #0284c7; text-align: left; font-size: 14px;">
                    <div style="color: #38bdf8; font-weight: bold; margin-bottom: 10px; font-size: 15px; border-bottom: 1px solid #1e293b; padding-bottom: 6px;">🔍 선택된 파일 첵섬 분석 결과</div>
                    <div style="margin-bottom: 8px;"><strong>📄 파일명:</strong> <span style="color: #cbd5e1; word-break: break-all;">${{fname}}</span></div>
                    <div style="margin-bottom: 8px;"><strong>📏 용량:</strong> <span style="color: #4ade80;">${{file.size.toLocaleString()}} Bytes (${{(file.size/1024).toFixed(1)}} KB)</span></div>
                    <div style="margin-bottom: 8px;"><strong>🎯 Big Bin Checksum (RTDTool v3.1.3):</strong> <span style="color: #38bdf8; font-weight: bold; font-family: monospace; font-size: 16px;">${{bigBinDisplay}}</span></div>
                    <div><strong>🧮 32-bit Byte Sum Checksum (RTDTool v3.8):</strong> <span style="color: #facc15; font-weight: bold; font-family: monospace; font-size: 15px;">${{sum32Chk}}</span></div>
                </div>`;
        }};
        reader.onerror = function() {{
            infoDiv.innerHTML = `<div style="color: #ef4444; font-size: 14px;">❌ 파일 읽기 오류 발생</div>`;
        }};
        reader.readAsArrayBuffer(file);
    }});

    function uploadFirmware() {{
        const fileInput = document.getElementById('fwFile');
        const file = fileInput.files[0];
        if (!file) {{
            alert('업로드할 .bin 펌웨어 파일을 선택해주세요!');
            return;
        }}

        const btn = document.getElementById('uploadBtn');
        const statusMsg = document.getElementById('status-msg');
        const progressContainer = document.getElementById('progress-container');
        const progressBar = document.getElementById('progress-bar');
        const progressText = document.getElementById('progress-text');

        btn.disabled = true;
        btn.style.opacity = '0.5';
        progressContainer.style.display = 'block';
        statusMsg.style.color = '#38bdf8';
        statusMsg.innerText = 'Pico 2 플래시 메모리에 파일 전송 중...';

        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/upload?name=' + encodeURIComponent(file.name), true);
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        xhr.timeout = 120000;

        xhr.upload.onprogress = function(e) {{
            if (e.lengthComputable) {{
                const percent = Math.round((e.loaded / e.total) * 100);
                progressBar.style.width = percent + '%';
                progressText.innerText = percent + '% (' + (e.loaded / 1024).toFixed(0) + ' KB / ' + (e.total / 1024).toFixed(0) + ' KB)';
            }}
        }};

        xhr.onload = function() {{
            btn.disabled = false;
            btn.style.opacity = '1';
            if (xhr.status === 200) {{
                statusMsg.style.color = '#4ade80';
                statusMsg.innerText = xhr.responseText;
                setTimeout(() => location.reload(), 2500);
            }} else {{
                statusMsg.style.color = '#ef4444';
                statusMsg.innerText = '❌ 업로드 실패: ' + xhr.responseText;
            }}
        }};

        xhr.onerror = function() {{
            btn.disabled = false;
            btn.style.opacity = '1';
            statusMsg.style.color = '#ef4444';
            statusMsg.innerText = '❌ 네트워크 오류 발생!';
        }};

        xhr.ontimeout = function() {{
            btn.disabled = false;
            btn.style.opacity = '1';
            statusMsg.style.color = '#ef4444';
            statusMsg.innerText = '❌ 네트워크 타임아웃 발생 (전송 시간 초과)';
        }};

        xhr.send(file);
    }}
    </script>
</body>
</html>"""
    return html

# 메인 루프
def run_server():
    # SPI 및 W5500 고속 초기화 (RP2350A 전용 30MHz SPI 클럭 설정)
    spi = machine.SPI(config.SPI_BUS, baudrate=30_000_000, polarity=0, phase=0,
                      sck=machine.Pin(config.SCK_PIN),
                      mosi=machine.Pin(config.MOSI_PIN),
                      miso=machine.Pin(config.MISO_PIN))

    w5500 = W5500(spi, config.CS_PIN, config.RST_PIN)
    w5500.setup_network(config.STATIC_IP, config.SUBNET_MASK, config.GATEWAY_IP, config.MAC_ADDRESS)

    # SNMP Agent 서비스 시작 (Socket 1 / UDP Port 161)
    snmp_agent = SimpleSNMPAgent(community="public", temp_func=read_cpu_temp, led_pin=led)

    # PHY 이더넷 링크(Auto-negotiation) 가동 대기 (최대 3초)
    print("🔌 이더넷 물리 링크(PHY Link) 상태 확인 중...")
    for _ in range(30):
        if w5500.is_link_up():
            print("✅ 이더넷 링크 연결 확인 완료! (Link UP)")
            break
        time.sleep_ms(100)
    else:
        print("⚠️ 이더넷 케이블 대기 중... (케이블 연결 시 자동 복구됩니다)")

    w5500.listen_server(config.HTTP_PORT)
    w5500.open_udp_socket(sn=1, port=161)

    print("=" * 60)
    print("🚀 W5500-EVB-Pico2 서비스 가동 완료!")
    print(f"🌐 Web Server: http://{w5500.get_ip()}:{config.HTTP_PORT} (TCP 80)")
    print(f"📡 SNMP Agent: udp://{w5500.get_ip()}:161 (UDP 161)")
    print("=" * 60)
    print("⏳ 수신 대기 중... (웹 접속 및 SNMP 요청 대기)")
    
    link_is_up = True
    loop_count = 0
    link_down_consecutive = 0

    while True:
        try:
            # 0. 이더넷 케이블 물리적 연결 (PHY Link) 주기적 감지
            loop_count += 1
            if loop_count >= 50:
                loop_count = 0
                current_link = w5500.is_link_up()
                if not current_link:
                    link_down_consecutive += 1
                    if link_down_consecutive >= 2 and link_is_up:
                        print("⚠️ [LAN 케이블 분리 감지] 링크 연결 끊김. 소켓 리셋 중...")
                        w5500.close_socket(0)
                        w5500.close_socket(1)
                        link_is_up = False
                else:
                    link_down_consecutive = 0
                    if not link_is_up:
                        print("✅ [LAN 케이블 재연결 감지] 링크 복구 완료! 웹 및 SNMP 서비스를 재개설합니다.")
                        w5500.listen_server(config.HTTP_PORT)
                        w5500.open_udp_socket(sn=1, port=161)
                        link_is_up = True

            if not link_is_up:
                time.sleep_ms(100)
                continue

            # 1. Socket 0: 웹 서버 (HTTP TCP Port 80)
            status = w5500.get_socket_status(sn=0)
            
            if status == W5500.SOCK_ESTABLISHED:
                req_bytes = w5500.read_rx_data(sn=0)
                req_str = req_bytes.decode('utf-8', 'ignore')
                
                if len(req_str) > 0:
                    first_line = req_str.split('\r\n')[0]
                    print(f"📥 [HTTP Request] {first_line}")
                    
                    # 펌웨어 파일 POST 업로드 처리
                    if "POST /upload" in first_line:
                        # 파일 이름 파싱
                        orig_filename = "firmware.bin"
                        if "?name=" in first_line or "&name=" in first_line:
                            try:
                                name_part = first_line.split("name=")[1].split(" ")[0].split("&")[0]
                                orig_filename = url_unquote(name_part)
                            except Exception:
                                pass

                        # Content-Length 파싱
                        content_length = 0
                        for line in req_str.split('\r\n'):
                            if line.lower().startswith('content-length:'):
                                content_length = int(line.split(':')[1].strip())
                                break
                        
                        # HTTP 헤더 종료 위치(\r\n\r\n) 찾기
                        header_end = req_bytes.find(b'\r\n\r\n')
                        if header_end != -1:
                            initial_body = req_bytes[header_end + 4:]
                        else:
                            initial_body = b''

                        target_filename = "firmware.bin"
                        print(f"📦 [펌웨어 업로드 시작] {orig_filename} ({content_length:,} bytes) -> {target_filename} (Pico 2 Flash)")
                        
                        written_bytes = 0
                        total_sum = 0
                        start_time = time.time()
                        
                        buffer = bytearray()
                        FLASH_WRITE_SIZE = 4096  # 4KB 단위 Flash 저장 (쓰기 오버헤드 90% 감소 및 소켓 타임아웃 방지)

                        try:
                            with open(target_filename, "wb") as f:
                                if initial_body:
                                    buffer.extend(initial_body)
                                    written_bytes += len(initial_body)
                                    total_sum = (total_sum + sum(initial_body)) & 0xFFFFFFFF

                                while written_bytes < content_length:
                                    if time.time() - start_time > 45:
                                        print("⚠️ [업로드 타임아웃] 45초 경과로 수신 중단됨")
                                        break
                                    
                                    # 소켓 연결 끊김 확인
                                    sock_st = w5500.get_socket_status(sn=0)
                                    if sock_st != W5500.SOCK_ESTABLISHED and sock_st != W5500.SOCK_CLOSE_WAIT:
                                        avail_check = w5500.rx_bytes_available(sn=0)
                                        if avail_check == 0:
                                            print(f"⚠️ [소켓 해제 감지] Socket 0 Status: 0x{sock_st:02X}")
                                            break

                                    avail = w5500.rx_bytes_available(sn=0)
                                    if avail > 0:
                                        chunk = w5500.read_rx_data(sn=0)
                                        if chunk:
                                            buffer.extend(chunk)
                                            written_bytes += len(chunk)
                                            total_sum = (total_sum + sum(chunk)) & 0xFFFFFFFF
                                            start_time = time.time()

                                            if len(buffer) >= FLASH_WRITE_SIZE:
                                                f.write(buffer)
                                                buffer = bytearray()
                                                gc.collect()
                                    else:
                                        time.sleep_us(100)

                                # 잔여 버퍼 Flush
                                if len(buffer) > 0:
                                    f.write(buffer)
                                    buffer = bytearray()
                                    gc.collect()
                            
                            # 첵섬 계산
                            big_bin_chk = extract_big_bin_checksum(orig_filename, total_sum)
                            sum32_chk = f"{(total_sum >> 16) & 0xFFFF:04X} {total_sum & 0xFFFF:04X} (0x{total_sum:08X})"

                            # 메타데이터 저장
                            info_meta = {
                                "filename": orig_filename,
                                "size_bytes": written_bytes,
                                "big_bin_checksum": big_bin_chk,
                                "sum32_checksum": sum32_chk
                            }
                            try:
                                with open("firmware_info.json", "w") as f_meta:
                                    json.dump(info_meta, f_meta)
                            except Exception:
                                pass

                            print(f"🎉 [업로드 성공] {target_filename} ({written_bytes:,} bytes) 저장 완료!")
                            print(f"   ├─► Big Bin Checksum: {big_bin_chk}")
                            print(f"   └─► 32-bit Sum Checksum: {sum32_chk}")

                            resp_msg = f"✅ 펌웨어 업로드 성공!\n- 파일명: {orig_filename}\n- Big Bin Checksum: {big_bin_chk}\n- 32-bit Byte Sum: {sum32_chk}\n- 크기: {written_bytes:,} Bytes"
                            header = f"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {len(resp_msg.encode('utf-8'))}\r\nConnection: close\r\n\r\n"
                            w5500.send_tx_data(header.encode('utf-8') + resp_msg.encode('utf-8'), sn=0)

                        except Exception as upload_err:
                            print(f"❌ [업로드 오류] {upload_err}")
                            resp_msg = f"업로드 에러: {upload_err}"
                            header = f"HTTP/1.1 500 Internal Error\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {len(resp_msg.encode('utf-8'))}\r\nConnection: close\r\n\r\n"
                            w5500.send_tx_data(header.encode('utf-8') + resp_msg.encode('utf-8'), sn=0)

                        w5500.disconnect_socket(sn=0)

                    else:
                        # 커맨드 전송 API 라우팅 처리 (/api/command)
                        if "/api/command" in first_line:
                            cmd_param = ""
                            if "?cmd=" in first_line or "&cmd=" in first_line:
                                try:
                                    cmd_param = first_line.split("cmd=")[1].split(" ")[0].split("&")[0]
                                    cmd_param = url_unquote(cmd_param)[:100]  # 최대 100자 제한
                                except Exception:
                                    pass
                            
                            json_body = json.dumps({
                                "status": "OK",
                                "command": cmd_param if cmd_param else "NONE",
                                "length": len(cmd_param),
                                "cpu_temp": round(read_cpu_temp(), 1),
                                "led_state": led.value()
                            })
                            header = f"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {len(json_body.encode('utf-8'))}\r\nConnection: close\r\n\r\n"
                            w5500.send_tx_data(header.encode('utf-8') + json_body.encode('utf-8'), sn=0)
                            w5500.disconnect_socket(sn=0)
                            continue

                        # 라우팅 처리
                        if "/led/on" in first_line:
                            led.value(1)
                        elif "/led/off" in first_line:
                            led.value(0)

                        # HTTP 응답 헤더 및 바디 작성
                        body = build_html_response()
                        header = f"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {len(body.encode('utf-8'))}\r\nConnection: close\r\n\r\n"
                        
                        w5500.send_tx_data(header.encode('utf-8') + body.encode('utf-8'), sn=0)
                        w5500.disconnect_socket(sn=0)
                    
                time.sleep_ms(1)
                w5500.listen_server(config.HTTP_PORT)
                
            elif status == W5500.SOCK_CLOSE_WAIT:
                w5500.disconnect_socket(sn=0)
                w5500.listen_server(config.HTTP_PORT)
                
            elif status == W5500.SOCK_CLOSED:
                w5500.listen_server(config.HTTP_PORT)

            # 2. Socket 1: SNMP Agent (UDP Port 161)
            udp_status = w5500.get_socket_status(sn=1)
            if udp_status == W5500.SOCK_UDP:
                remote_ip, remote_port, payload = w5500.recv_udp_packet(sn=1)
                if payload:
                    print(f"📡 [SNMP Request] From {remote_ip}:{remote_port} ({len(payload)} bytes)")
                    response = snmp_agent.process_snmp_request(payload)
                    if response:
                        w5500.send_udp_packet(remote_ip, remote_port, response, sn=1)
                        print(f"   └─► Sent Response ({len(response)} bytes) to {remote_ip}:{remote_port}")
            else:
                w5500.open_udp_socket(sn=1, port=161)

        except KeyboardInterrupt:
            print("\n🛑 서버 수동 종료 (KeyboardInterrupt)")
            raise
        except Exception as e:
            print("⚠️ 루프 처리 중 예외 발생:", e)
            try:
                w5500.disconnect_socket(sn=0)
                w5500.listen_server(config.HTTP_PORT)
            except Exception:
                pass

        # USB CDC 및 고속 응답을 위한 최소 딜레이 (1ms)
        time.sleep_ms(1)


if __name__ == "__main__":
    try:
        run_server()
    except KeyboardInterrupt:
        print("\n🛑 서버 종료 완료")

