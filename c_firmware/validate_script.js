
let currentUptime = 123;
function updateClock(){
  const d = new Date();
  const m = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  const el = document.getElementById('liveClock');
  if(el){
    el.innerText = m[d.getMonth()] + ' ' + String(d.getDate()).padStart(2,'0') + ' \'' + String(d.getFullYear()).slice(-2) + ' ' + d.toTimeString().split(' ')[0];
  }
}
function formatUptime(sec){
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return '⏱ Uptime: ' + String(d).padStart(2,'0') + 'd ' + String(h).padStart(2,'0') + 'h ' + String(m).padStart(2,'0') + 'm ' + String(s).padStart(2,'0') + 's';
}
function tickUptime(){
  currentUptime++;
  const el = document.getElementById('liveUptime');
  if(el){ el.innerText = formatUptime(currentUptime); }
  const dpms = document.getElementById('onTimeDpms');
  if(dpms){
    const h = Math.floor(currentUptime / 3600);
    const m = Math.floor((currentUptime % 3600) / 60);
    const s = currentUptime % 60;
    dpms.innerText = String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0') + ' (00:00)';
  }
}
function fetchStatus(){
  fetch('/api/status?t=' + Date.now()).then(r => r.json()).then(d => {
    if(d && d.uptime_sec !== undefined && d.uptime_sec > 0){
      currentUptime = d.uptime_sec;
    }
  }).catch(e => {});
}
function toggleLed(state){
  fetch('/api/led?state=' + state).then(r => r.json()).then(d => {
    const val = document.getElementById('ledStateVal');
    if(val){
      val.innerHTML = d.led_state ? 'ON 🟢' : 'OFF 🔴';
      val.style.color = d.led_state ? '#4ade80' : '#ef4444';
    }
  }).catch(e => {});
}
function startTimers(){
  updateClock();
  tickUptime();
  setInterval(updateClock, 1000);
  setInterval(tickUptime, 1000);
  setInterval(fetchStatus, 3000);
  const inp = document.getElementById('ethCmdInput');
  if(inp){
    inp.addEventListener('input', updateCharCount);
    inp.addEventListener('keyup', updateCharCount);
    inp.addEventListener('change', updateCharCount);
  }
}
function switchTab(name, btn){
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  if(btn){
    btn.classList.add('active');
  } else {
    document.querySelectorAll('.nav-btn').forEach(b => {
      if(b.getAttribute('onclick') && b.getAttribute('onclick').includes("'" + name + "'")) b.classList.add('active');
    });
  }
  const allSections = ['overview-section', 'firmware-section', 'rs232-section', 'led_ctrl-section', 'generic-section'];
  allSections.forEach(id => {
    const el = document.getElementById(id);
    if(el) el.style.display = 'none';
  });
  if(name === 'overview'){
    const el = document.getElementById('overview-section');
    if(el) el.style.display = 'block';
  } else if(name === 'firmware'){
    const el = document.getElementById('firmware-section');
    if(el) el.style.display = 'grid';
  } else if(name === 'rs232'){
    const el = document.getElementById('rs232-section');
    if(el) el.style.display = 'block';
  } else if(name === 'led_ctrl'){
    const el = document.getElementById('led_ctrl-section');
    if(el) el.style.display = 'block';
  } else {
    const el = document.getElementById('generic-section');
    const title = document.getElementById('generic-title');
    if(el){
      if(title) title.innerText = name.toUpperCase().replace('_', ' ') + ' Setup';
      el.style.display = 'block';
    }
  }
}
function handleFileSelect(e){
  const file = e.target.files[0];
  const infoDiv = document.getElementById('selected-file-info');
  if(!file){ infoDiv.style.display='none'; return; }
  const fname = file.name;
  let bigBinFromFn = null;
  const match = fname.match(/0[xX]([0-9a-fA-F]{8})/);
  if(match && match[1]){
    const hex = match[1].toUpperCase();
    bigBinFromFn = hex.substring(0,4)+' '+hex.substring(4,8);
  }
  infoDiv.style.display='block';
  infoDiv.innerHTML = '<div style="background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #5bc0be;text-align:left;font-size:11px;"><span style="color:#5bc0be;font-weight:bold;">⏳ 체크섬 계산 중...</span> <span style="color:#cbd5e1;">' + fname + ' (' + (file.size/1024).toFixed(1) + ' KB)</span></div>';
  const reader = new FileReader();
  reader.onload = function(evt){
    const bytes = new Uint8Array(evt.target.result);
    let totalSum = 0;
    for(let i=0; i<bytes.length; i++) totalSum = (totalSum + bytes[i]) >>> 0;
    const h = ((totalSum >>> 16) & 0xFFFF).toString(16).padStart(4,'0').toUpperCase();
    const l = (totalSum & 0xFFFF).toString(16).padStart(4,'0').toUpperCase();
    const sumHex = totalSum.toString(16).padStart(8,'0').toUpperCase();
    const sum32Chk = h + ' ' + l + ' (0x' + sumHex + ')';
    const bigBinDisplay = bigBinFromFn ? bigBinFromFn : (h + ' ' + l);
    infoDiv.innerHTML = '<div style="background:#0b132b;padding:6px 8px;border-radius:6px;border:1px solid #0284c7;text-align:left;font-size:11px;"><div style="color:#cbd5e1;margin-bottom:2px;">📁 <strong>' + fname + '</strong> (' + (file.size/1024).toFixed(1) + ' KB)</div><div style="color:#5bc0be;font-family:monospace;font-size:11px;">🏷 Big Bin: ' + bigBinDisplay + ' | 🔢 Sum32: ' + sum32Chk + '</div></div>';
  };
  reader.readAsArrayBuffer(file);
}
function clearStoredFirmware(){
  if(!confirm('저장된 Realtek 펌웨어 정보를 삭제(초기화)하시겠습니까?')) return;
  fetch('/api/clear_fw').then(r => r.json()).then(d => {
    const el = document.getElementById('stored-fw-container');
    if(el){
      el.innerHTML = '<div style="color:#94a3b8; font-size:12px; background:#0b132b; padding:8px; border-radius:6px; border:1px solid #3a506b;">저장된 펌웨어 없음</div>';
    }
  }).catch(e => alert('초기화 실패'));
}
async function uploadFirmware(){
  const file = document.getElementById('fwFile').files[0];
  if(!file) return alert('업로드할 Realtek 펌웨어 파일(.bin)을 선택해주세요!');
  const btn = document.getElementById('uploadBtn');
  const statusMsg = document.getElementById('status-msg');
  const progressContainer = document.getElementById('progress-container');
  const progressBar = document.getElementById('progress-bar');
  const progressText = document.getElementById('progress-text');
  btn.disabled = true; btn.style.opacity = '0.5';
  progressContainer.style.display = 'block';
  progressBar.style.width = '0%';
  progressText.innerText = 'Pico 2 플래시 메모리 섹터 소거 중...';
  statusMsg.style.color = '#5bc0be';
  statusMsg.innerText = '⚡ Pico 2 온보드 플래시 메모리 소거 및 전송 준비 중...';
  async function sendWithRetry(url, opt, retries=3){
    for(let i=0; i<retries; i++){
      try {
        const r = await fetch(url, opt);
        if(r.ok) return r;
      } catch(e){}
      await new Promise(res => setTimeout(res, 30));
    }
    throw new Error('네트워크 전송 실패');
  }
  try {
    const startRes = await sendWithRetry('/upload_start?name=' + encodeURIComponent(file.name) + '&size=' + file.size, { method: 'POST' });
    const CHUNK_SIZE = 65536;
    let offset = 0;
    while(offset < file.size){
      const end = Math.min(offset + CHUNK_SIZE, file.size);
      const chunk = file.slice(offset, end);
      const pct = Math.round((offset / file.size) * 100);
      progressBar.style.width = pct + '%';
      progressText.innerText = pct + '% (' + (offset / 1024).toFixed(0) + ' KB / ' + (file.size / 1024).toFixed(0) + ' KB)';
      statusMsg.innerText = '⚡ 64KB 고속 청크 전송 중 (' + pct + '%)...';
      await sendWithRetry('/upload_chunk?offset=' + offset + '&size=' + (end - offset), { method: 'POST', body: chunk });
      offset = end;
      await new Promise(res => setTimeout(res, 10));
    }
    progressBar.style.width = '100%';
    progressText.innerText = '100% (전송 완료)';
    statusMsg.innerText = '🔢 펌웨어 체크섬 검증 및 플래시 메타데이터 영구 저장 중...';
    const finRes = await sendWithRetry('/upload_finish?name=' + encodeURIComponent(file.name) + '&size=' + file.size, { method: 'POST' });
    const d = await finRes.json();
    btn.disabled = false; btn.style.opacity = '1';
    statusMsg.style.color = '#4ade80';
    statusMsg.innerText = '⚡ Realtek 펌웨어(' + d.filename + ') 100% 무결성 저장 성공!';
    const el = document.getElementById('stored-fw-container');
    if(el){
      el.innerHTML = '<div style="text-align: left; background: #0b132b; padding: 8px; border-radius: 6px; border: 1px solid #3a506b; font-size: 12px;">' +
        '<div style="margin-bottom: 4px;"><strong>📁 저장된 파일명:</strong> <span style="color: #cbd5e1; word-break: break-all;">' + d.filename + '</span></div>' +
        '<div style="margin-bottom: 4px;"><strong>📊 저장된 용량:</strong> <span style="color: #4ade80;">' + d.size.toLocaleString() + ' Bytes (' + d.size_kb + ' KB)</span></div>' +
        '<div style="margin-bottom: 4px;"><strong>🏷 Big Bin Checksum (RTDTool v3.1.3):</strong> <span style="color: #5bc0be; font-weight: bold; font-family: monospace;">' + d.big_bin + '</span></div>' +
        '<div style="margin-bottom: 6px;"><strong>🔢 32-bit Byte Sum Checksum (RTDTool v3.8):</strong> <span style="color: #facc15; font-weight: bold; font-family: monospace;">' + d.sum32 + '</span></div>' +
        '<div style="display:flex; gap:6px; margin-top:8px; flex-wrap:wrap;">' +
        '<button onclick="pingRtdScaler(); return false;" class="btn" style="background:#0284c7; padding:6px 10px; font-size:11px; font-weight:bold;">📡 I2C 통신 확인</button>' +
        '<button onclick="flashRtdScaler(); return false;" id="rtdFlashBtn" class="btn" style="background:#22c55e; padding:6px 12px; font-size:11px; font-weight:bold;">⚡ 스케일러 ISP 플래시 시작</button>' +
        '<button onclick="clearStoredFirmware(); return false;" style="background:#ef4444; color:#fff; border:none; padding:6px 8px; border-radius:6px; font-size:11px; font-weight:bold; cursor:pointer;">🗑 초기화</button>' +
        '</div>' +
        '<div id="rtd-isp-progress-container" style="display:none; margin-top:8px; background:#0b132b; border-radius:6px; border:1px solid #3a506b; height:16px; position:relative; overflow:hidden;"><div id="rtd-isp-progress-bar" style="width:0%; height:100%; background:linear-gradient(90deg,#22c55e,#5bc0be); transition: width 0.2s;"></div><div id="rtd-isp-progress-text" style="position:absolute; width:100%; top:0; left:0; line-height:16px; font-size:10px; font-weight:bold; color:#fff; text-align:center;">0%</div></div>' +
        '<div id="rtd-isp-status-msg" style="margin-top:6px; font-size:12px; min-height:16px; white-space:pre-wrap; text-align:left; color:#94a3b8;"></div>' +
        '</div>';
    }
  } catch(err){
    btn.disabled = false; btn.style.opacity = '1';
    statusMsg.style.color = '#ef4444';
    statusMsg.innerText = '❌ 전송 오류: ' + err.message;
  }
}
function pingRtdScaler(){
  const msg = document.getElementById('rtd-isp-status-msg');
  if(msg){ msg.style.color = '#5bc0be'; msg.innerText = '📡 Channel 2 (GP2/GP3) Realtek 스케일러(0x4A) I2C 통신 확인 중...'; }
  fetch('/api/rtd/ping')
  .then(r => r.json())
  .then(d => {
    if(d.connected){
      if(msg){
        msg.innerHTML = '<div style="background:#0b132b; padding:8px 12px; border-radius:6px; border:1px solid #22c55e; font-size:12px; color:#cbd5e1;">' +
          '<div style="color:#4ade80; font-weight:bold; margin-bottom:4px;">' + d.message + '</div>' +
          '<div>• <strong>I2C 연결:</strong> <span style="color:#a5f3fc;">Channel 2 (GP2:SCL / GP3:SDA @ 주소 0x4A)</span></div>' +
          '<div>• <strong>상태:</strong> <span style="color:#22c55e; font-weight:bold;">정상 응답 (ACK 수신 완료)</span></div>' +
          '<button onclick="readRtdFlashInfo(); return false;" class="btn" style="margin-top:6px; background:#0284c7; padding:4px 10px; font-size:11px; font-weight:bold;">🔍 온보드 Flash 칩셋 JEDEC ID 상세 조회</button>' +
          '</div>';
      }
    } else {
      if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ Realtek 스케일러 응답 없음 (I2C 0x4A NACK - GP2/GP3 배선 및 스케일러 전원 확인 필요)'; }
    }
  }).catch(e => {
    if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ I2C 통신 오류 발생'; }
  });
}
function readRtdFlashInfo(){
  const msg = document.getElementById('rtd-isp-status-msg');
  if(msg){ msg.style.color = '#5bc0be'; msg.innerText = '🔍 온보드 Flash 칩셋 JEDEC ID (0x9F) 조회 중...'; }
  fetch('/api/rtd/info')
  .then(r => r.json())
  .then(d => {
    if(d.connected && msg){
      let detail = '<div style="background:#0b132b; padding:8px 12px; border-radius:6px; border:1px solid #0284c7; font-size:12px; color:#cbd5e1;">' +
        '<div style="color:#5bc0be; font-weight:bold; margin-bottom:4px;">📡 Realtek 스케일러 온보드 Flash 감지 정보</div>' +
        '<div>• <strong>Flash 제조사:</strong> <span style="color:#facc15; font-weight:bold;">' + (d.mfg_name || 'N/A') + ' (ID: ' + d.mfg_id + ')</span></div>' +
        '<div>• <strong>Flash 칩 모델:</strong> <span style="color:#4ade80; font-weight:bold;">' + (d.flash_model || 'Generic SPI Flash') + '</span></div>' +
        '<div>• <strong>Flash 용량:</strong> <span style="color:#6fffe9; font-weight:bold;">' + (d.flash_size || 'N/A') + '</span></div>' +
        '<div>• <strong>JEDEC ID:</strong> <span style="color:#cbd5e1; font-family:monospace;">' + d.mfg_id + ' ' + d.mem_type + ' ' + d.cap_id + '</span></div>' +
        '</div>';
      msg.innerHTML = detail;
    }
  }).catch(e => {
    if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ Flash 정보 조회 오류'; }
  });
}
function flashRtdScaler(){
  if(!confirm('Pico 2에 저장된 Realtek 펌웨어를 Channel 2 (GP2:SCL, GP3:SDA)를 통해 실제 Realtek 스케일러 칩에 플래싱하시겠습니까?')) return;
  const btn = document.getElementById('rtdFlashBtn');
  const msg = document.getElementById('rtd-isp-status-msg');
  const progContainer = document.getElementById('rtd-isp-progress-container');
  const progBar = document.getElementById('rtd-isp-progress-bar');
  const progText = document.getElementById('rtd-isp-progress-text');
  if(btn){ btn.disabled = true; btn.style.opacity = '0.5'; }
  if(progContainer){ progContainer.style.display = 'block'; }
  if(progBar){ progBar.style.width = '0%'; }
  if(progText){ progText.innerText = '0% (ISP 진입 및 64KB 블록 소거 중...)'; }
  if(msg){ msg.style.color = '#facc15'; msg.innerText = '⚡ Realtek 스케일러 ISP 플래시 시작 중...'; }
  let startTime = Date.now();
  const totalEstimatedSec = 16.0;
  const progTimer = setInterval(() => {
    const elapsed = (Date.now() - startTime) / 1000;
    let pct = Math.min(95, Math.round((elapsed / totalEstimatedSec) * 95));
    if(pct < 10) {
      if(progText) progText.innerText = pct + '% (64KB 블록 소거 중...)';
      if(msg) msg.innerText = '⚡ 64KB 블록 소거 진행 중 (' + pct + '%)...';
    } else {
      const estKB = Math.min(768, Math.round((pct / 95) * 768));
      if(progText) progText.innerText = pct + '% (' + estKB + ' KB / 768 KB)';
      if(msg) msg.innerText = '⚡ I2C 64B 고속 버스트 플래시 중 (' + pct + '%)...';
    }
    if(progBar) progBar.style.width = pct + '%';
  }, 200);
  fetch('/api/rtd/flash', { method: 'POST' })
  .then(r => r.json())
  .then(d => {
    clearInterval(progTimer);
    if(btn){ btn.disabled = false; btn.style.opacity = '1'; }
    if(d.success){
      if(progBar){ progBar.style.width = '100%'; }
      if(progText){ progText.innerText = '100% (플래시 완료)'; }
      if(msg){ msg.style.color = '#4ade80'; msg.innerText = '🎉 ' + d.message; }
    } else {
      if(msg){ msg.style.color = '#ef4444'; msg.innerText = d.message; }
    }
  }).catch(e => {
    clearInterval(progTimer);
    if(btn){ btn.disabled = false; btn.style.opacity = '1'; }
    if(msg){ msg.style.color = '#ef4444'; msg.innerText = '❌ 플래시 통신 오류 발생!'; }
  });
}
function uploadPicoOta(){
  const file = document.getElementById('picoOtaFile').files[0];
  if(!file) return alert('Pico 2 펌웨어(.bin/.uf2) 파일을 선택해주세요!');
  const reader = new FileReader();
  reader.onload = function(e){
    const buf = e.target.result;
    const u8 = new Uint8Array(buf);
    const magic_uf2 = (u8[0] | (u8[1] << 8) | (u8[2] << 16) | (u8[3] << 24)) >>> 0;
    const initial_sp = magic_uf2;
    const is_uf2 = (magic_uf2 === 0x0A324655);
    const is_rp2350_bin = (initial_sp === 0x4D535052 || initial_sp === 0xFFFFEDAC || (initial_sp >= 0x20000000 && initial_sp <= 0x200B0000));
    const fname = file.name.toLowerCase();
    const is_realtek = fname.includes('dh9') || fname.includes('rlt') || fname.includes('dlc') || (u8[0] === 0x02 || u8[0] === 0x12);
    if(!is_uf2 && !is_rp2350_bin && is_realtek){
      alert('⚠️ [안전 차단] 선택하신 파일(' + file.name + ')은 Realtek 스케일러 펌웨어입니다!\n\nPico 2 OTA 카드에는 Pico 2 전용 펌웨어(w5500_pico2_firmware.bin/.uf2)만 업로드할 수 있습니다.');
      return;
    }
    if(!confirm('Pico 2 보드를 이더넷 OTA 원격 펌웨어로 업데이트하고 자동 재부팅하시겠습니까?')) return;
    const btn = document.getElementById('picoOtaBtn');
    const statusMsg = document.getElementById('pico-ota-status-msg');
    const progressContainer = document.getElementById('pico-ota-progress-container');
    const progressBar = document.getElementById('pico-ota-progress-bar');
    const progressText = document.getElementById('pico-ota-progress-text');
    btn.disabled = true; btn.style.opacity = '0.5';
    progressContainer.style.display = 'block';
    statusMsg.style.color = '#0284c7';
    statusMsg.innerText = '🚀 Pico 2 이더넷 OTA 펌웨어 전송 중...';
    let uploadDone = false;
    function showSuccess(msg){
      statusMsg.style.color = '#4ade80';
      let cnt = 4;
      statusMsg.innerText = (msg || '🚀 Pico 2 이더넷 OTA 펌웨어 업로드 성공!') + '\n\n⚡ ' + cnt + '초 후 새 버전으로 자동 연결됩니다...';
      const timer = setInterval(() => {
        cnt--;
        if(cnt >= 0){
          statusMsg.innerText = (msg || '🚀 Pico 2 이더넷 OTA 펌웨어 업로드 성공!') + '\n\n⚡ ' + cnt + '초 후 새 버전으로 자동 연결됩니다...';
        } else {
          clearInterval(timer);
          location.reload();
        }
      }, 1000);
    }
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload_pico_fw?name=' + encodeURIComponent(file.name), true);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.timeout = 120000;
    xhr.upload.onprogress = function(e){
      if(e.lengthComputable){
        const pct = Math.round((e.loaded / e.total) * 100);
        progressBar.style.width = pct + '%';
        progressText.innerText = pct + '% (' + (e.loaded / 1024).toFixed(0) + ' KB / ' + (e.total / 1024).toFixed(0) + ' KB)';
        if(pct >= 100) uploadDone = true;
      }
    };
    xhr.onload = function(){
      btn.disabled = false; btn.style.opacity = '1';
      if(xhr.status === 200){
        showSuccess(xhr.responseText);
      } else {
        statusMsg.style.color = '#ef4444';
        statusMsg.innerText = '❌ OTA 업로드 실패: ' + xhr.responseText;
      }
    };
    xhr.onerror = function(){
      if(uploadDone){
        showSuccess('🚀 Pico 2 이더넷 OTA 전송 완료! (재부팅 적용 중...)');
      } else {
        btn.disabled = false; btn.style.opacity = '1';
        statusMsg.style.color = '#ef4444';
        statusMsg.innerText = '❌ 네트워크 오류 발생!';
      }
    };
    xhr.send(buf);
  };
  reader.readAsArrayBuffer(file);
}
function updateCharCount(){
  const input = document.getElementById('ethCmdInput');
  const cnt = document.getElementById('ethCharCounter');
  if(input && cnt){ cnt.innerText = input.value.length; }
}
function clearEthLog(){
  const log = document.getElementById('ethCmdLog');
  if(log){ log.innerHTML = '<div style="color:#94a3b8;">[시스템] RS-232 터미널 로그가 초기화되었습니다.</div>'; }
}
function downloadEthLog(){
  const log = document.getElementById('ethCmdLog');
  if(!log) return;
  const text = log.innerText || log.textContent;
  const blob = new Blob([text], {type: 'text/plain;charset=utf-8'});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  const d = new Date();
  const dateStr = d.getFullYear() + String(d.getMonth()+1).padStart(2,'0') + String(d.getDate()).padStart(2,'0') + '_' + String(d.getHours()).padStart(2,'0') + String(d.getMinutes()).padStart(2,'0') + String(d.getSeconds()).padStart(2,'0');
  a.download = 'rs232_serial_log_' + dateStr + '.txt';
  document.body.appendChild(a); a.click();
  document.body.removeChild(a); URL.revokeObjectURL(url);
}
let cmdHistory = [];
let historyIdx = -1;
function handleCmdKeyDown(e){
  if(e.key === 'Enter'){
    sendEthCommand();
    return false;
  } else if(e.key === 'ArrowUp'){
    if(cmdHistory.length > 0 && historyIdx < cmdHistory.length - 1){
      historyIdx++;
      const input = document.getElementById('ethCmdInput');
      if(input){ input.value = cmdHistory[cmdHistory.length - 1 - historyIdx]; updateCharCount(); }
    }
    e.preventDefault();
  } else if(e.key === 'ArrowDown'){
    if(historyIdx > 0){
      historyIdx--;
      const input = document.getElementById('ethCmdInput');
      if(input){ input.value = cmdHistory[cmdHistory.length - 1 - historyIdx]; updateCharCount(); }
    } else if(historyIdx === 0){
      historyIdx = -1;
      const input = document.getElementById('ethCmdInput');
      if(input){ input.value = ''; updateCharCount(); }
    }
    e.preventDefault();
  }
}
function setPreset(cmd, ending){
  const input = document.getElementById('ethCmdInput');
  if(input){ input.value = cmd; updateCharCount(); }
  if(ending){
    const sel = document.getElementById('ethEndingSelect');
    if(sel) sel.value = ending;
  }
}
function setAndSend(cmd, ending){
  setPreset(cmd, ending);
  sendEthCommand();
}
let isSending = false;
let sendWatchdog = null;
function resetSendBtn(){
  isSending = false;
  if(sendWatchdog){ clearTimeout(sendWatchdog); sendWatchdog = null; }
  const btn = document.getElementById('ethSendBtn');
  const input = document.getElementById('ethCmdInput');
  if(btn){
    btn.style.opacity = '1';
    btn.style.background = '#22c55e';
    btn.style.transform = 'scale(1)';
    btn.innerHTML = '🚀 전송 (Send)';
    btn.disabled = false;
  }
  if(input){ input.style.border = '1px solid #3a506b'; }
}
function sendEthCommand(){
  if(isSending) return;
  isSending = true;
  const input = document.getElementById('ethCmdInput');
  const log = document.getElementById('ethCmdLog');
  const portSelect = document.getElementById('ethUartPortSelect');
  const baudSelect = document.getElementById('ethBaudSelect');
  const endingSelect = document.getElementById('ethEndingSelect');
  const timeoutSelect = document.getElementById('ethTimeoutSelect');
  const btn = document.getElementById('ethSendBtn');
  if(btn){
    btn.style.opacity = '0.85';
    btn.style.background = '#0284c7';
    btn.style.transform = 'scale(0.95)';
    btn.innerHTML = '⏳ 전송 중...';
    btn.disabled = true;
  }
  const cmd = input ? input.value.trim() : '';
  if(!cmd){
    if(btn){ btn.innerHTML = '⚠️ 커맨드 입력 필요!'; btn.style.background = '#eab308'; }
    if(input){ input.focus(); input.style.border = '2px solid #eab308'; }
    setTimeout(resetSendBtn, 800);
    return;
  }
  if(cmdHistory.length === 0 || cmdHistory[cmdHistory.length - 1] !== cmd){
    cmdHistory.push(cmd);
    if(cmdHistory.length > 30) cmdHistory.shift();
  }
  historyIdx = -1;
  const port = portSelect ? portSelect.value : 'uart0';
  const baud = baudSelect ? baudSelect.value : '9600';
  const ending = endingSelect ? endingSelect.value : 'NONE';
  const timeoutVal = timeoutSelect ? parseInt(timeoutSelect.value) : 2000;
  if(input){ input.style.border = '2px solid #5bc0be'; }
  const now = new Date();
  const timeStr = String(now.getHours()).padStart(2,'0') + ':' + String(now.getMinutes()).padStart(2,'0') + ':' + String(now.getSeconds()).padStart(2,'0') + '.' + String(Math.floor(now.getMilliseconds()/100));
  const autoScroll = document.getElementById('ethAutoScroll') ? document.getElementById('ethAutoScroll').checked : true;
  log.innerHTML += '<div>[' + timeStr + '] 📤 <strong style="color:#38bdf8;">TX (' + port.toUpperCase() + ' @ ' + baud + 'bps, ' + ending + '):</strong> ' + cmd + '</div>';
  if(autoScroll) log.scrollTop = log.scrollHeight;
  const t0 = Date.now();
  const controller = new AbortController();
  const abortTimeout = setTimeout(() => controller.abort(), timeoutVal + 2500);
  sendWatchdog = setTimeout(resetSendBtn, timeoutVal + 3000);
  fetch('/api/command?port=' + port + '&cmd=' + encodeURIComponent(cmd) + '&baud=' + baud + '&ending=' + ending + '&timeout=' + timeoutVal, { signal: controller.signal })
  .then(r => r.json())
  .then(d => {
    clearTimeout(abortTimeout);
    const dt = Date.now() - t0;
    const lat = d.latency_ms !== undefined ? d.latency_ms : dt;
    if(d.hex && d.hex.trim().length > 0){
      log.innerHTML += '<div style="color:#4ade80;">[' + timeStr + '] 📥 <strong style="color:#4ade80;">RX (' + lat + 'ms, ' + d.rx_bytes + 'B):</strong> <span style="color:#facc15; font-family:monospace;">HEX:[' + d.hex.trim() + ']</span> <span style="color:#6fffe9;">ASCII:"' + d.ascii + '"</span></div>';
      if(btn){ btn.innerHTML = '✅ 수신 완료 (' + lat + 'ms)'; btn.style.background = '#22c55e'; }
    } else {
      log.innerHTML += '<div style="color:#fbbf24;">[' + timeStr + '] ⚠️ <strong>RX (' + lat + 'ms):</strong> 타겟 응답 없음 (타임아웃 ' + (timeoutVal/1000).toFixed(1) + 's)</div>' +
        '<div style="color:#94a3b8; font-size:10px; padding-left:12px;">확인: 배선 TX/RX 교차 연결, 보레이트(' + baud + 'bps), 종단문자(' + ending + '), 3.3V/RS232 레벨변환기</div>';
      if(btn){ btn.innerHTML = '⚠️ 수신 타임아웃'; btn.style.background = '#eab308'; }
    }
    if(autoScroll) log.scrollTop = log.scrollHeight;
    setTimeout(resetSendBtn, 800);
  }).catch(e => {
    clearTimeout(abortTimeout);
    const dt = Date.now() - t0;
    log.innerHTML += '<div style="color:#ef4444;">[' + timeStr + '] ❌ 통신 타임아웃 또는 전송 오류 (' + dt + 'ms)</div>' +
      '<div style="color:#94a3b8; font-size:10px; padding-left:12px;">확인: W5500 네트워크 상태 또는 하드웨어 연결을 확인해주세요.</div>';
    if(btn){ btn.innerHTML = '❌ 전송 타임아웃'; btn.style.background = '#ef4444'; }
    setTimeout(resetSendBtn, 800);
  });
  updateCharCount();
}
let autoRepeatTimer = null;
function toggleAutoRepeat(){
  const chk = document.getElementById('ethAutoRepeatChk');
  const intSel = document.getElementById('ethIntervalSelect');
  if(chk && chk.checked){
    const ms = intSel ? parseInt(intSel.value) : 2000;
    sendEthCommand();
    autoRepeatTimer = setInterval(sendEthCommand, ms);
  } else {
    if(autoRepeatTimer){ clearInterval(autoRepeatTimer); autoRepeatTimer = null; }
  }
}
function getStoredMacros(){
  try { const m = localStorage.getItem('dh_custom_macros_v2'); return m ? JSON.parse(m) : null; } catch(e){ return null; }
}
function saveMacrosToStorage(macros){
  try { localStorage.setItem('dh_custom_macros_v2', JSON.stringify(macros)); } catch(e){}
}
function renderCustomMacros(){
  const container = document.getElementById('customMacroContainer');
  if(!container) return;
  let macros = getStoredMacros();
  if(!macros || macros.length === 0){
    macros = [
      { name: '버전 확인', cmd: 'GETVERSIONX', ending: 'NONE' },
      { name: '전원 상태', cmd: 'GETSTATUS', ending: 'NONE' },
      { name: 'HDMI 1', cmd: 'SOURCE HDMI1', ending: 'NONE' },
      { name: 'DP 입력', cmd: 'SOURCE DP', ending: 'NONE' },
      { name: 'HEX Ping', cmd: '69 00 01 FF', ending: 'NONE' }
    ];
    saveMacrosToStorage(macros);
  }
  let html = '';
  macros.forEach((m, idx) => {
    html += '<div style="display:inline-flex; align-items:center; background:#1c2541; border:1px solid #3a506b; border-radius:4px; margin:2px;">' +
      '<button onclick="setAndSend(\'' + m.cmd + '\',\'' + (m.ending || 'NONE') + '\');" style="background:none; border:none; color:#6fffe9; padding:4px 8px; font-size:11px; font-weight:bold; cursor:pointer;">' + m.name + '</button>' +
      '<span onclick="deleteCustomMacro(' + idx + ');" style="color:#ef4444; padding:2px 6px; cursor:pointer; font-size:10px; border-left:1px solid #3a506b;" title="삭제">✕</span>' +
      '</div>';
  });
  container.innerHTML = html;
}
function addCustomMacro(){
  const name = prompt('새 매크로 버튼 이름을 입력하세요 (예: 밝기 100):');
  if(!name) return;
  const cmd = prompt('전송할 커맨드를 입력하세요 (예: BRIGHTNESS 100 또는 69 00 02 FF):');
  if(!cmd) return;
  let macros = getStoredMacros() || [];
  macros.push({ name: name, cmd: cmd, ending: 'NONE' });
  saveMacrosToStorage(macros);
  renderCustomMacros();
}
function deleteCustomMacro(idx){
  let macros = getStoredMacros() || [];
  if(idx >= 0 && idx < macros.length){
    macros.splice(idx, 1);
    saveMacrosToStorage(macros);
    renderCustomMacros();
  }
}
