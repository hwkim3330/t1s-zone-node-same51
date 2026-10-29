#!/usr/bin/env python3
"""T1S bus console: one page that shows who is on the bus and lets you poke them.

Holds every board's USB serial port open for as long as it runs (opening the ESP's port resets it),
reads the status lines the boards print anyway, and polls them every few seconds:
  SAM E51 nodes (nEDBG CDC):   `t1s status`  -> "t1s: ... PLCA on id N count M ... PST=1 ..." + "net: <ip> ..."
  ESP T1S HAT  (Espressif JTAG): `stat 2000`  -> "stat: ms=.. plca=0/8 link=up ... rx=.. tx=.."
                                 `status`      -> "link: up  ip a.b.c.d/..." and "lan: ..."
    python3 tools/console/server.py [--port 8830]   ->  http://localhost:8830
Only stdlib + pyserial."""
import argparse
import glob
import json
import os
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

import packets
import zone

ap = argparse.ArgumentParser()
ap.add_argument('--port', type=int, default=8830)
a = ap.parse_args()
HERE = os.path.dirname(os.path.abspath(__file__))
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')


class Board:
    def __init__(self, path):
        self.path = path
        base = os.path.basename(path)
        self.kind = 'esp' if 'Espressif' in base else 'sam'
        m = re.search(r'_([0-9A-Fa-f:]{17}|MC\w+?)-if', base)
        self.serial = m.group(1) if m else base
        self.name = ('ESP ' + self.serial[-5:]) if self.kind == 'esp' else ('SAM E51 …' + self.serial[-3:])
        self.st = {'plca_id': None, 'plca_count': None, 'pst': None, 'ip': None, 'link': None,
                   'rx': None, 'tx': None, 'mac': None, 'lan': None, 'seen': 0}
        self.lines = []          # recent console lines (for the page's log and for command replies)
        self.lock = threading.Lock()
        self.s = None
        self.open()
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=self.poller, daemon=True).start()

    def open(self):
        """(Re)open the port; boards get unplugged and come back (flashing one at a time)."""
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = self.path, 115200, 0.2
            # Both lines high, for both kinds. nEDBG forwards the UART only with DTR set. The ESP32-S3's USB serial
            # resets the chip on RTS high + DTR low: the old "both low" passed through that state (pyserial sets
            # DTR before RTS) and every console start rebooted the gateway.
            s.dtr = s.rts = True
            s.open()
            self.s = s
            return True
        except Exception:
            self.s = None
            return False

    def send(self, cmd):
        with self.lock:
            try:
                if self.s:
                    self.s.write(cmd.encode() + b'\r\n')
            except Exception:
                self.s = None

    def reader(self):
        buf = b''
        while True:
            if self.s is None:
                time.sleep(1)
                if self.open():
                    self.lines.append((time.time(), '[console] port reopened'))
                continue
            try:
                buf += self.s.read(4096)
            except Exception:
                try:
                    self.s.close()
                except Exception:
                    pass
                self.s = None; self.st['seen'] = 0
                self.lines.append((time.time(), '[console] port lost (unplugged?)'))
                continue
            *done, buf = buf.split(b'\n')
            for raw in done:
                line = ANSI.sub('', raw.decode(errors='replace')).replace('uart:~$', '').strip()
                line = re.sub(r'^.*?(t1s: |net: |stat: )', r'\1', line)
                if line:
                    self.parse(line)
                    self.lines.append((time.time(), line))
                    del self.lines[:-400]

    def parse(self, l):
        st = self.st
        if m := re.search(r'PLCA on id (\d+) count (\d+)', l):
            st['plca_id'], st['plca_count'] = int(m[1]), int(m[2])
        if l.startswith('t1s:') and 'PLCA off' in l:
            st['plca_id'] = 'csma'
        if m := re.match(r'plca: on, id (\d+) of (\d+)', l):          # ESP `status`
            st['plca_id'], st['plca_count'] = int(m[1]), int(m[2])
            st['pst'] = True if st['plca_id'] == 0 else st['pst']
        if l.startswith('plca: off'):
            st['plca_id'] = 'csma'
        if m := re.search(r'PST=(\d)', l):
            st['pst'] = m[1] == '1'
        if m := re.search(r'link=(\d)', l) if l.startswith('t1s:') else None:
            st['link'] = m[1] == '1'
        if m := re.match(r'net: (\S+) up=(\d) carrier=(\d) \| bytes rx (\d+) tx (\d+)', l):
            st['ip'], st['rx'], st['tx'] = m[1], int(m[4]), int(m[5]); st['seen'] = time.time()
        if m := re.search(r'Link addr : (\S+)', l):
            st['mac'] = m[1].lower()
        if l.startswith('stat: '):
            kv = dict(x.split('=', 1) for x in l[6:].split() if '=' in x)
            if '/' in kv.get('plca', ''):
                i, c = kv['plca'].split('/'); st['plca_id'], st['plca_count'] = int(i), int(c)
                st['pst'] = True if st['plca_id'] == 0 else st['pst']
            st['link'] = kv.get('link') == 'up'
            st['rx'], st['tx'], st['ptp'] = kv.get('rx'), kv.get('wire'), kv.get('ptp')
            st['seen'] = time.time()
        if (m := re.match(r'link: (\w+)\s+ip (\d+\.\d+\.\d+\.\d+)', l)) and m[2] != '0.0.0.0':
            st['ip'] = m[2]
        if m := re.match(r'ptpl: \d+ offset (-?\d+) ns delay (-?\d+) ns', l):
            st['ptp_ns'], st['ptp_delay'], st['ptp_at'] = int(m[1]), int(m[2]), time.time()
        if m := re.match(r'ptp: .*last offset (-?\d+) ns delay (-?\d+) ns', l):
            if 'lock off' not in l:
                st['ptp_ns'], st['ptp_delay'], st['ptp_at'] = int(m[1]), int(m[2]), time.time()
        if m := re.match(r'mode: (\w+)', l):
            st['mode'] = m[1]
            if m[1] == 'bridge':
                st['ip'] = None
        if m := re.match(r'bridge (\S+) -> (\S+): in (\d+) fwd (\d+) .*dropped\(no TX room\) (\d+)', l):
            st['br_' + ('down' if m[2] == 'T1S' else 'up')] = f'{m[4]} fwd, {m[5]} dropped'
        if m := re.match(r'w5500: .*-> cable (\w+)', l):
            st['lan'] = 'up' if m[1] == 'LINKED' else 'down'
        if m := re.match(r'lan: (\w+)\s+ip (\S+)', l):
            st['lan'] = f'{m[1]} {m[2]}'

    def poller(self):
        time.sleep(1)
        if self.kind == 'esp':
            # `stat` is not saved on the ESP: after any reboot it goes quiet, so ask again every time
            while True:
                self.send('status'); time.sleep(1); self.send('stat 2000')
                time.sleep(10)
        else:
            self.send('net iface')
            n = 0
            while True:
                time.sleep(3); self.send('t1s status')
                n += 1
                if n % 3 == 0:
                    self.send('ptp')

    def run(self, cmd, wait):
        t0 = time.time(); self.send(cmd); time.sleep(wait)
        return [l for t, l in self.lines if t >= t0 and not l.startswith(('stat: ', 't1s: ', 'net: '))]


