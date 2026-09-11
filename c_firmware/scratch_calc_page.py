with open('c_firmware/curl_page.html', 'r', encoding='utf-8', errors='ignore') as f:
    html = f.read()

print(f"curl_page.html length: {len(html)} bytes")
