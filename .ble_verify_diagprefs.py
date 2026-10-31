"""
BLE 真机验证滑杆参数（set_diag_prefs / reset_diag_prefs）：
  1) 扫描并连接 JJTP-XIAOJING
  2) get_status -> 读 config.benchtst 默认值
  3) set_diag_prefs bl50_pwm=60 -> ack DIAG_PREFS_SET
  4) get_status -> 确认 bl50_pwm=60
  5) reset_diag_prefs -> 恢复 40/80/1500 并确认
用法: python .ble_verify_diagprefs.py
"""
import asyncio, json, sys, time
from bleak import BleakScanner, BleakClient

SVC = '0000fff0-0000-1000-8000-00805f9b34fb'
RX  = '0000fff1-0000-1000-8000-00805f9b34fb'
TX  = '0000fff2-0000-1000-8000-00805f9b34fb'
NAME = 'JJTP-XIAOJING'

class Device:
    def __init__(self, client):
        self.client = client
        self.pending = {}
        self.loop = asyncio.get_event_loop()
        self.rx_buf = b''
        self.status_msgs = []   # 收到的 type=="status" 快照

    def on_notify(self, _h, data: bytearray):
        self.rx_buf += bytes(data)
        # 简易帧切分：按完整 JSON 对象解析（本协议一帧一 JSON）
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
        """等待一帧 type=="status" 快照（丢弃历史缓存）。"""
        before = len(self.status_msgs)
        loop = asyncio.get_event_loop()
        for _ in range(int(timeout / 0.1)):
            await asyncio.sleep(0.1)
            if len(self.status_msgs) > before:
                return self.status_msgs[-1]
        return None

def find_benchtst(obj):
    """递归找 benchtst 对象。"""
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

async def read_benchtst(devobj, seq, label):
    """发送 get_status，然后等一帧 status 快照取 config.benchtst。"""
    seq[0] += 1
    r = await devobj.cmd(seq[0], cmd='get_status')
    print(f'{label} get_status -> type={r.get("type")} code={r.get("code")}', flush=True)
    if r.get('type') != 'ack' or r.get('code') != 'OK':
        print(f'{label} !! get_status 未成功: {r}', flush=True)
        return None
    st = await devobj.wait_status(timeout=5.0)
    if st is None:
        print(f'{label} !! 未等到 status 快照', flush=True)
        return None
    bt = find_benchtst(st)
    if bt is None:
        print(f'{label} !! status 中未找到 config.benchtst，打印 status 结构：', flush=True)
        def walk(o, prefix=''):
            if isinstance(o, dict):
                for k, v in o.items():
                    if isinstance(v, (dict, list)):
                        print(f'    {prefix}{k}:')
                        walk(v, prefix + '  ')
                    else:
                        print(f'    {prefix}{k} = {v}')
        walk(st)
        return None
    print(f'{label} benchtst = {bt}', flush=True)
    return bt

async def main():
    print('[ble] 扫描中（最多 30s）...', flush=True)
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

    async with BleakClient(dev.address, timeout=15) as client:
        devobj = Device(client)
        await client.start_notify(TX, devobj.on_notify)
        seq = [100]

        bt = await read_benchtst(devobj, seq, '[1]')
        if bt is None:
            sys.exit(1)
        assert int(bt.get('bl50_pwm', 0)) == 40, '默认 bl50_pwm 应=40'
        assert int(bt.get('ibt2_pwm', 0)) == 80, '默认 ibt2_pwm 应=80'
        assert int(bt.get('act_duration_ms', 0)) == 1500, '默认 act_duration_ms 应=1500'

        seq[0] += 1
        r = await devobj.cmd(seq[0], cmd='set_diag_prefs', bl50_pwm=60, ibt2_pwm=90)
        print(f'[2] set_diag_prefs(60,90) -> type={r.get("type")} code={r.get("code")} ok={r.get("ok")}', flush=True)
        assert r.get('code') == 'DIAG_PREFS_SET', 'set_diag_prefs 应返回 DIAG_PREFS_SET'

        bt = await read_benchtst(devobj, seq, '[3]')
        assert bt is not None, '回读失败'
        assert int(bt.get('bl50_pwm', 0)) == 60, '回读 bl50_pwm 应=60'
        assert int(bt.get('ibt2_pwm', 0)) == 90, '回读 ibt2_pwm 应=90'
        print('[3] 参数已生效（内存 + NVS 异步持久化）', flush=True)

        seq[0] += 1
        r = await devobj.cmd(seq[0], cmd='reset_diag_prefs')
        print(f'[4] reset_diag_prefs -> type={r.get("type")} code={r.get("code")} ok={r.get("ok")}', flush=True)
        assert r.get('code') == 'DIAG_PREFS_RESET', 'reset 应返回 DIAG_PREFS_RESET'

        bt = await read_benchtst(devobj, seq, '[5]')
        assert bt is not None, '恢复后回读失败'
        assert int(bt.get('bl50_pwm', 0)) == 40, '恢复后 bl50_pwm 应=40'
        assert int(bt.get('act_duration_ms', 0)) == 1500, '恢复后 act_duration_ms 应=1500'

        print('\n[RESULT] 滑杆参数固件侧闭环验证 PASS ✅', flush=True)

asyncio.run(main())