boards = []


def scan():
    """Boards plugged in later (a new node, or one that was away while another was flashed) join the page."""
    while True:
        known = {b.path for b in boards}
        for p in sorted(glob.glob('/dev/serial/by-id/*')):
            if ('nEDBG' in p or 'Espressif' in p) and p not in known:
                try:
                    boards.append(Board(p))
                except Exception:
                    pass
        time.sleep(3)


threading.Thread(target=scan, daemon=True).start()
time.sleep(1)


GATEWAY = os.environ.get('ZONE_GATEWAY_IP', '192.168.100.65')
# on the same subnet but not on the T1S bus: the two D10 switches between the PCs (KETI1 .1, KETI2 .2) and the older
# switch addresses (.4); the capture sees their management frames
NOT_NODES = set(os.environ.get('ZONE_NOT_NODES', '192.168.100.1,192.168.100.2,192.168.100.4').split(','))
gw = {'ok': None, 'ms': None, 't': 0, 'last_ok': 0, 'hist': []}


def _ping_gateway():
    # the ESP bridges the nodes' frames but sends few of its own: asked directly every 5 s
    while True:
        r = subprocess.run(['ping', '-c1', '-W1', GATEWAY], capture_output=True, text=True)
        m = re.search(r'time=([0-9.]+)', r.stdout)
        ok = r.returncode == 0
        gw.update(ok=ok, ms=float(m.group(1)) if m else gw['ms'], t=time.time())
        if ok:
            gw['last_ok'] = time.time()
        gw['hist'] = (gw['hist'] + [ok])[-24:]          # the last 2 minutes
        time.sleep(5)


