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
            # ESP: DTR/RTS low keeps it out of reset / download mode. nEDBG: forwards the UART only with DTR set.
            s.dtr = s.rts = self.kind == 'sam'
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


print(f'{len(boards)} boards:', ', '.join(f'{b.name} ({os.path.realpath(b.path)})' for b in boards))
print(f'http://localhost:{a.port}')
ThreadingHTTPServer(('0.0.0.0', a.port), H).serve_forever()
