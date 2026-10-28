"""(from t1s-zone-bench tools/zenoh_decode.py; values here typed by the Vehicle API key table, tools/vehicle_api.py)
Zenoh on UDP 7447, decoded for the console's traffic panel (a port of elite-t1s-hat tools/zenoh.lua: JOIN, FRAME ->
PUSH/PUT/DEL and DECLARE, key ids from D_KEYEXPR remembered per sender), plus the ZoneLink payloads: one
little-endian scalar per VSS key (docs/ZONELINK.md).   describe(payload, src, dst) -> "PUT vehicle/speed=42.3, ..." """
import struct

N_NAMES = {0x1f: "OAM", 0x1e: "DECLARE", 0x1d: "PUSH", 0x1c: "REQUEST", 0x1b: "RESPONSE", 0x1a: "RESPONSE_FINAL",
           0x19: "INTEREST"}
D_NAMES = {0x00: "D_KEYEXPR", 0x01: "U_KEYEXPR", 0x02: "D_SUBSCRIBER", 0x03: "U_SUBSCRIBER", 0x04: "D_QUERYABLE",
           0x05: "U_QUERYABLE", 0x06: "D_TOKEN", 0x07: "U_TOKEN", 0x1a: "D_FINAL"}
T_NAMES = {0x01: "INIT", 0x02: "OPEN", 0x03: "CLOSE", 0x04: "KEEP_ALIVE", 0x05: "FRAME", 0x06: "FRAGMENT", 0x07: "JOIN"}
GEAR = {126: "P", 127: "D", 0: "N", -1: "R"}
declared = {}          # sender -> {id: key}


def vss_value(key, p):
    """the value by the key's type in the Vehicle API table, else the old ZoneLink guesses"""
    try:
        import vehicle_api as api
        k = api.LEGACY.get(key, key)
        parts = k.split('/')
        if k not in api.KEYS and len(parts) > 2 and parts[0] == 'ecu':
            k = '/'.join(['ecu', '{ecu}'] + parts[2:])
        t = api.KEYS.get(k, {}).get('type')
        if t in api.STRUCT:
            v = api.unpack(k, p)
            return GEAR.get(v, str(v)) if 'selectedgear' in k else ('%.2f' % v if isinstance(v, float) else str(v))
        if t == 'bytes':
            return '%d B' % len(p)
    except Exception:
        pass
    try:
        if key.endswith(("/speed", "/proximity_rear")) and len(p) == 4:
            return "%.2f" % struct.unpack("<f", p)[0]
        if "selectedgear" in key and len(p) == 1:
            g = struct.unpack("<b", p)[0]
            return GEAR.get(g, str(g))
        if key.endswith("/gearflick") and len(p) == 1:
            return chr(p[0])
        if len(p) == 2:
            return str(struct.unpack("<h" if "steeringwheel" in key else "<H", p)[0])
        if len(p) == 1:
            return str(p[0])
    except struct.error:
        pass
    t = p.decode(errors="replace")
    return "'%s'" % t[:24] if t.isprintable() else "%d B" % len(p)


def _zint(b, o):
    v = sh = 0
    for i in range(9):
        x = b[o + i]
        v |= (x & 0x7f) << sh
        if not x & 0x80:
            return v, i + 1
        sh += 7
    return v, 9


def _zbytes(b, o):
    n, l = _zint(b, o)
    if o + l + n > len(b):
        raise IndexError
    return b[o + l:o + l + n], l + n


def _ext(b, o):
    s = o
    while True:
        h = b[o]
        enc = (h >> 5) & 3
        o += 1
        if enc == 1:
            o += _zint(b, o)[1]
        elif enc == 2:
            o += _zbytes(b, o)[1]
        if not h & 0x80:
            return o - s


class _Ids:
    """key ids: the sender's own declarations, then the receiver's (a router publishes with the subscriber's ids)"""
    def __init__(self, own, peer):
        self.own, self.peer = own, peer

    def get(self, i, d=""):
        return self.own.get(i, self.peer.get(i, d))


def _wire(b, o, suffix, tab):
    i, l = _zint(b, o)
    name = ""
    if suffix:
        s, sl = _zbytes(b, o + l)
        name = s.decode(errors="replace")
        l += sl
    full = (tab.get(i, "") if i else "") + name
    return full or "#%d" % i, l, i, name


def _network(b, o, tab, out):
    h = b[o]
    mid = h & 0x1f
    o0, o = o, o + 1
    if mid == 0x1d:                       # PUSH
        key, l, _, _ = _wire(b, o, h & 0x20, tab)
        o += l
        if h & 0x80:
            o += _ext(b, o)
        ph = b[o]
        pm = ph & 0x1f
        o += 1
        if ph & 0x20:                     # timestamp
            _, l = _zint(b, o)
            o += l + 1 + b[o + l]
        if pm == 0x01 and ph & 0x40:      # encoding
            e, l = _zint(b, o)
            o += l
            if e & 1:
                o += _zbytes(b, o)[1]
        if ph & 0x80:
            o += _ext(b, o)
        if pm == 0x01:
            p, l = _zbytes(b, o)
            o += l
            out.append("%s=%s" % (key, vss_value(key, bytes(p))))
        else:
            out.append("DEL " + key)
    elif mid == 0x1e:                     # DECLARE
        if h & 0x20:
            o += _zint(b, o)[1]
        if h & 0x80:
            o += _ext(b, o)
        dh = b[o]
        dm = dh & 0x1f
        p = o + 1
        what = D_NAMES.get(dm, "decl 0x%02x" % dm)
        if dm != 0x1a:
            i, l = _zint(b, p)
            p += l
            if dm in (0x00, 0x02, 0x04, 0x06):
                key, kl, kid, suf = _wire(b, p, dh & 0x20, tab)
                p += kl
                if dm == 0x00:
                    tab.own[i] = (tab.get(kid, "") if kid else "") + suf
                what += " " + key
            if dh & 0x80:
                p += _ext(b, p)
        out.append(what)
        o = p
    else:
        out.append(N_NAMES.get(mid, "N 0x%02x" % mid))
        o = len(b)
    return o - o0


def describe(b, src, dst=None):
    tab = _Ids(declared.setdefault(src, {}), declared.setdefault(dst, {}))
    out, o = [], 0
    try:
        while o < len(b):
            h = b[o]
            mid = h & 0x1f
            o += 1
            if mid == 0x05:                   # FRAME
                o += _zint(b, o)[1]
                if h & 0x80:
                    o += _ext(b, o)
                while o < len(b):
                    o += _network(b, o, tab, out)
            elif mid == 0x04:
                out.append("KEEP_ALIVE")
                if h & 0x80:
                    o += _ext(b, o)
            else:
                out.append(T_NAMES.get(mid, "T 0x%02x" % mid))
                o = len(b)
    except (IndexError, ValueError):
        out.append("(cut)")
    s = ", ".join(out)
    puts = [x for x in out if "=" in x]
    return ("PUT " + ", ".join(puts)) if puts and len(puts) == len(out) else s
