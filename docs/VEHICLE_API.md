# Vehicle API v1 — one key space for every component (proposal, 2026-10-28)

Today the bus speaks three dialects: `vehicle/<VSS path>` (the sidecar, the ESP zone controller), `vehicle/private/zone/…`
(zone inputs, panel view, high beam) and `t1s/<board>/…` (node heartbeat, picture, commands). One signal can have two
publishers (the high beam: the sidecar always sends 0, the stick node sends its own key), vehicle signals and device
housekeeping share a namespace, and there is no command API with a result. This is the one structure every component
(zone nodes, ESP gateway, vehicle-API sidecar, console, HMI gateway, Autoware bridge) should speak instead, built on the
signal catalogue that already exists (`elite-t1s-hat/firmware/catalog/*.yaml` + `zcgen.py`).

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
   `vehicle/private/cabin/stick/{x,y}`, the rear obstacle distance `vehicle/private/body/obstacle/rear/distance` (m) —
   not `private/zone/…` and not `private/external/…`. Which zone measured it is ECU-plane information.
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
| `vehicle/private/external/proximity_rear` | `vehicle/private/body/obstacle/rear/distance` | zone |
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
