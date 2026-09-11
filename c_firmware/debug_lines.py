with open('http_server.c', 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

for i, l in enumerate(lines):
    if "⚡ Realtek FW 전송 진행:" in l:
        print(f"Found at {i+1}")
        for j in range(i, min(i+30, len(lines))):
            print(f"{j+1}: {repr(lines[j])}")
        break