def bus():
    """who is on the bus, from the network: zone heartbeats over Zenoh (id, role, PLCA beacon, PTP) and the frames the
    capture saw from each address. A board on USB only adds its console; a board on USB alone is not on the bus."""
    z, seen = zone.state(), packets.talkers()
    usb = {b.st['ip']: b.name for b in boards if b.st.get('ip')}
    # the ESP answers ~1 ping in 10 late while it bridges the bus (2/20 lost, 2026-10-29) and the nodes' traffic goes
    # on: red only after 20 s without an answer, the loss shown apart
    h = gw['hist']
    out = [{'kind': 'esp', 'name': 'ESP 게이트웨이', 'ip': GATEWAY, 'plca_id': 0, 'alive': time.time() - gw['last_ok'] < 20,
            'ping_ms': gw['ms'], 'ping_loss': round(100 * h.count(False) / len(h)) if h else None,
            'wire': seen.get(GATEWAY), 'usb': usb.get(GATEWAY)}]
    ips = set()
    for n in z['nodes']:
        h = n.get('hello') or {}
        ip = h.get('ip')
        if not ip:
            continue
        ips.add(ip)
        w = seen.get(ip)
        out.append({'kind': 'sam', 'name': h.get('role') or f"sam{n['id']}", 'id': n['id'], 'plca_id': n['id'], 'ip': ip,
                    'alive': n['hello_age'] < 5, 'hb_age': n['hello_age'], 'pst': h.get('pst'), 'ptp_ns': h.get('ptp_ns'),
                    'api': h.get('api'), 'fw': h.get('fw'), 'wire': w, 'mac': w and w['mac'], 'usb': usb.get(ip)})
    for ip, w in seen.items():          # talking on the wire without a heartbeat (old firmware, no Zenoh yet)
        if ip not in ips and ip != GATEWAY and ip not in NOT_NODES:
            out.append({'kind': 'sam', 'name': packets.name(ip), 'plca_id': None, 'ip': ip, 'alive': w['age'] < 15,
                        'wire': w, 'mac': w['mac'], 'usb': usb.get(ip), 'no_hb': True})
    return {'nodes': out, 'capture': packets.status, 'usb_only': [b.name for b in boards if b.kind == 'sam' and b.st.get('ip') not in ips]}


