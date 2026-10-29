"""What crosses the ESP gateway, live, for the console's packet panel (like t1s-zone-bench's Packets tab).

dumpcap on the bench PC's USB NIC (the gateway's Ethernet side: every node <-> zenohd TCP session, SMP firmware
updates, ping, ARP) streams pcap to us; each frame is classified, Zenoh is decoded down to key names and values
(zenoh_decode.py, the Vehicle API table for the types), and the console gets rates per protocol and per flow plus a
filtered list. The PTP exchange (UDP 5007) never leaves the T1S segment, so it is not here (docs/PACKETS.md).
Needs dumpcap (the wireshark group)."""
import collections
import os
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import zenoh_decode  # noqa: E402

PC = os.environ.get('ZONE_PC_IP', '192.168.100.51')


def _iface():
    # the port that holds the bench address: a USB NIC once, now the port behind the D10 switches, which also carries
    # the Autoware PC's link (hence the capture filter below)
    try:
        for l in subprocess.run(['ip', '-o', '-4', 'addr'], capture_output=True, text=True).stdout.splitlines():
            if f' {PC}/' in l:
                return l.split()[1]
    except OSError:
        pass
    return 'enxc84d442001a1'


IFACE = os.environ.get('ZONE_IFACE') or _iface()
# only the bus subnet: the same port carries the Autoware PC's DDS/CARLA traffic (hundreds of Mbit/s)
FILTER = os.environ.get('ZONE_FILTER', 'net 192.168.100.0/24 or arp')
T0 = time.time()
log = collections.deque(maxlen=6000)        # (t, proto, src, dst, bytes, info, dir)
recent = collections.deque()                 # (time, proto, src, dst, bytes) of the last WINDOW s
WINDOW = 10.0
lock = threading.Lock()
status = {'running': False, 'error': None, 'frames': 0, 'iface': IFACE}
seen = {}                                    # source IP on the bus subnet -> {t, frames, bytes, mac, protos}
names = {PC: 'PC (zenohd)', '192.168.100.65': 'ESP gateway', '192.168.100.73': 'sam3', '192.168.100.74': 'sam4'}


def name(ip):
    return names.get(ip, ip)


def _zenoh(u, src, dst):
    """a TCP segment of a Zenoh session: [u16 LE length][transport message]... (whole batches as a rule)"""
    out, o = [], 0
    while o + 2 <= len(u):
        n = struct.unpack('<H', u[o:o + 2])[0]
        if n == 0 or o + 2 + n > len(u):
            out.append('(continued)')
            break
        out.append(zenoh_decode.describe(bytes(u[o + 2:o + 2 + n]), src, dst))
        o += 2 + n
    return ' | '.join(x for x in out if x) or 'data'


def classify(f):
    """-> (proto, src, dst, info)"""
    et = struct.unpack('>H', f[12:14])[0]
    p = f[14:]
    if et == 0x0806 and len(p) >= 28:
        s, d = '.'.join(map(str, p[14:18])), '.'.join(map(str, p[24:28]))
        return 'ARP', s, d, ('who has %s' % d) if p[7] == 1 else ('%s is at' % s)
    if et == 0x86DD:
        return 'IPv6', '', '', ''
    if et != 0x0800 or len(p) < 20:
        return '0x%04x' % et, '', '', ''
    ihl = (p[0] & 0x0F) * 4
    src, dst = '.'.join(map(str, p[12:16])), '.'.join(map(str, p[16:20]))
    pr = p[9]
    if pr == 1:
        return 'ICMP', src, dst, {8: 'echo request', 0: 'echo reply'}.get(p[ihl], 'type %d' % p[ihl])
    if pr == 6 and len(p) >= ihl + 20:
        sp, dp = struct.unpack('>HH', p[ihl:ihl + 4])
        doff = (p[ihl + 12] >> 4) * 4
        tot = struct.unpack('>H', p[2:4])[0]
        u = p[ihl + doff:tot]
        fl = p[ihl + 13]
        if 7448 in (sp, dp) or 7447 in (sp, dp):
            if not u:
                return 'TCP ack', src, dst, ('SYN ' if fl & 2 else '') + ('FIN ' if fl & 1 else '') + ('RST ' if fl & 4 else '') + 'ack'
            return 'Zenoh', src, dst, _zenoh(u, '%s:%d' % (src, sp), '%s:%d' % (dst, dp))
        return 'TCP', src, dst, '%d -> %d, %d B' % (sp, dp, len(u))
    if pr == 17 and len(p) >= ihl + 8:
        sp, dp = struct.unpack('>HH', p[ihl:ihl + 4])
        u = p[ihl + 8:]
        if 1337 in (sp, dp):
            return 'SMP', src, dst, ('request' if dp == 1337 else 'answer') + ' %d B' % len(u)
        if 5007 in (sp, dp):
            return 'PTP sync', src, dst, ''
        if 5353 in (sp, dp):
            return 'mDNS', src, dst, ''
        if 67 in (sp, dp) or 68 in (sp, dp):
            return 'DHCP', src, dst, ''
        return 'UDP', src, dst, '%d -> %d, %d B' % (sp, dp, len(u))
    return 'IPv4', src, dst, 'proto %d' % pr


