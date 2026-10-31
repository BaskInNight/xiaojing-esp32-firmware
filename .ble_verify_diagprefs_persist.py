"""
滑杆参数 NVS 持久化验证：
  1) BLE set_diag_prefs(bl50_pwm=60, ibt2_pwm=90) -> DIAG_PREFS_SET
  2) 断开 BLE，等待异步 NVS 持久化完成
  3) 串口 COM8 冷复位 -> 等启动完成
  4) BLE 重连 get_status -> 读回 config.benchtst 应为 60/90（证明 NVS 生效）
  5) reset_diag_prefs -> 恢复 40/80/1500
  6) get_status -> 确认恢复
用法: python .ble_verify_diagprefs_persist.py
"""
import asyncio, json, sys, time, serial
from bleak import BleakScanner, BleakClient

SVC = '0000fff0-0000-1000-8000-00805f9b34fb'
RX  = '0000fff1-0000-1000-8000-00805f9b34fb'
TX  = '0000fff2-0000-1000-8000-00805f9b34fb'
NAME = 'JJTP-XIAOJING'
PORT = 'COM8'

class Device:
    def __init__(self, client):
        self.client = client
        self.pending = {}
        self.loop = asyncio.get_event_loop()
        self.rx_buf = b''
        self.status_msgs = []

    def on_notify(self, _h, data: bytearray):
        self.rx_buf += bytes(data)
        txt = self.rx_buf.decode('utf-8', errors='replace')
        depth = 0; started = False; start = 0
        for i, ch in enumerate(txt):
            if ch == '{':
                if not started: started = True; start = i
                depth += 1
            elif ch == '}':
                depth -= 1
                if started and depth == 0:
                    try:
                        msg = json.loads(txt[start:i+1])
                    except Exception:
                        pass
                    else:
                        seq = msg.get('seq')
                        if seq in self.pending:
                            self.pending.pop(seq).set_result(msg)
                        if msg.get('type') == 'status':
                            self.status_msgs.append(msg)
                    started = False
        if started:
            self.rx_buf = txt[start:].encode('utf-8')
        else:
            self.rx_buf = b''

    async def cmd(self, seq, **payload):
        body = {"v": 1, "type": "cmd", "seq": seq}
        body.update(payload)
        fut = self.loop.create_future()
        self.pending[seq] = fut
        raw = json.dumps(body).encode('utf-8')
        await self.client.write_gatt_char(RX, raw, response=True)
        try:
            return await asyncio.wait_for(fut, timeout=5.0)
        except asyncio.TimeoutError:
            return {"type": "nack", "seq": seq, "code": "RX_TIMEOUT"}

    async def wait_status(self, timeout=5.0):
        before = len(self.status_msgs)
        for _ in range(int(timeout / 0.1)):
            await asyncio.sleep(0.1)
            if len(self.status_msgs) > before:
                return self.status_msgs[-1]
        return None

def find_benchtst(obj):
    if isinstance(obj, dict):
        if 'benchtst' in obj and isinstance(obj['benchtst'], dict):
            return obj['benchtst']
        for v in obj.values():
            r = find_benchtst(v)
            if r: return r
    elif isinstance(obj, list):
        for v in obj:
            r = find_benchtst(v)
            if r: return r
    return None

async def scan_and_connect():
    print('[ble] 扫描中...', flush=True)
    dev = None
    async with BleakScanner() as scanner:
        start = time.time()
        while time.time() - start < 30:
            for d in scanner.discovered_devices:
                nm = (d.name or '').strip()
                if nm == NAME or (nm and NAME.lower() in nm.lower()):
                    dev = d; break
                if d.address and d.address.upper().startswith('94:A9:90'):
                    dev = d; break
            if dev: break
            await asyncio.sleep(0.5)
    if not dev:
        print('[ble] 未找到设备 ' + NAME, flush=True)
        sys.exit(1)
    print(f'[ble] 找到 {dev.name} @ {dev.address}，连接中...', flush=True)
    client = BleakClient(dev.address, timeout=15)
    await client.connect()
    d = Device(client)
    await client.start_notify(TX, d.on_notify)
    return client, d

async def read_bt(d, seq, label):
    seq[0] += 1
    r = await d.cmd(seq[0], cmd='get_status')
    print(f'{label} get_status -> {r.get("type")}/{r.get("code")}', flush=True)
    st = await d.wait_status(timeout=5.0)
    if st is None:
        print(f'{label} !! 无 status 快照', flush=True)
        return None
    return find_benchtst(st)

def serial_reset_and_wait(seconds=12):
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.2)
    except Exception as e:
        print('[serial] open failed:', e); sys.exit(1)
    ser.reset_input_buffer()
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.15); ser.setRTS(False)
    print(f'[serial] COM8 复位，等待 {seconds}s 启动...', flush=True)
    time.sleep(seconds)
    ser.close()

async def main():
    # 起始 seq 用大值：避开上/前一次脚本留在固件 seq-cache（最近 8 条，RAM 不随
    # BLE 断开清空）里的 100~105，防止同 seq 不同命令被误判为 SEQ_CONFLICT。
    seq = [900]

    client, d = await scan_and_connect()
    seq[0] += 1
    r = await d.cmd(seq[0], cmd='set_diag_prefs', bl50_pwm=60, ibt2_pwm=90)
    print(f'[1] set_diag_prefs(60,90) -> {r.get("type")}/{r.get("code")}', flush=True)
    assert r.get('code') == 'DIAG_PREFS_SET', 'set 应返回 DIAG_PREFS_SET'
    await client.disconnect()
    print('[1] 已断开，等待异步 NVS 持久化 3s...', flush=True)
    time.sleep(3)

    serial_reset_and_wait(seconds=12)

    client, d = await scan_and_connect()
    seq[0] += 1
    bt = await read_bt(d, seq, '[2]')
    assert bt is not None, '重连后回读失败'
    print(f'[2] 冷启动后 benchtst = {bt}', flush=True)
    assert int(bt.get('bl50_pwm', 0)) == 60, '冷启动后 bl50_pwm 应=60（NVS 持久化）'
    assert int(bt.get('ibt2_pwm', 0)) == 90, '冷启动后 ibt2_pwm 应=90（NVS 持久化）'
    print('[2] ✅ NVS 持久化生效：60/90 跨冷启动保留', flush=True)

    seq[0] += 1
    r = await d.cmd(seq[0], cmd='reset_diag_prefs')
    print(f'[3] reset_diag_prefs -> {r.get("type")}/{r.get("code")}', flush=True)
    assert r.get('code') == 'DIAG_PREFS_RESET', 'reset 应返回 DIAG_PREFS_RESET'
    await client.disconnect()
    print('[3] 等待 NVS 持久化 3s...', flush=True)
    time.sleep(3)

    serial_reset_and_wait(seconds=12)

    client, d = await scan_and_connect()
    seq[0] += 1
    bt = await read_bt(d, seq, '[4]')
    assert bt is not None, '恢复后回读失败'
    print(f'[4] 恢复出厂后 benchtst = {bt}', flush=True)
    assert int(bt.get('bl50_pwm', 0)) == 40, '恢复后 bl50_pwm 应=40'
    assert int(bt.get('ibt2_pwm', 0)) == 80, '恢复后 ibt2_pwm 应=80'
    assert int(bt.get('act_duration_ms', 0)) == 1500, '恢复后 act_duration_ms 应=1500'
    await client.disconnect()

    print('\n[RESULT] 滑杆参数 NVS 持久化闭环验证 PASS ✅', flush=True)

asyncio.run(main())
