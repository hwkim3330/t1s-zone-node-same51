"""The zone nodes as Zenoh sees them, for the console: one client session to the bench zenohd keeps the last
heartbeat, picture and sensor reading of every node, the vehicle state the nodes are drawing, and runs the
long actions (firmware update over T1S, PLCA burst measurement) in the background.

    t1s/sam<id>/hello   {"id","ip","up","pst","cmds","ptp_ns"}      1 Hz
    t1s/sam<id>/panel   300 B RGB, this node's 10x10 half as drawn    5 Hz
    vehicle/private/zone/stick/x|y, .../proximity/raw (u16), vehicle/private/external/proximity_rear (f32 m)
    vehicle/** (VSS)    the car, from the vehicle-API sidecar
"""
import base64
import json
import os
import struct
import subprocess
import threading
import time
from collections import deque

try:
    import zenoh
except ImportError:          # the console still runs on the serial side without it
    zenoh = None

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
ROUTER = os.environ.get('ZONE_ROUTER', 'tcp/192.168.100.51:7448')
IMAGE = os.path.join(ROOT, 'build/node-g/node/zephyr/zephyr.signed.bin')

nodes = {}                   # id -> dict
car = {}                     # VSS key below vehicle/ -> value
sensors = {'stick_x': None, 'stick_y': None, 'prox_raw': None, 'prox_m': None, 'at': 0}
jobs = {'update': None, 'qos': None}
lock = threading.Lock()
VSS_FMT = {'speed': '<f', 'powertrain/transmission/selectedgear': '<b', 'chassis/steeringwheel/angle': '<h',
           'private/drivingmode': '<B'}


def _node(i):
    return nodes.setdefault(i, {'id': i, 'hello': None, 'hello_at': 0, 'panel': None, 'panel_at': 0,
                                'ptp': deque(maxlen=120)})


def _on_hello(m):
    try:
        h = json.loads(m.payload.to_string())
    except ValueError:
        return
    with lock:
        n = _node(h.get('id'))
        n['hello'], n['hello_at'] = h, time.time()
        if h.get('ptp_ns') is not None:
            n['ptp'].append(h['ptp_ns'])


def _on_panel(m):
    k = str(m.key_expr)            # t1s/sam<id>/panel
    try:
        i = int(k.split('/')[1][3:])
    except ValueError:
        return
    with lock:
        n = _node(i)
        n['panel'], n['panel_at'] = base64.b64encode(m.payload.to_bytes()).decode(), time.time()


def _on_vehicle(m):
    k = str(m.key_expr)[len('vehicle/'):]
    b = m.payload.to_bytes()
    with lock:
        if k == 'private/zone/stick/x' and len(b) == 2:
            sensors['stick_x'], sensors['at'] = struct.unpack('<H', b)[0], time.time()
        elif k == 'private/zone/stick/y' and len(b) == 2:
            sensors['stick_y'] = struct.unpack('<H', b)[0]
        elif k == 'private/zone/proximity/raw' and len(b) == 2:
            sensors['prox_raw'], sensors['at'] = struct.unpack('<H', b)[0], time.time()
        elif k == 'private/external/proximity_rear' and len(b) == 4:
            sensors['prox_m'] = round(struct.unpack('<f', b)[0], 2)
        elif k.endswith('/target'):
            return
        elif k in VSS_FMT and len(b) >= struct.calcsize(VSS_FMT[k]):
            car[k] = struct.unpack(VSS_FMT[k], b[:struct.calcsize(VSS_FMT[k])])[0]
        elif len(b) == 1:
            car[k] = b[0]
        car['_at'] = time.time()


def start():
    if zenoh is None:
        return
    def run():
        while True:
            try:
                c = zenoh.Config()
                c.insert_json5('mode', '"client"')
                c.insert_json5('connect/endpoints', json.dumps([ROUTER]))
                s = zenoh.open(c)
                s.declare_subscriber('t1s/*/hello', _on_hello)
                s.declare_subscriber('t1s/*/panel', _on_panel)
                s.declare_subscriber('vehicle/**', _on_vehicle)
                while True:
                    time.sleep(3600)
            except Exception as e:                       # zenohd not up yet: try again
                print('zone: zenoh', e, flush=True)
                time.sleep(5)
    threading.Thread(target=run, daemon=True).start()


def state():
    now = time.time()
    with lock:
        out = {'nodes': [], 'car': {k: v for k, v in car.items() if not k.startswith('_')},
               'car_age': round(now - car['_at'], 1) if '_at' in car else None,
               'sensors': dict(sensors, age=round(now - sensors['at'], 1) if sensors['at'] else None),
               'jobs': {k: v for k, v in jobs.items()}, 'zenoh': zenoh is not None}
        for i, n in sorted(nodes.items(), key=lambda kv: kv[0] or 0):
            p = list(n['ptp'])
            out['nodes'].append({'id': i, 'hello': n['hello'], 'hello_age': round(now - n['hello_at'], 1),
                                 'panel': n['panel'], 'panel_age': round(now - n['panel_at'], 1) if n['panel_at'] else None,
                                 'ptp_hist': p[-60:],
                                 'ptp_sd': round((sum((x - sum(p) / len(p)) ** 2 for x in p) / len(p)) ** 0.5, 1) if len(p) > 5 else None})
    return out


def _job(name, cmd):
    def run():
        jobs[name] = {'running': True, 'started': time.strftime('%T'), 'out': ''}
        try:
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=900)
            out = r.stdout + r.stderr
        except Exception as e:
            out = repr(e)
        lines = [l for l in out.splitlines() if not l.rstrip().endswith('%')]
        jobs[name] = {'running': False, 'started': jobs[name]['started'], 'finished': time.strftime('%T'),
                      'out': '\n'.join(lines[-24:])}
    if jobs.get(name) and jobs[name].get('running'):
        return False
    threading.Thread(target=run, daemon=True).start()
    return True


def update_all():
    with lock:
        ips = [n['hello']['ip'] for n in nodes.values() if n['hello'] and time.time() - n['hello_at'] < 10]
    if not ips or not os.path.exists(IMAGE):
        return False
    return _job('update', ['python3', 'tools/smp_udp.py', ','.join(ips), 'update', IMAGE])


def qos():
    return _job('qos', ['python3', 'tools/qos_test.py', '60'])