class H(BaseHTTPRequestHandler):
    def log_message(self, *x):
        pass

    def reply(self, code, body, ctype='application/json'):
        b = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code); self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)

    def do_GET(self):
        if self.path in ('/', '/index.html'):
            return self.reply(200, open(os.path.join(HERE, 'index.html'), 'rb').read(), 'text/html; charset=utf-8')
        if self.path == '/api/zone':
            return self.reply(200, zone.state())
        if self.path == '/api/bus':
            return self.reply(200, bus())
        if self.path.startswith('/icon.png'):
            p = os.path.join(HERE, 'icon.png')
            if os.path.exists(p):
                return self.reply(200, open(p, 'rb').read(), 'image/png')
        if self.path.startswith('/api/log'):           # /api/log?i=1&n=300: one board's recent console lines
            from urllib.parse import parse_qs, urlparse
            q = parse_qs(urlparse(self.path).query)
            b = boards[int(q.get('i', ['0'])[0])]
            n = int(q.get('n', ['200'])[0])
            return self.reply(200, [f'{time.strftime("%T", time.localtime(t))} {l}' for t, l in b.lines[-n:]])
        if self.path.startswith('/api/packets/rates'):
            for n in zone.state()['nodes']:          # the node names in the list: the ECU role from its heartbeat
                h = n.get('hello') or {}
                if h.get('ip'):
                    packets.names[h['ip']] = f"{h.get('role') or 'sam'} (sam{n['id']})"
            return self.reply(200, packets.rates())
        if self.path.startswith('/api/packets'):     # /api/packets?p=Zenoh&text=beam&limit=200
            from urllib.parse import parse_qs, urlparse
            return self.reply(200, packets.query(parse_qs(urlparse(self.path).query)))
        if self.path == '/api/state':
            return self.reply(200, [{'i': i, 'kind': b.kind, 'name': b.name, 'serial': b.serial,
                                     'tty': os.path.realpath(b.path), **b.st,
                                     'log': [l for _, l in b.lines[-14:]]} for i, b in enumerate(boards)])
        self.reply(404, {})

    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        q = json.loads(self.rfile.read(n) or b'{}')
        if self.path == '/api/cmd':
            b = boards[int(q['i'])]
            cmd = str(q['cmd'])[:120].replace('\r', '').replace('\n', '')
            return self.reply(200, {'out': b.run(cmd, float(q.get('wait', 1.5)))})
        if self.path == '/api/zone/sh':           # {id, cmd, wait}: a node's console over the bus, no USB
            out = zone.sh(int(q['id']), str(q['cmd'])[:90].replace('\n', ' '), float(q.get('wait', 5)))
            return self.reply(200, {'out': out if out is not None else ['(응답 없음: 노드가 버스에 없거나 0.13.10 이전 펌웨어)']})
        if self.path == '/api/wireshark':
            return self.reply(200, {'ok': packets.wireshark()})
        if self.path == '/api/zone/update':
            return self.reply(200, {'started': zone.update_all()})
        if self.path == '/api/zone/qos':
            return self.reply(200, {'started': zone.qos()})
        if self.path == '/api/ping':               # every node pings every other node (3 each)
            res = []
            for dst in boards:                     # the PC itself, through the ESP bridge
                if dst.st['ip']:
                    out = subprocess.run(['ping', '-c', '3', '-W', '2', dst.st['ip']], capture_output=True, text=True).stdout
                    ms = [float(x) for x in re.findall(r'time=([\d.]+) ms', out)]
                    res.append({'src': 'PC', 'dst': dst.name, 'ok': len(ms), 'ms': ms})
            for src in boards:
                for dst in boards:
                    if src is dst or not dst.st['ip'] or not src.st['ip']:
                        continue
                    cmd = f"net ping -c 3 {dst.st['ip']}" if src.kind == 'sam' else f"ping {dst.st['ip']} 3"
                    out = src.run(cmd, 4.5)
                    ms = [float(x) for l in out for x in re.findall(r'time=([\d.]+) ms|^ping: seq \d+ ([\d.]+) ms', l) for x in x if x]
                    ok = len(ms)
                    res.append({'src': src.name, 'dst': dst.name, 'ok': ok, 'ms': ms})
            return self.reply(200, res)
        self.reply(404, {})


zone.start()
packets.start()
threading.Thread(target=_ping_gateway, daemon=True).start()
print(f'{len(boards)} boards:', ', '.join(f'{b.name} ({os.path.realpath(b.path)})' for b in boards))
print(f'http://localhost:{a.port}')
ThreadingHTTPServer(('0.0.0.0', a.port), H).serve_forever()
