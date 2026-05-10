#!/usr/bin/env python3
"""InfiniTime OTA via Nordic Secure DFU.  Requires: pip install bleak"""

import asyncio, struct, zipfile, argparse, sys
from bleak import BleakClient, BleakScanner

UUID_BUTTONLESS = '8e400001-f315-4f60-9fb8-838830daea50'
UUID_CTRL       = '8ec90001-f315-4f60-9fb8-838830daea50'
UUID_DATA       = '8ec90002-f315-4f60-9fb8-838830daea50'

PRN      = 10   # packet receipt notification interval
PKT_SIZE = 20   # bytes per BLE packet

OP_CREATE  = 0x01
OP_SET_PRN = 0x02
OP_CRC     = 0x03
OP_EXECUTE = 0x04
OP_SELECT  = 0x06
OP_RESP    = 0x60
TYPE_CMD   = 0x01
TYPE_DATA  = 0x02


def mac_inc(addr: str) -> str:
    v = int(addr.replace(':', ''), 16) + 1
    s = f'{v:012X}'
    return ':'.join(s[i:i+2] for i in range(0, 12, 2))


async def run(address: str, zip_path: str):
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        dat = z.read(next(n for n in names if n.endswith('.dat')))
        fw  = z.read(next(n for n in names if n.endswith('.bin')))
    print(f"Package: {len(fw)} byte firmware, {len(dat)} byte init packet")

    q: asyncio.Queue[bytes] = asyncio.Queue()
    def on_notify(_, data: bytearray): q.put_nowait(bytes(data))

    async def wait(timeout=30):
        d = await asyncio.wait_for(q.get(), timeout)
        if d[0] != OP_RESP:
            raise RuntimeError(f"Unexpected opcode 0x{d[0]:02x}")
        if d[2] != 0x01:
            raise RuntimeError(f"DFU error proc=0x{d[1]:02x} res=0x{d[2]:02x}")
        return d[1], d[3:]

    # ── Step 1: connect and check mode ───────────────────────────────
    print(f"Connecting to {address} ...")
    async with BleakClient(address) as c:
        print("Services found:")
        for svc in c.services:
            print(f"  SVC {svc.uuid}")
            for ch in svc.characteristics:
                print(f"    CHR {ch.uuid}  [{','.join(ch.properties)}]")

        in_app = any(UUID_BUTTONLESS == str(s.uuid).lower() for s in c.services)
        if in_app:
            print("Application mode detected — triggering DFU reboot ...")
            await c.start_notify(UUID_BUTTONLESS, on_notify)
            await c.write_gatt_char(UUID_BUTTONLESS, b'\x01', response=True)
            await asyncio.sleep(2)

    dfu_addr = mac_inc(address) if in_app else address
    if in_app:
        print(f"Reconnecting to bootloader at {dfu_addr} ...")
        await asyncio.sleep(2)

    # ── Step 2: upload ───────────────────────────────────────────────
    async with BleakClient(dfu_addr) as c:
        await c.start_notify(UUID_CTRL, on_notify)

        async def cmd(op, params=b''):
            await c.write_gatt_char(UUID_CTRL, bytes([op]) + params, response=True)

        async def pkt(chunk: bytes):
            await c.write_gatt_char(UUID_DATA, chunk, response=False)

        await cmd(OP_SET_PRN, struct.pack('<H', PRN))
        await wait()

        # Init packet (dat file)
        await cmd(OP_SELECT, bytes([TYPE_CMD]))
        _, extra = await wait()
        offset = struct.unpack_from('<I', extra, 4)[0]
        if offset == 0 or offset > len(dat):
            await cmd(OP_CREATE, bytes([TYPE_CMD]) + struct.pack('<I', len(dat)))
            await wait()
        for i in range(0, len(dat), PKT_SIZE):
            await pkt(dat[i:i+PKT_SIZE])
        await cmd(OP_CRC)
        await wait()
        await cmd(OP_EXECUTE)
        await wait()
        print("Init packet OK")

        # Firmware image
        await cmd(OP_SELECT, bytes([TYPE_DATA]))
        _, extra = await wait()
        max_sz = struct.unpack_from('<I', extra, 0)[0]
        offset = struct.unpack_from('<I', extra, 4)[0]
        total  = len(fw)
        print(f"Uploading {total} bytes (max object {max_sz}, resume offset {offset})")

        obj = (offset // max_sz) * max_sz if max_sz else 0
        while obj < total:
            obj_sz = min(max_sz, total - obj)
            await cmd(OP_CREATE, bytes([TYPE_DATA]) + struct.pack('<I', obj_sz))
            await wait()

            segs, end = 0, obj + obj_sz
            for i in range(obj, end, PKT_SIZE):
                await pkt(fw[i:i+PKT_SIZE])
                segs += 1
                if segs % PRN == 0:
                    _, extra = await wait()
                    curr = struct.unpack_from('<I', extra, 0)[0]
                    pct  = curr * 100 // total
                    bar  = '#' * (pct // 2) + '.' * (50 - pct // 2)
                    print(f"\r  [{bar}] {pct:3d}%  {curr}/{total}B", end='', flush=True)

            await cmd(OP_CRC)
            await wait()
            await cmd(OP_EXECUTE)
            await wait()
            obj += max_sz

        print(f"\r  [{'#'*50}] 100%  {total}/{total}B")
        print("Upload complete — watch will reboot into new firmware.")


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='InfiniTime BLE DFU (bleak-based, Python 3.12+)')
    ap.add_argument('zip', help='DFU zip package path')
    ap.add_argument('-a', '--address', required=True, help='Watch BLE address')
    args = ap.parse_args()
    asyncio.run(run(args.address, args.zip))
