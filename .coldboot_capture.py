"""
冷启动捕获：复位 COM8 -> 捕获完整启动日志 -> 过滤关键行并做 safe-OFF 摘要检查。
用法: python .coldboot_capture.py <out.log> [duration_s]
"""
import sys, time, serial

PORT = 'COM8'
BAUD = 115200
OUT = sys.argv[1] if len(sys.argv) > 1 else 'build_diagprefs_coldboot.log'
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 40

KEY = ['POWERON', 'MCP', 'machine_config', 'config_persist', 'calibrated',
       'ibt2', 'water', 'dry', 'cha', 'benchtst', 'diag_prefs', 'schema',
       'fault', 'FAULT', 'panic', 'assert', 'abort', 'Guru', 'exception',
       'Rebooting', 'Error', 'display', 'bootstrap', 'heartbeat', 'idle',
       'synced', 'OFF']

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
except Exception as e:
    print("open failed:", e); sys.exit(1)

# flush 旧数据
ser.reset_input_buffer()

print(f"[cb] {PORT} @ {BAUD}, 复位中...", flush=True)
# CH340 复位序列: EN(RTS) 低 -> GPIO0(DTR) 保持高(正常启动) -> 释放 EN
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)
time.sleep(0.1)

print(f"[cb] 捕获 {DURATION}s 冷启动日志 -> {OUT}", flush=True)
start = time.time()
buf = []
keylines = []
while time.time() - start < DURATION:
    raw = ser.read(8192)
    if raw:
        txt = raw.decode('utf-8', errors='replace')
        buf.append(txt)
        for ln in txt.splitlines():
            lnl = ln.lower()
            if any(k.lower() in lnl for k in KEY):
                keylines.append(f"[{time.time()-start:7.1f}s] {ln}")
ser.close()

full = "".join(buf)
with open(OUT, 'w', encoding='utf-8') as f:
    f.write(full)

print(f"\n=== 关键行 ({len(keylines)}) ===", flush=True)
for k in keylines:
    print(k, flush=True)
print(f"\n=== 完整日志已存 {OUT} ({len(full)} chars) ===", flush=True)
