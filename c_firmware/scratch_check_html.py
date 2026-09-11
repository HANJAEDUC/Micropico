with open('c_firmware/curl_page.html', 'r', encoding='utf-8', errors='ignore') as f:
    html = f.read()

idx = html.find('id="led_ctrl-section"')
if idx != -1:
    content = html[idx-50:idx+600]
    import sys
    sys.stdout.buffer.write(content.encode('utf-8'))
    sys.stdout.buffer.write(b'\n')
else:
    print("Not found")
