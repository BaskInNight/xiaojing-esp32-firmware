#!/usr/bin/env python3
"""Board-level UV self-test gate verification over BLE (no lamp light).
Sends hello + uv_self_test to the JJTP device and prints replies.
Expected on bare board (no 0-deg magnet): a reject code, lamp stays OFF.
"""
import asyncio
import json
import sys

from bleak import BleakClient, BleakScanner

SERVICE = '0000FFF0-0000-1000-8000-00805F9B34FB'
WRITE = '0000FFF1-0000-1000-8000-00805F9B34FB'
NOTIFY = '0000FFF2-0000-1000-8000-00805F9B34FB'


class FrameAssembler:
    def __init__(self):
        self.buf = b''

    def push(self, chunk):
        self.buf += bytes(chunk)
        frames = []
        while True:
            start = self.buf.find(b'{')
            if start < 0:
                self.buf = b''
                break
            self.buf = self.buf[start:]
            depth = 0
            in_str = False
            esc = False
            idx = -1
            for i, b in enumerate(self.buf):
                if in_str:
                    if esc:
                        esc = False
                    elif b == 0x5C:
                        esc = True
                    elif b == 0x22:
                        in_str = False
                    continue
                if b == 0x22:
                    in_str = True
                elif b == 0x7B:
                    depth += 1
                elif b == 0x7D:
                    depth -= 1
                    if depth == 0:
                        idx = i
                        break
            if idx < 0:
                break
            raw = self.buf[:idx + 1]
            self.buf = self.buf[idx + 1:]
            try:
                frames.append(json.loads(raw.decode('utf-8')))
            except Exception:
                pass
        return frames


async def main():
    target = None
    print('scanning...')
    devices = await BleakScanner.discover(timeout=8)
    for d in devices:
        print('-', repr(d.name), d.address)
        if (d.name or '').startswith('JJTP'):
            target = d
            print('FOUND', d.name, d.address)
    if target is None:
        # fallback: match by FFF0 service UUID broadcast (device name lives
        # in the scan response, which Windows WinRT may not surface)
        print('matching by FFF0 service uuid...')
        seen = {}

        def cb(device, adv):
            if adv and 'fff0' in str(adv.service_uuids).lower():
                seen[device.address] = device

        scanner = BleakScanner(detection_callback=cb)
        await scanner.start()
        await asyncio.sleep(6)
        await scanner.stop()
        if seen:
            target = list(seen.values())[0]
            print('FOUND_BY_SERVICE', target.address)
    if target is None:
        print('NO_JJTP_DEVICE_FOUND')
        sys.exit(1)

    async with BleakClient(target.address, timeout=20) as client:
        print('CONNECTED')
        frames = []
        assembler = FrameAssembler()

        def on_notify(_char, data):
            for fr in assembler.push(data):
                frames.append(fr)

        await client.start_notify(NOTIFY, on_notify)

        seq = 0
        async def cmd(name, payload=None):
            nonlocal seq
            seq += 1
            msg = {'v': 1, 'type': 'cmd', 'seq': seq, 'cmd': name}
            if payload:
                msg.update(payload)
            await client.write_gatt_char(WRITE, json.dumps(msg).encode())
            await asyncio.sleep(2.0)

        await cmd('hello')
        caps = [f for f in frames if f.get('type') == 'capabilities']
        print('CAPABILITIES', json.dumps(caps[0]) if caps else 'none')
        diag = (caps[0] or {}).get('diagnostics')
        print('DIAGNOSTICS', diag)
        status = [f for f in frames if f.get('type') == 'status']
        if status:
            last = status[-1]
            print('STATUS', 'uv_self_test=', last.get('uv_self_test'),
                  'fault_code=', last.get('fault_code'), 'position=', last.get('position'))

        frames.clear()
        status = [f for f in frames if f.get('type') == 'status']
        if not status:
            await cmd('get_status')
            status = [f for f in frames if f.get('type') == 'status']
        pos = None
        if status:
            pos = status[-1].get('position')
            print('STATUS_PRE', 'position=', pos,
                  'uv_self_test=', status[-1].get('uv_self_test'))

        # SAFETY GUARD: only run the gate test when the reported position is
        # clearly NOT the zero/alignment position (fresh + 0). If position is
        # 0 we must NOT auto-light the UV lamp without user confirmation.
        guard_ok = pos not in (0, None)
        print('GATE_TEST_GUARD', 'pos=', pos, 'proceed=', guard_ok)
        if not guard_ok:
            print('GATE_TEST_SKIPPED_POS_IS_ZERO_OR_UNKNOWN')
            frames.clear()
            await cmd('get_status')
            st = [f for f in frames if f.get('type') == 'status']
            if st:
                print('STATUS_AFTER', 'uv_self_test=', st[-1].get('uv_self_test'),
                      'uv_on=', st[-1].get('uv_on'))
            await client.stop_notify(NOTIFY)
            print('DONE_SAFE_SKIP')
            return

        frames.clear()
        await cmd('uv_self_test')
        acks = [f for f in frames if f.get('type') in ('ack', 'nack')]
        print('UV_SELF_TEST_ACK', json.dumps(acks[-1]) if acks else 'none')

        frames.clear()
        await cmd('get_status')
        st = [f for f in frames if f.get('type') == 'status']
        if st:
            print('STATUS_AFTER', 'uv_self_test=', st[-1].get('uv_self_test'),
                  'uv_on=', st[-1].get('uv_on'))

        await client.stop_notify(NOTIFY)
    print('DONE')


if __name__ == '__main__':
    asyncio.run(main())
