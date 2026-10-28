#!/usr/bin/env python3
"""What the zone bus actually carries: frame sizes and rates per kind of traffic, measured, as a Markdown table
plus the raw captures (for docs/PACKETS.md and whoever writes the documents later).

Two views at once, for --secs seconds:
  PC   dumpcap on the bench PC's USB NIC (the ESP gateway's RJ45 side): Zenoh TCP node <-> zenohd, SMP, ping,
       ARP -- everything that crosses the gateway
  T1S  the ESP gateway's `capture` (PSRAM, 128 B snap, true length kept): the frames on the bus that are addressed
       to the ESP itself or are broadcast / multicast -- the PTP exchange (UDP 5007) lives only here
    tools/capture_sizes.py [--secs 30] [--iface enxc84d442001a1] [--out docs/captures]
Needs tools/bench_up.sh (console :8830) and tshark/dumpcap (wireshark group).
"""
import argparse
import json
import os
import statistics as st
import struct
import subprocess
import time
import urllib.request

ap = argparse.ArgumentParser()
ap.add_argument('--secs', type=int, default=30)
ap.add_argument('--iface', default='enxc84d442001a1')
ap.add_argument('--out', default='docs/captures')
a = ap.parse_args()
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
OUT = os.path.join(ROOT, a.out)
os.makedirs(OUT, exist_ok=True)
TS = time.strftime('%Y%m%d-%H%M%S')
CON = 'http://localhost:8830'


def esp_index():
    s = json.load(urllib.request.urlopen(CON + '/api/state', timeout=10))
    return [b['i'] for b in s if b['kind'] == 'esp'][0]


def esp(cmd, wait=1.5):
    r = urllib.request.Request(CON + '/api/cmd', data=json.dumps({'i': esp_index(), 'cmd': cmd, 'wait': wait}).encode(),
                               method='POST')
    return json.load(urllib.request.urlopen(r, timeout=60))['out']


# ---------------------------------------------------------------- capture
pc_file = os.path.join(OUT, f'pc-{TS}.pcapng')
dump = subprocess.Popen(['dumpcap', '-q', '-i', a.iface, '-a', f'duration:{a.secs}', '-w', pc_file],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
esp('capture start 8000')
time.sleep(a.secs)
esp('capture stop')
dump.wait()

# the ESP's records, 100 at a time: "cap <i> <t_us> <ifc> <len> <hex of the first 128 B>"
recs, i = [], 0
while True:
    out = esp(f'capture dump {i} 100', wait=2.5)
    got = [l.split() for l in out if l.startswith('cap ') and len(l.split()) >= 6]
    for g in got:
        recs.append((int(g[2]), int(g[3]), int(g[4]), bytes.fromhex(g[5])))
    if len(got) < 100:
        break
    i += 100
t1s_file = os.path.join(OUT, f't1s-{TS}.pcap')
with open(t1s_file, 'wb') as f:
    f.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 128, 1))
    for t, ifc, ln, d in recs:
        f.write(struct.pack('<IIII', t // 1000000, t % 1000000, len(d), ln) + d)


# ---------------------------------------------------------------- classify
def rows(path):
    fields = ['frame.time_relative', 'frame.len', 'eth.src', 'eth.type', 'ip.src', 'ip.dst', 'ip.proto',
              'tcp.srcport', 'tcp.dstport', 'tcp.len', 'udp.srcport', 'udp.dstport', 'udp.length']
    cmd = ['tshark', '-r', path, '-T', 'fields', '-E', 'separator=|'] + sum([['-e', f] for f in fields], [])
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    for l in out.splitlines():
        v = dict(zip(fields, l.split('|')))
        yield v


def kind(v):
    et = v['eth.type'].split(',')[0]
    if et == '0x0806':
        return 'ARP'
    if et == '0x88f7':
        return 'gPTP (802.1AS)'
    if v['ip.proto'] == '1':
        return 'ICMP (ping)'
    if v['tcp.srcport'] or v['tcp.dstport']:
        ports = {v['tcp.srcport'], v['tcp.dstport']}
        if '7448' in ports or '7447' in ports:
            pl = int(v['tcp.len'] or 0)
            up = v['tcp.dstport'] in ('7448', '7447')
            if pl == 0:
                return 'Zenoh TCP ack (no data)'
            return ('Zenoh node->PC' if up else 'Zenoh PC->node') + (' small (<=60 B data)' if pl <= 60 else ' (>60 B data)')
        return 'other TCP'
    if v['udp.srcport'] or v['udp.dstport']:
        ports = {v['udp.srcport'], v['udp.dstport']}
        if '5007' in ports:
            return 'PTP sync exchange (UDP 5007)'
        if '1337' in ports:
            return 'SMP firmware update (UDP 1337)'
        if '7447' in ports or '7446' in ports:
            return 'Zenoh UDP / scouting'
        return 'other UDP'
    return f'other (ethertype {et})'


def table(path, secs, title):
    by = {}
    for v in rows(path):
        try:
            by.setdefault(kind(v), []).append(int(v['frame.len']))
        except ValueError:
            continue
    lines = [f'### {title}', '', f'`{os.path.relpath(path, ROOT)}`, {secs} s', '',
             '| kind | frames | /s | size min / median / max (B) | B/s | % of 10 Mb/s (+8 B preamble) |',
             '|---|---|---|---|---|---|']
    for k, s in sorted(by.items(), key=lambda kv: -sum(kv[1])):
        bps = sum(x + 8 for x in s) / secs
        lines.append(f'| {k} | {len(s)} | {len(s) / secs:.1f} | {min(s)} / {int(st.median(s))} / {max(s)} | '
                     f'{sum(s) / secs:.0f} | {bps * 8 / 1e7 * 100:.2f} |')
    tot = sum(sum(s) for s in by.values())
    lines += ['', f'total {tot / secs:.0f} B/s = {tot * 8 / secs / 1e7 * 100:.2f} % of 10 Mb/s', '']
    return '\n'.join(lines)


md = [f'## Capture {TS}', '',
      table(pc_file, a.secs, 'PC side (bench USB NIC, across the ESP gateway)'),
      table(t1s_file, a.secs, f'T1S side (ESP gateway `capture`: frames to the ESP + broadcast/multicast; {len(recs)} frames)')]
print('\n'.join(md))
with open(os.path.join(OUT, f'sizes-{TS}.md'), 'w') as f:
    f.write('\n'.join(md) + '\n')
