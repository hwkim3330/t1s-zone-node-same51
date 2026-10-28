#!/usr/bin/env python3
"""Soak test of the zone bus: every 30 s for N minutes, check a SAM node end to end and log anything off.
    tools/soak.py [minutes] [node ip]     (needs tools/bench_up.sh running)"""
import json, re, statistics as st, subprocess, sys, time, urllib.request
import zenoh

MIN = float(sys.argv[1]) if len(sys.argv) > 1 else 15
IP = sys.argv[2] if len(sys.argv) > 2 else '192.168.100.73'


def cmd(i, c, wait=1.2):
    r = urllib.request.Request('http://localhost:8830/api/cmd', data=json.dumps({'i': i, 'cmd': c, 'wait': wait}).encode(), method='POST')
    return json.load(urllib.request.urlopen(r, timeout=30))['out']


def sam_index():
    s = json.load(urllib.request.urlopen('http://localhost:8830/api/state', timeout=10))
    return [b['i'] for b in s if b['kind'] == 'sam' and b.get('ip') == IP][0]


c = zenoh.Config(); c.insert_json5('mode', '"client"'); c.insert_json5('connect/endpoints', '["tcp/192.168.100.51:7448"]')
z = zenoh.open(c)
hello = []
z.declare_subscriber('t1s/*/hello', lambda m: hello.append((time.time(), json.loads(m.payload.to_string()))))
offs, problems, rounds = [], [], 0


class Problems(list):
    def append(self, p):
        print('PROBLEM', *p, flush=True)        # at once, not only in the summary
        super().append(p)


problems = Problems()
t_end = time.time() + MIN * 60
first_up = None
while time.time() < t_end:
    time.sleep(30)
    rounds += 1
    now = time.time()
    try:
        i = sam_index()
        recent = [h for t, h in hello if now - t < 30 and h.get('ip') == IP]
        if not recent:
            problems.append((time.strftime('%T'), 'no hello in 30 s'))
        else:
            up = recent[-1]['up']
            if first_up is not None and up < first_up:
                problems.append((time.strftime('%T'), 'node rebooted (uptime %d)' % up))
            first_up = up
            o = [h['ptp_ns'] for h in recent if h.get('ptp_ns') is not None]
            offs += o
            if len(o) < len(recent):
                problems.append((time.strftime('%T'), 'not locked in %d/%d hellos' % (len(recent) - len(o), len(recent))))
        lamps = ' '.join(cmd(i, 'lamps'))
        if 'live' not in lamps:
            problems.append((time.strftime('%T'), 'lamps: ' + lamps[-80:]))
        mem = ' '.join(cmd(i, 'net mem'))
        m = re.search(r'\s(\d+)\s+(\d+)\s+RX\b', mem)
        if m and int(m[2]) < int(m[1]) // 2:
            problems.append((time.strftime('%T'), 'RX pool %s/%s free' % (m[2], m[1])))
        zl = ' '.join(cmd(i, 'zenoh'))
        rc = re.search(r'watchdog reconnects (\d+)', zl)
        if rc and int(rc[1]) > 0:
            problems.append((time.strftime('%T'), 'zenoh watchdog reconnects ' + rc[1]))
        for host in (IP, '192.168.100.65'):
            if subprocess.run(['ping', '-c', '2', '-W', '1', host], capture_output=True).returncode:
                problems.append((time.strftime('%T'), 'ping %s failed' % host))
        smp = subprocess.run(['python3', 'tools/smp_udp.py', IP, 'list'], capture_output=True, text=True, timeout=30)
        if 'active' not in smp.stdout:
            problems.append((time.strftime('%T'), 'SMP list failed'))
    except Exception as e:
        problems.append((time.strftime('%T'), 'check error: %r' % e))
    print('%s round %d: %d offsets so far, problems %d' % (time.strftime('%T'), rounds, len(offs), len(problems)), flush=True)
z.close()
if offs:
    print('PTP offset over %.0f min: n %d, mean %.1f ns, sd %.1f ns, max |%d| ns' % (MIN, len(offs), st.mean(offs), st.pstdev(offs), max(abs(x) for x in offs)))
print('problems:', problems if problems else 'none')
