#!/usr/bin/env python3
"""PLCA burst on the gateway vs. command latency under load.

PC -> zenohd -> ESP gateway (coordinator, PLCA id 0) -> T1S -> SAM: a `ping` on t1s/sam<id>/cmd, answered by
`pong` on t1s/sam<id>/ack, timed on the PC. Everything the PC sends to a node leaves the bus through the
gateway's one transmit opportunity per PLCA cycle, so when the gateway has its own traffic queued (here its
`blast` to another node), a command waits behind it. PLCA burst (MMS4 0xCA05: MAXBC 15:8, BTMR 7:0) lets the
gateway send MAXBC more frames in the same opportunity.

    tools/qos_test.py [samples] [--node 3] [--load-to 192.168.100.74]   (tools/bench_up.sh running)
"""
import argparse
import json
import statistics as st
import threading
import time
import urllib.request

import zenoh

ap = argparse.ArgumentParser()
ap.add_argument('samples', type=int, nargs='?', default=100)
ap.add_argument('--node', type=int, default=3)
ap.add_argument('--load-to', default='192.168.100.74')
a = ap.parse_args()


def console(kind, c, wait=1.0):
    s = json.load(urllib.request.urlopen('http://localhost:8830/api/state', timeout=10))
    i = [b['i'] for b in s if b['kind'] == kind][0]
    r = urllib.request.Request('http://localhost:8830/api/cmd', data=json.dumps({'i': i, 'cmd': c, 'wait': wait}).encode(),
                               method='POST')
    return json.load(urllib.request.urlopen(r, timeout=60))['out']


cfg = zenoh.Config()
cfg.insert_json5('mode', '"client"')
cfg.insert_json5('connect/endpoints', '["tcp/192.168.100.51:7448"]')
z = zenoh.open(cfg)
got = threading.Event()
z.declare_subscriber('t1s/sam%d/ack' % a.node, lambda m: got.set() if m.payload.to_string() == 'pong' else None)
pub = z.declare_publisher('t1s/sam%d/cmd' % a.node)
time.sleep(1)


def rtts(n):
    out = []
    for _ in range(n):
        got.clear()
        t0 = time.perf_counter()
        pub.put('ping')
        if got.wait(1.0):
            out.append((time.perf_counter() - t0) * 1000)
        time.sleep(0.05)
    return out


def show(name, v, n):
    if not v:
        print('%-26s no answers' % name)
        return
    v = sorted(v)
    q = lambda p: v[min(len(v) - 1, int(p * len(v)))]
    print('%-26s n %3d/%d  p50 %6.2f  p95 %6.2f  p99 %6.2f  max %6.2f ms' % (name, len(v), n, q(.5), q(.95), q(.99), v[-1]))


for maxbc in (0, 4):
    console('esp', 'reg w 4 ca05 %x' % (maxbc << 8 | 0x80))
    rb = ' '.join(console('esp', 'reg r 4 ca05'))
    print('gateway PLCA burst MAXBC %d  (%s)' % (maxbc, rb.strip()[-40:]))
    show('  idle', rtts(a.samples), a.samples)
    console('esp', 'blast %s %d 1400 9 0' % (a.load_to, int(a.samples * 0.08) + 8), wait=0.3)
    time.sleep(2)
    show('  gateway blasting %s' % a.load_to.split('.')[-1], rtts(a.samples), a.samples)
    time.sleep(int(a.samples * 0.08) + 8)
console('esp', 'reg w 4 ca05 80')      # back to no burst (the reset value)
z.close()
