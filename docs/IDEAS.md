# Ideas (not started)

## Traffic lights on T1S (2026-10-28)

A traffic light as one more node on the 10BASE-T1S segment, the way the zone lamps are: a SAM E51 + LAN8651 board
with an LED click per signal head.

- **Sim → light**: the CARLA traffic light the ego is approaching (`ego.get_traffic_light()`, its state and timing)
  published on Zenoh, the node draws red / amber / green (+ arrows) on its panel. PTP-locked like the zones, so a
  group of heads switches on the same edge.
- **Light → car (V2I)**: the other direction is the interesting one: the node itself owns the signal phase and
  publishes it (SPaT-like: state, time to change), and Autoware takes it as a traffic signal instead of (or next to)
  the camera's recognition (`/perception/traffic_light_recognition/traffic_signals`, 10 Hz today). The light then
  is the source of truth and CARLA follows it (set_state on the CARLA actor).
- Town04's centre has the cluster (33 of its 43 lights, CARLA x 150..450, y -450..-150): the `patrol.py --city`
  route already drives through it.
- Vehicle API: infrastructure is not the vehicle, so its own root (e.g. `infra/signal/<id>/state`, `.../timing`)
  rather than `vehicle/`; an `ecu/`-like heartbeat for the node.
- Open: how many heads per node (one panel = one intersection approach?), latency budget vs the camera path,
  what Autoware does when the V2I and the camera disagree.
