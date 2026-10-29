"""The zone nodes as Zenoh sees them, for the console: one client session to the bench zenohd keeps the last
heartbeat, picture and sensor reading of every node, the vehicle state the nodes are drawing, and runs the
long actions (firmware update over T1S, PLCA burst measurement) in the background.

    ecu/<role>/status   {"id","ip","up","pst","cmds","ptp_ns","role","api"}   1 Hz   (old: t1s/sam<id>/hello)
    ecu/<role>/display  300 B RGB, this node's 10x10 half as drawn            5 Hz   (old: t1s/sam<id>/panel)
    vehicle/private/cabin/stick/x|y (u16), ecu/<role>/diag/proximity_raw (u16),
    vehicle/adas/obstacledetection/rear/center/distance (f32 m)                       (old: vehicle/private/zone/...)
    vehicle/** (VSS)    the car, from the vehicle-API sidecar
Keys and types from tools/vehicle_api.py (generated from api/vehicle_api.yaml); old and v1 keys are both taken
during the migration (docs/VEHICLE_API.md), so old and new firmware show the same.
"""
import base64
import json
import os
import struct
import subprocess
import threading
import time
import sys
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import vehicle_api as api  # noqa: E402

try:
    import zenoh
except ImportError:          # the console still runs on the serial side without it
    zenoh = None

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
ROUTER = os.environ.get('ZONE_ROUTER', 'tcp/192.168.100.51:7448')
IMAGE = os.path.join(ROOT, 'build/node-api/node/zephyr/zephyr.signed.bin')

nodes = {}                   # id -> dict
car = {}                     # VSS key below vehicle/ -> value
sensors = {'stick_x': None, 'stick_y': None, 'prox_raw': None, 'prox_m': None, 'at': 0}
jobs = {'update': None, 'qos': None}
lock = threading.Lock()
roles = {}                   # role -> node id (from the heartbeats)
ads = {'hb': None, 'at': 0}  # ecu/ads/status: the ADS heartbeat (the sidecar publishes it, 1 Hz)
STICK_X, STICK_Y = 'vehicle/private/cabin/stick/x', 'vehicle/private/cabin/stick/y'
PROX_M = 'vehicle/adas/obstacledetection/rear/center/distance'


def _node(i):
    return nodes.setdefault(i, {'id': i, 'hello': None, 'hello_at': 0, 'panel': None, 'panel_at': 0,
                                'ptp': deque(maxlen=120)})


def _on_hello(m):
    try:
        h = json.loads(m.payload.to_string())
    except ValueError:
        return
    if h.get('id') is None:          # ecu/ads/status (the ADS heartbeat) matches ecu/*/status too: not a zone node
        return
    with lock:
        n = _node(h.get('id'))
        n['hello'], n['hello_at'] = h, time.time()
        if h.get('role'):
            roles[h['role']] = h.get('id')
        if h.get('ptp_ns') is not None:
            n['ptp'].append(h['ptp_ns'])


def _on_panel(m):
    k = str(m.key_expr).split('/')   # ecu/<role>/display or t1s/sam<id>/panel
    try:
        i = roles[k[1]] if k[0] == 'ecu' else int(k[1][3:])
    except (KeyError, ValueError):
        return
    with lock:
        n = _node(i)
        n['panel'], n['panel_at'] = base64.b64encode(m.payload.to_bytes()).decode(), time.time()


def _value(k, b):
    """decode by the key table; untabled keys: one byte = a flag"""
    if k in api.KEYS and api.KEYS[k]['type'] in api.STRUCT:
        try:
            return api.unpack(k, b)
        except Exception:
            return None
    return b[0] if len(b) == 1 else None


def _on_ads(m):
    try:
        h = json.loads(m.payload.to_string())
    except ValueError:
        return
    with lock:
        ads['hb'], ads['at'] = h, time.time()


def _on_vehicle(m):
    k = str(m.key_expr)
    if k.startswith('ecu/') and k.endswith('/diag/proximity_raw'):
        k = 'ecu/{ecu}/diag/proximity_raw'
    k = api.LEGACY.get(k, k)       # old key -> v1
    b = m.payload.to_bytes()
    v = _value(k, b)
    with lock:
        if k == STICK_X:
            sensors['stick_x'], sensors['at'] = v, time.time()
        elif k == STICK_Y:
            sensors['stick_y'] = v
        elif k == 'ecu/{ecu}/diag/proximity_raw':
            sensors['prox_raw'], sensors['at'] = v, time.time()
        elif k == PROX_M and v is not None:
            sensors['prox_m'] = round(v, 2)
        elif k.endswith('/target') or not k.startswith('vehicle/'):
            return
        elif v is not None:
            car[k[len('vehicle/'):]] = round(v, 2) if isinstance(v, float) else v
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
                for k in ('ecu/*/status', 't1s/*/hello'):
                    s.declare_subscriber(k, _on_hello)
                for k in ('ecu/*/display', 't1s/*/panel'):
                    s.declare_subscriber(k, _on_panel)
                s.declare_subscriber('ecu/ads/status', _on_ads)
                for k in ('vehicle/**', 'ecu/*/diag/**'):
                    s.declare_subscriber(k, _on_vehicle)
                while True:
                    time.sleep(3600)
            except Exception as e:                       # zenohd not up yet: try again
                print('zone: zenoh', e, flush=True)
                time.sleep(5)
    threading.Thread(target=run, daemon=True).start()


def state():
    now = time.time()
    with lock:
        out = {'ads': dict(ads['hb'] or {}, age=round(now - ads['at'], 1)) if ads['at'] else None,
               'nodes': [], 'car': {k: v for k, v in car.items() if not k.startswith('_')},
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
