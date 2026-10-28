# Vehicle API v1 — one key space for every component (2026-10-28; reviewed, baseline COVESA VSS v6.1)

Today the bus speaks three dialects: `vehicle/<VSS path>` (the sidecar, the ESP zone controller), `vehicle/private/zone/…`
(zone inputs, panel view, high beam) and `t1s/<board>/…` (node heartbeat, picture, commands). One signal can have two
publishers (the high beam: the sidecar always sends 0, the stick node sends its own key), vehicle signals and device
housekeeping share a namespace, and there is no command API with a result. This is the one structure every component
(zone nodes, ESP gateway, vehicle-API sidecar, console, HMI gateway, Autoware bridge) should speak instead, built on the
signal catalogue that already exists (`elite-t1s-hat/firmware/catalog/*.yaml` + `zcgen.py`).

## Review outcome (2026-10-28)

An outside review agreed with the structure, with three points, all taken:
1. **Keep requests and commands apart**: `…/target` is VSS actuator semantics (desired state, the owner decides and the
   state key confirms), `vehicle/cmd/<name>` is an imperative call with a result. Not merged.
2. **Authentication follows the network**, not one scheme for the whole car: on Ethernet / TSN segments zenohd ACLs per
   key prefix plus TLS or mTLS on the links (MACsec where the switches have it); on a T1S segment, which is a closed
   multidrop wire behind its gateway, the gateway is the enforcement point (it only bridges what its ACL allows onto
   the segment) and nodes are trusted by position. Requests and commands from outside the vehicle always pass an
   authenticated gateway (the HMI gateway's control token today).
3. **Baseline = the current VSS release, v6.1 (2026-09-17)**, checked against its `vss.json` (1737 paths): every
   standard key in use exists unchanged; the rear obstacle distance, DRL and parking lamps have standard paths (below);
   the stick, driving mode, automated-driving marker and blink phase have none and stay in the overlay.

## Four planes, four roots

| plane | key | what | pattern |
|---|---|---|---|
| **signals** | `vehicle/<vss path>` | the vehicle's state, one LE scalar per key (catalogue type) | put / subscribe; latest value by `get` (zenohd memory storage) |
| **requests** | `vehicle/<vss path>/target` | "make this actuator so" (VSS actuator semantics) | put; the **owner** applies it and publishes the state key = the acknowledgement |
| **commands** | `vehicle/cmd/<name>` | imperative actions with a result, the Tesla Fleet API shape (`honk_horn`, `flash_lights`, `set_speed_limit`, `navigation_request`, `autopilot` …) | Zenoh **query** with JSON args → reply `{"result": bool, "reason": str, …}` |
| **ECUs** | `ecu/<ecu>/<topic>` | the devices themselves: `status` (1 Hz heartbeat), `display` (what a panel shows), `diag`, `config`, `cmd`, `fw` | `<ecu>` is the **role** (`zone-left`, `zone-right`, `zone-gw`, `hmi`, `adas`), never a board, bus or MCU name; `ecu/all/<topic>` addresses every ECU |

Rules:
1. **One owner per signal key.** The catalogue names it (`owner:`); nobody else puts the state key. Others ask through
   `…/target` or `vehicle/cmd/…`. (The high beam: owner = the zone that drives the headlamps; the stick puts
   `vehicle/body/lights/beam/high/ison/target`; the sidecar stops sending a value it does not know.)
2. **Standard VSS first.** A key is the lower-case VSS path without `Vehicle.`; only what VSS has no path for goes under
   the overlay `vehicle/private/<vss branch>/…`, named after *what it is*, not *who sends it*: the stick is
   `vehicle/private/cabin/stick/{x,y}` (VSS 6.1 has no input-device branch) — not `private/zone/…` and not
   `private/external/…`; the rear obstacle distance has a standard path, so it uses it. Which zone measured a value is
   ECU-plane information.
3. **Types, units, rates, max age and the QoS class live in the catalogue only** (the five classes of the QoS mapper:
   Zenoh priority → PCP → TSN / PLCA). Every component's key table is generated from it (C header for the nodes,
   Python for the sidecar / console / bridges, the Wireshark table) — no hand-written key strings.
4. **Freshness is part of the contract**: a value older than its `max_age` is stale and the reader goes to the item's
   `safe` state (the catalogue already has both); the heartbeat `ecu/<ecu>/status` carries the catalogue hash, so a
   node built against another catalogue version is visible at once.

## Mapping of today's keys

