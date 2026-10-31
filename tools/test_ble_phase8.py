#!/usr/bin/env python3
"""Real BLE central smoke test for the Phase 8 app protocol."""

import argparse
import asyncio
import json
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner


DEVICE_NAME = "JJTP-XIAOJING"
SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
RX_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
TX_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"


class JsonFrames:
    def __init__(self):
        self.queue = asyncio.Queue()
        self.buf = bytearray()
        self.depth = 0
        self.in_string = False
        self.escaped = False

    def feed(self, _sender, data):
        for byte in data:
            ch = chr(byte)
            if not self.buf:
                if ch.isspace():
                    continue
                if ch != "{":
                    raise RuntimeError("notification did not start with JSON object")
                self.depth = 1
                self.buf.append(byte)
                continue
            self.buf.append(byte)
            if self.in_string:
                if self.escaped:
                    self.escaped = False
                elif ch == "\\":
                    self.escaped = True
                elif ch == '"':
                    self.in_string = False
            elif ch == '"':
                self.in_string = True
            elif ch in "[{":
                self.depth += 1
            elif ch in "]}":
                self.depth -= 1
                if self.depth == 0:
                    raw = bytes(self.buf)
                    self.buf.clear()
                    self.queue.put_nowait(json.loads(raw.decode("utf-8")))

    async def collect(self, count, timeout=8.0):
        result = []
        deadline = time.monotonic() + timeout
        while len(result) < count:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError(f"received {len(result)}/{count} JSON notifications")
            result.append(await asyncio.wait_for(self.queue.get(), remain))
        return result


def require(condition, message):
    if not condition:
        raise AssertionError(message)


async def run(args):
    found = await BleakScanner.discover(timeout=args.scan_timeout, return_adv=True)
    target = None
    advertisement = None
    for device, adv in found.values():
        if (adv.local_name or device.name or "") == DEVICE_NAME:
            target, advertisement = device, adv
            break
    require(target is not None, f"{DEVICE_NAME} advertisement not found")
    require(SERVICE_UUID in [str(u).lower() for u in advertisement.service_uuids],
            "FFF0 missing from advertisement")

    evidence = {
        "device_name": DEVICE_NAME,
        "address": target.address,
        "rssi": advertisement.rssi,
        "advertised_service": SERVICE_UUID,
    }
    frames = JsonFrames()
    async with BleakClient(target, timeout=15.0) as client:
        require(client.is_connected, "GATT connection failed")
        services = client.services
        service = services.get_service(SERVICE_UUID)
        require(service is not None, "FFF0 service not discovered")
        rx = services.get_characteristic(RX_UUID)
        tx = services.get_characteristic(TX_UUID)
        require(rx is not None and tx is not None, "FFF1/FFF2 missing")
        require("write" in rx.properties, "FFF1 missing write property")
        require("write-without-response" in rx.properties,
                "FFF1 missing write-without-response property")
        require("read" in tx.properties and "notify" in tx.properties,
                "FFF2 missing read/notify properties")
        evidence["rx_properties"] = sorted(rx.properties)
        evidence["tx_properties"] = sorted(tx.properties)

        await client.start_notify(tx, frames.feed)

        hello = b'{"v":1,"seq":1,"type":"cmd","cmd":"hello"}'
        await client.write_gatt_char(rx, hello, response=False)
        first = await frames.collect(3)
        require(first[0].get("type") == "ack" and first[0].get("seq") == 1,
                "hello ACK missing")
        require(first[1].get("type") == "capabilities", "capabilities missing")
        require("submit_plan" in first[1].get("commands", []),
                "capability command list incomplete")
        require(first[2].get("type") == "status" and
                first[2].get("ble_connected") is True,
                "connected status missing")

        await client.write_gatt_char(rx, hello, response=False)
        replay = await frames.collect(3)
        require(replay[0] == first[0], "duplicate seq did not replay cached ACK")
        require(replay[1].get("type") == "capabilities" and
                replay[2].get("type") == "status", "duplicate hello side data missing")

        conflict = b'{"v":1,"seq":1,"type":"cmd","cmd":"get_status"}'
        await client.write_gatt_char(rx, conflict, response=False)
        conflict_reply = (await frames.collect(1))[0]
        require(conflict_reply.get("type") == "nack" and
                conflict_reply.get("code") == "SEQ_CONFLICT",
                "same-seq conflict was not rejected")

        status_cmd = b'{"v":1,"seq":2,"type":"cmd","cmd":"get_status"}'
        await client.write_gatt_char(rx, status_cmd, response=False)
        status_frames = await frames.collect(2)
        require(status_frames[0].get("type") == "ack" and
                status_frames[0].get("seq") == 2, "get_status ACK missing")
        require(status_frames[1].get("type") == "status", "status notify missing")

        readback = json.loads((await client.read_gatt_char(tx)).decode("utf-8"))
        require(readback.get("type") == "status", "FFF2 read-back is not status")
        await client.stop_notify(tx)

        evidence.update({
            "connected": True,
            "hello_frames": first,
            "duplicate_replay": replay[0] == first[0],
            "sequence_conflict": conflict_reply,
            "get_status_frames": status_frames,
            "readback_type": readback.get("type"),
        })

    evidence["disconnected_cleanly"] = True
    if args.output:
        Path(args.output).write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("BLE_PHASE8_REAL_GATT: PASS")
    print("DEVICE:", evidence["address"], "RSSI:", evidence["rssi"])
    print("GATT: FFF0 + FFF1(write/write-no-rsp) + FFF2(read/notify)")
    print("PROTOCOL: hello/status/replay/conflict/read-back PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scan-timeout", type=float, default=15.0)
    parser.add_argument("--output")
    args = parser.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
