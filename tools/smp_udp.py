#!/usr/bin/env python3
"""Update a T1S node's firmware over the network: mcumgr SMP over UDP (port 1337), no USB.

    tools/smp_udp.py 192.168.100.73 list                      # slots, versions, hashes
    tools/smp_udp.py 192.168.100.73 update build/node-mcuboot/node/zephyr/zephyr.signed.bin
    tools/smp_udp.py 192.168.100.73 reset

`update` = upload into slot 1, mark it for a test boot, reset. MCUboot swaps it in; the app confirms
itself once its network driver is up, otherwise the next reset swaps the old image back.
Several hosts at once: `update` takes a comma-separated list. Needs cbor2 (already used by imgtool)."""
import hashlib
import socket
import struct
import sys
import time

import cbor2

PORT, CHUNK = 1337, 384      # node's SMP netbuf is 1024 B; keep the CBOR frame well inside it
seq = 0


def smp(sock, host, op, group, cmd, body, timeout=3.0, tries=4):
    global seq
    seq = (seq + 1) & 0xff
    payload = cbor2.dumps(body)
    frame = struct.pack('>BBHHBB', op, 0, len(payload), group, seq, cmd) + payload
    for _ in range(tries):
        sock.sendto(frame, (host, PORT))
        sock.settimeout(timeout)
        try:
            while True:
                data, _ = sock.recvfrom(2048)
                if data[6] == seq:
                    return cbor2.loads(data[8:])
        except socket.timeout:
            continue
    raise TimeoutError(f'{host}: no SMP reply (group {group} cmd {cmd})')


def state(sock, host):
    return smp(sock, host, 0, 1, 0, {}).get('images', [])


def show(sock, host):
    for im in state(sock, host):
        flags = ' '.join(k for k in ('active', 'confirmed', 'pending', 'permanent') if im.get(k))
        print(f"{host} slot {im['slot']}: v{im.get('version')} {im['hash'].hex()[:16]} {flags}")


def update(sock, host, path):
    img = open(path, 'rb').read()
    sha = hashlib.sha256(img).digest()
    off, t0 = 0, time.time()
    while off < len(img):
        body = {'image': 0, 'off': off, 'data': img[off:off + CHUNK]}
        if off == 0:
            body.update(len=len(img), sha=sha)
        r = smp(sock, host, 2, 1, 1, body)
        if r.get('rc', 0):
            raise RuntimeError(f'{host}: upload rc {r}')
        if off * 10 // len(img) != r['off'] * 10 // len(img):
            print(f'{host}: {r["off"] * 100 // len(img)} %', flush=True)
        off = r['off']
    dt = time.time() - t0
    print(f'{host}: {len(img)} B in {dt:.1f} s ({len(img) * 8 / dt / 1e3:.0f} kbit/s)')
    new = [im for im in state(sock, host) if im['slot'] == 1][0]
    r = smp(sock, host, 2, 1, 0, {'hash': new['hash'], 'confirm': False})
    print(f'{host}: test boot of {new["hash"].hex()[:16]} marked ({r.get("rc", 0)}), resetting')
    smp(sock, host, 2, 0, 5, {})


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    hosts, what = sys.argv[1].split(','), sys.argv[2]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for h in hosts:
        if what == 'list':
            show(sock, h)
        elif what == 'reset':
            smp(sock, h, 2, 0, 5, {})
        elif what == 'update':
            update(sock, h, sys.argv[3])
    if what == 'update':
        time.sleep(15)       # MCUboot swap (~2 x 176 KB of 8 KiB erases) + boot + T1S up
        for h in hosts:
            try:
                show(sock, h)
            except TimeoutError as e:
                print(e)


main()