| today | v1 | owner |
|---|---|---|
| `vehicle/speed`, `…/selectedgear`, `…/steeringwheel/angle`, pedals, `body/lights/…/issignaling`, brake, backup, hazard, doors, horn | unchanged (already VSS) | vehicle (Autoware / sim, via the sidecar) |
| `vehicle/private/drivingmode` | unchanged (no VSS path) | vehicle |
| `vehicle/private/lights/directionindicator/{left,right}/islit` | unchanged (blink phase; overlay) | vehicle; zones may derive it from PTP |
| `vehicle/body/lights/beam/high/ison` (sidecar, always 0) + `vehicle/private/zone/lights/beam/high` (stick node) | `vehicle/body/lights/beam/high/ison` from the **headlamp zone only**; requests on `…/ison/target` | zone (headlamps) |
| `vehicle/private/zone/stick/{x,y}` | `vehicle/private/cabin/stick/{x,y}` | the zone that has the stick |
| `vehicle/private/zone/stick/gearflick` | gone: the flick is a request, `…/selectedgear/target` | — |
| `vehicle/private/zone/proximity/raw` | `ecu/<zone>/diag` (raw counts are diagnostics) | zone |
| `vehicle/private/external/proximity_rear` | **`vehicle/adas/obstacledetection/rear/center/distance`** (VSS 6.1, float, m) + `…/rear/center/iswarning` (bool) | zone with the sensor |
| (zone lamps, new) | **`vehicle/body/lights/running/ison`** (DRL), **`vehicle/body/lights/parking/ison`** (tail), `vehicle/body/lights/beam/low/ison` — VSS 6.1, published by the lamp zone as the state it shows | lamp zone |
| (zone lamps, new) | `vehicle/private/body/lights/automateddriving/ison` (SAE J3134 marker; no VSS path) | lamp zone |
| `vehicle/private/zone/body/isstatelost` | `ecu/<zone>/status` (`safe` flag) | zone |
| `vehicle/private/zone/panel/view` | `ecu/all/config/display_view` | whoever sets it (the stick) |
| `t1s/sam<id>/hello` / `t1s/<node>/hello` | `ecu/<zone>/status` | each ECU |
| `t1s/sam<id>/panel` | `ecu/<zone>/display` | each ECU |
| `t1s/sam<id>/cmd`, `t1s/all/cmd`, `…/ack` | `ecu/<zone>/cmd`, `ecu/all/cmd` as queries (reply = the ack) | each ECU |
| `t1s/<node>/config`, `…/config/ack` | `ecu/<ecu>/config` (query) | each ECU |
| HMI gateway HTTP commands (`/api/1/vehicles/kdrive/command/<name>`) | also `vehicle/cmd/<name>` (query), same names and answers | the HMI gateway / Autoware |
| `vehicle_data` (HTTP / `/vss`) | `get vehicle/**` from zenohd's storage | zenohd |

## Migration (nothing breaks on the way)

1. **Catalogue**: add the Zenoh section (key, type, unit, owner, QoS class, rate, max age, target) for every key above;
   `zcgen.py` writes the key tables (C / Python / docs). zenohd gets a memory storage on `vehicle/**` and `ecu/**`.
2. **Zone nodes, console, HMI bridge**: speak v1 and *also* the old keys for one release (publish both, accept both).
3. **Sidecar**: v1 keys; stops publishing signals it does not own (high beam); serves `vehicle/cmd/*` by forwarding to
   the HMI gateway's command API (results passed back).
4. **ESP zone controller**: v1 keys (firmware build from the generated header).
5. Drop the old keys; the catalogue hash in `ecu/*/status` confirms everyone moved.

## Status (2026-10-28)

| step | state |
|---|---|
| 1. key table | **done**: `api/vehicle_api.yaml` (30 keys: type, unit, owner, QoS class, max age, `target`, VSS 6.1 path, legacy keys) → `tools/apigen.py` → `apps/node/src/vehicle_api.h`, `tools/vehicle_api.py`, [`VEHICLE_API_KEYS.md`](VEHICLE_API_KEYS.md). `--check` fails on a stale file. The table's hash goes into every `ecu/<role>/status` (`"api"`). Still separate from the ESP's signal catalogue (`elite-t1s-hat/firmware/catalog/`); merging the two is step 4. |
| 1. latest values | **done**: `api/zenohd.json5` (memory storages `vehicle/**`, `ecu/**`, REST :8834), started by `tools/bench_up.sh`. Every stored sample carries its HLC timestamp, which the reader compares with `max_age`. |
| 2. zone nodes | **done in v0.13.0**: publish v1 and old keys, accept both; role = the panel half (`zone-left` / `zone-right`); the stick asks for the high beam on `…/beam/high/ison/target`, zone-left owns and publishes the lamp state keys (high / low beam, DRL, parking, J3134 marker) at 1 Hz; rear distance + `iswarning` (< 1.5 m, what the lamps show). |
| 2. console / HMI bridge | **done**: decode by the generated table, old keys mapped to v1 (`LEGACY`); node cards show role and API hash (or "old keys"). |
| 3. sidecar | **started**: no longer publishes or answers the zone-owned lamp keys (korea-autoware-sim `1a6d416`). `vehicle/cmd/*` queries: open. |
| 4. ESP zone controller | open |
| 5. drop old keys | open |
