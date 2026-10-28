# Review request: Vehicle API v1 (`docs/VEHICLE_API.md`)

Please review the proposal as an architect of an automotive zonal E/E architecture (Zenoh over 10BASE-T1S and TSN,
COVESA VSS, Autoware). Repo: this one (zone node firmware, tools, measured traffic in `docs/PACKETS.md`).
Context: zone nodes are SAM E51 + LAN8651 running zenoh-pico 1.10 (client of a zenohd on the bench PC, through an ESP32
gateway that bridges T1S to Ethernet); a vehicle-API sidecar maps Autoware / a simulator onto VSS keys; an HMI gateway
has a Tesla-Fleet-API-like HTTP command API. Measured: a 1-byte signal costs ~93 B on the wire (TCP), TCP acks are
the largest share of bus time, the bus is ~2.4 % loaded with two nodes.

Answer each with a concrete recommendation (and a reference: VSS spec section, Zenoh doc, ISO/SAE standard) where one exists.

1. **The four planes** (signals `vehicle/<vss>`, requests `…/target`, commands `vehicle/cmd/<name>`, ECUs `ecu/<role>/…`):
   is the split right? Would you merge requests and commands, or keep both (VSS actuator semantics vs imperative calls)?
2. **Commands as Zenoh queries.** zenoh-pico's queryable support is compiled out on the nodes to save RAM (SAM E51,
   256 KB; currently ~50 % used). Queries (reply = result) vs put + a reply key (`…/cmd/<name>/result/<id>`)? Cost and
   delivery guarantees of each on a lossy half-duplex segment.
3. **Arbitration.** Several sources may request the same actuator (HMI, Autoware, a cabin stick, a diagnostic tool).
   Where should arbitration live (the owner ECU? a vehicle-level arbiter?), and how should priority / source be
   carried (in the key, the payload, Zenoh attachments)?
4. **Security.** Anyone on the bus can put a target or call a command today. Options: zenohd ACLs per key prefix,
   per-ECU credentials, signed payloads (MACsec-like at Ethernet level for T1S?). What is proportionate for a demo that
   should look production-shaped?
5. **One scalar per key vs bundles.** Given the measured overhead, at what rate / size should signals go in bundles
   (the catalogue has 0x8000+ bundle items for the zone's own UDP frames) instead of one key per scalar? Can the VSS
   key space stay the public face while the wire uses bundles?
6. **VSS paths.** Check the proposed overlay names against VSS 4.x: is there a standard path for the rear obstacle
   distance, a cabin input device (stick), the automated-driving marker lamp, a turn-signal blink phase? Correct the
   mapping table where a standard path exists.
7. **Latest-value reads.** `get vehicle/**` from a zenohd memory storage: how to keep a dead ECU's last value from
   reading as current (timestamps / HLC, `max_age` from the catalogue, liveliness tokens per ECU)?
8. **Naming of ECUs by role** (`zone-left`, `zone-gw`): enough for redundancy (two ECUs for one role, a fail-over),
   multiple vehicles (a fleet prefix?), and versioning (catalogue hash in the heartbeat)?
9. Anything that will not scale from 2 zone nodes on one T1S segment to a car (4 zones, a TSN backbone, ~200 signals).
