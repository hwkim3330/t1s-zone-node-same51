"""Authenticated commands for the zone nodes' bus shell (firmware >= 0.14, Kconfig APP_BUS_SHELL_AUTH).

    ash <ctr> <tag> <command>    on ecu/<role>/cmd or t1s/sam<id>/cmd
    tag = HMAC-SHA256(key, f"{bn}|{id}|{ctr}|{command}")[:16].hex()

bn is the node's per-boot nonce and id its PLCA id, both from its heartbeat (ecu/<role>/status: "bn", "id");
ctr must grow on that node (the PC's clock in ms). The key is 32 bytes as 64 hex digits in
~/.config/t1s-keys/bus.key ($T1S_BUS_KEY_FILE), the same file the firmware build reads -- never in the repository.
Heartbeat "auth": 0 no bus shell, 1 the old open "sh <cmd>", 2 "ash" with key id "kid" (sha256 of the hex text, 8).

    python3 tools/bus_auth.py <id> <command...>     # one command over the bus, prints the answer
"""
import hashlib
import hmac
import json
import os
import sys
import threading
import time

KEY_FILE = os.environ.get('T1S_BUS_KEY_FILE', os.path.expanduser('~/.config/t1s-keys/bus.key'))
ROUTER = os.environ.get('ZONE_ROUTER', 'tcp/192.168.100.51:7448')
_last = [0]


def key():
    """(key bytes, key id) or (None, None) when there is no key file"""
    try:
        h = open(KEY_FILE).read().strip().lower()
    except OSError:
        return None, None
    return bytes.fromhex(h), hashlib.sha256(h.encode()).hexdigest()[:8]


def counter():
    """milliseconds, strictly growing within this process"""
    _last[0] = max(_last[0] + 1, int(time.time() * 1000))
    return _last[0]


def payload(cmd, hb, k=None):
    """the command payload for a node with heartbeat hb (dict): "ash ..." if it wants it, else "sh ..." """
    if (hb or {}).get('auth') != 2:
        return 'sh ' + cmd
    k = k or key()[0]
    if k is None:
        raise RuntimeError(f'node {hb.get("id")} wants authenticated commands: no key in {KEY_FILE}')
    ctr = counter()
    tag = hmac.new(k, f"{hb['bn']}|{hb['id']}|{ctr}|{cmd}".encode(), hashlib.sha256).digest()[:16].hex()
    return f'ash {ctr} {tag} {cmd}'


def run(session, node_id, cmd, wait=5.0, hb_wait=3.0):
    """one command on node <node_id> over an open zenoh session -> its answer text (without "sh:"), or None.
    Takes the node's heartbeat first (for bn / auth), addresses it by id (t1s/sam<id>/cmd): unique even when two
    nodes claim one role."""
    hb, ev, out = {}, threading.Event(), [None]

    def on_hb(m):
        try:
            h = json.loads(m.payload.to_string())
        except ValueError:
            return
        if h.get('id') == node_id:
            hb.update(h)

    def on_ack(m):
        t = m.payload.to_string()
        if t.startswith('sh:'):
            out[0] = t[3:]
            ev.set()

    s1 = session.declare_subscriber('ecu/*/status', on_hb)
    s2 = session.declare_subscriber(f't1s/sam{node_id}/ack', on_ack)
    try:
        t0 = time.time()
        while not hb and time.time() - t0 < hb_wait:
            time.sleep(0.05)
        if not hb:
            return None
        session.put(f't1s/sam{node_id}/cmd', payload(cmd, hb))
        ev.wait(wait)
        return out[0]
    finally:
        s1.undeclare()
        s2.undeclare()


def open_session():
    import zenoh
    c = zenoh.Config()
    c.insert_json5('mode', '"client"')
    c.insert_json5('connect/endpoints', json.dumps([ROUTER]))
    return zenoh.open(c)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    s = open_session()
    r = run(s, int(sys.argv[1]), ' '.join(sys.argv[2:]), float(os.environ.get('W', '6')))
    print(r if r is not None else '(no answer)')
    s.close()
