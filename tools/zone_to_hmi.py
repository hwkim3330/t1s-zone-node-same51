#!/usr/bin/env python3
"""The T1S zone in the car's HMI, and the zone's thumbstick as a driver control.

  zone -> HMI  every 0.5 s: the console's /api/zone (nodes, pictures, sensors) -> POST <gateway>/api/zone, which
               the PLEOS HMI shows as its "T1S 존" app (panels, sync, proximity) and turns into alerts
               (rear proximity < 0.4 m, a node not answering)
  stick -> car a flick up / down (past 80 % and back) = the maximum speed +5 / -5 km/h, through the gateway's
               set_velocity_limit -- the same request the HMI's < > buttons send. Nothing else is commanded.

    tools/zone_to_hmi.py [--gateway http://10.42.0.90:8780] [--console http://localhost:8830]
Token: ~/.config/pleos_gateway_token (the gateway's control token). Started by tools/bench_up.sh.
"""
import argparse
import json
import os
import time
import urllib.request

ap = argparse.ArgumentParser()
ap.add_argument('--gateway', default=os.environ.get('GATEWAY', 'http://10.42.0.90:8780'))
ap.add_argument('--console', default='http://localhost:8830')
ap.add_argument('--step', type=float, default=5.0)
a = ap.parse_args()
TOKF = os.path.expanduser('~/.config/pleos_gateway_token')


def token():
    try:
        return open(TOKF).read().strip()
    except OSError:
        return ''


def get(url, timeout=3):
    return json.load(urllib.request.urlopen(url, timeout=timeout))


def post(path, body, timeout=10):
    r = urllib.request.Request(a.gateway + path, data=json.dumps(body).encode(), method='POST',
                               headers={'Content-Type': 'application/json', 'X-Control-Token': token()})
    return json.load(urllib.request.urlopen(r, timeout=timeout))


def log(*m):
    print(time.strftime('%T'), *m, flush=True)


armed = True          # a flick counts once; the stick has to come back to the centre before the next
last_err = 0
while True:
    time.sleep(0.5)
    try:
        z = get(a.console + '/api/zone')
    except Exception as e:
        if time.time() - last_err > 30:
            log('console:', e)
            last_err = time.time()
        continue
    nodes = [{'id': n['id'], 'ip': (n['hello'] or {}).get('ip'), 'up': (n['hello'] or {}).get('up'),
              'ptp_ns': (n['hello'] or {}).get('ptp_ns'), 'ptp_sd': n.get('ptp_sd'),
              'role': (n['hello'] or {}).get('role'), 'api': (n['hello'] or {}).get('api'),
              'alive': n['hello_age'] is not None and n['hello_age'] < 5, 'panel': n.get('panel')}
             for n in z.get('nodes', []) if n.get('id') is not None]
    s = z.get('sensors', {})
    sensors = {k: s.get(k) for k in ('stick_x', 'stick_y', 'prox_raw', 'prox_m')} if (s.get('age') or 99) < 5 else {}
    car = z.get('car', {}) if (z.get('car_age') or 99) < 5 else {}
    lamps = {k: int(bool(car.get(v))) for k, v in (
        ('high', 'body/lights/beam/high/ison'), ('low', 'body/lights/beam/low/ison'),
        ('tail', 'body/lights/parking/ison'), ('left', 'body/lights/directionindicator/left/issignaling'),
        ('right', 'body/lights/directionindicator/right/issignaling'), ('hazard', 'body/lights/hazard/issignaling'),
        ('brake', 'body/lights/brake/isactive'), ('ad', 'private/body/lights/automateddriving/ison'))}
    try:
        post('/api/zone', {'nodes': nodes, 'sensors': sensors, 'lamps': lamps,
                           'bus': {'gateway': 'ESP 존 게이트웨이 .65', 'note': 'PLCA 0/8 · PTP 마스터 · 10BASE-T1S'}})
    except Exception as e:
        if time.time() - last_err > 30:
            log('gateway:', e)
            last_err = time.time()
        continue

    y = sensors.get('stick_y')
    if y is None:
        continue
    if 1000 < y < 3100:
        armed = True
    elif armed and (y >= 3700 or y <= 400):
        armed = False
        try:
            cur = get(a.gateway + '/vss/Vehicle.Private.Autoware.VelocityLimit').get(
                'Vehicle.Private.Autoware.VelocityLimit.External') or 40.0
            want = max(5.0, min(130.0, cur + (a.step if y >= 3700 else -a.step)))
            r = post('/api/1/vehicles/kdrive/command/set_velocity_limit', {'kmh': want})
            log('stick', 'up' if y >= 3700 else 'down', f'{cur:.0f} -> {want:.0f} km/h:', r.get('response', r))
        except Exception as e:
            log('set_velocity_limit:', e)