def _run():
    while True:
        try:
            pr = subprocess.Popen(['dumpcap', '-q', '-P', '-i', IFACE, '-f', FILTER, '-w', '-'], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
            r = pr.stdout
            hdr = r.read(24)
            if len(hdr) < 24:
                raise OSError((pr.stderr.read() or b'dumpcap ended').decode(errors='replace').strip()[-200:])
            le = hdr[:4] == b'\xd4\xc3\xb2\xa1'
            rec = '<IIII' if le else '>IIII'
            status.update(running=True, error=None)
            while True:
                h = r.read(16)
                if len(h) < 16:
                    raise OSError('dumpcap ended')
                ts, us, incl, orig = struct.unpack(rec, h)
                f = r.read(incl)
                try:
                    proto, s, d, info = classify(f)
                except Exception as e:          # a frame we cannot read is shown, not fatal
                    proto, s, d, info = '?', '', '', repr(e)[:80]
                t = ts + us / 1e6
                dr = 'tx' if s == PC else 'rx' if d == PC else ''
                with lock:
                    status['frames'] += 1
                    log.append((round(t - T0, 3), proto, s, d, orig, info[:400], dr))
                    recent.append((t, proto, s, d, orig))
                    if s.startswith('192.168.100.') and s != PC:
                        e = seen.setdefault(s, {'frames': 0, 'bytes': 0, 'protos': set()})
                        e['t'] = t; e['frames'] += 1; e['bytes'] += orig; e['protos'].add(proto)
                        e['mac'] = ':'.join('%02x' % x for x in f[6:12])
                    while recent and recent[0][0] < t - WINDOW:
                        recent.popleft()
        except Exception as e:
            status.update(running=False, error=str(e))
            time.sleep(5)


def start():
    threading.Thread(target=_run, daemon=True).start()


def talkers():
    """every address that sent a frame through the gateway: the bus as the wire sees it (no USB needed)"""
    now = time.time()
    with lock:
        return {ip: {'age': round(now - e['t'], 1), 'frames': e['frames'], 'bytes': e['bytes'], 'mac': e['mac'],
                     'protos': sorted(e['protos'])} for ip, e in seen.items()}


def rates():
    """frames/s, kbit/s per protocol and per flow over the last WINDOW s; share of the 10 Mbit/s bus (+8 B preamble,
    12 B gap: what a frame costs on the wire)"""
    with lock:
        R = list(recent)
    byp, byf = collections.defaultdict(lambda: [0, 0]), collections.defaultdict(lambda: [0, 0])
    for _, pr, s, d, n in R:
        byp[pr][0] += 1; byp[pr][1] += n
        byf[(name(s), name(d), pr)][0] += 1; byf[(name(s), name(d), pr)][1] += n
    wire = sum(n + 20 for *_, n in R) * 8 / WINDOW / 1e7 * 100
    return {'window': WINDOW, 'status': status, 'bus_pct': round(wire, 2),
            'protos': sorted(([k, round(c / WINDOW, 1), round(b * 8 / WINDOW / 1e3, 1)] for k, (c, b) in byp.items()),
                             key=lambda x: -x[2]),
            'flows': sorted(([f[0], f[1], f[2], round(c / WINDOW, 1), round(b * 8 / WINDOW / 1e3, 1)]
                             for f, (c, b) in byf.items()), key=lambda x: -x[4])[:20]}


def query(q):
    """the list, newest first: p = protocol, text = substring (node names too), limit"""
    g = lambda k, d='': (q.get(k) or [d])[0]
    pr, txt, lim = g('p'), g('text').lower(), min(500, int(g('limit', '200')))
    out = []
    with lock:
        L = list(log)
    for f in reversed(L):
        if pr and f[1] != pr:
            continue
        row = (f[0], f[1], name(f[2]), name(f[3]), f[4], f[5], f[6])
        if txt and txt not in ' '.join(map(str, row)).lower():
            continue
        out.append(row)
        if len(out) >= lim:
            break
    return {'now': round(time.time() - T0, 3), 'rows': out, 'buffered': len(L)}


def wireshark():
    env = dict(os.environ)
    env.setdefault('DISPLAY', ':1')
    try:
        subprocess.Popen(['wireshark', '-i', IFACE, '-k'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return True
    except OSError:
        return False
