# Packets on the zone bus — measured

Raw data for the documents: every capture is in `docs/captures/` (`pc-*.pcapng` from the bench PC's USB NIC,
`t1s-*.pcap` from the ESP gateway's `capture`, `sizes-*.md` the tables below as produced). Re-run:
`tools/capture_sizes.py --secs 30` (with `tools/bench_up.sh` up).

## Setup of the first capture (2026-10-28 12:50, 30 s)

- Bus: ESP gateway (PLCA 0/8, `mode node` + `gw on`, IP .65) in the middle, SAM E51 zone nodes sam3 (.73, proximity)
  and sam4 (.74, thumbstick), firmware v0.11.3, both PTP-locked to the gateway.
- Traffic: Autoware driving in CARLA → vehicle-API sidecar (`zonelink.py`) → zenohd (bench PC) → nodes; the nodes
  publish their sensors at 20 Hz, their 10×10 picture at 5 Hz and a heartbeat at 1 Hz; PTP lock 16 exchanges/s each.
- Sizes are Ethernet frames as captured, **without the 4-byte FCS**; on T1S add FCS 4 B + preamble/SFD 8 B per frame.

## What it shows

| Traffic | Payload | Frame (captured) | On the wire | Rate (2 nodes) |
|---|---|---|---|---|
| Control / vehicle signal to a node (VSS key: lamp, gear, speed …) | 1–4 B | **92–93 B** | ~105 B | 95/s |
| Sensor from a node (stick x/y, proximity, u16) | 2 B | **64–65 B** | ~77 B | 85/s |
| Node picture (`t1s/sam<id>/panel`) | 300 B | **364 B** | ~376 B | 10/s |
| Several signals batched by zenohd in one frame | — | 120–1050 B (median 196) | — | 10/s |
| TCP acknowledgement (no data) | 0 | **54–60 B** | ~72 B | **152/s** |
| PTP sync exchange (UDP 5007, request / reply / follow-up) | 32 B | **74 B** | ~86 B | 36/s seen by the gateway |
| ARP | — | 60 B | — | ~1/s |

Total ≈ 30 KB/s ≈ **2.4 % of the 10 Mb/s bus** (PC side) + 0.24 % PTP.

- A one-byte control value costs ~93 B on the wire: Ethernet 14 + IPv4 20 + TCP 20(+12 options) + Zenoh framing ~10–20.
- **TCP acks outnumber the data frames** (152/s vs ~180 data frames/s) and cost more bus time than the signals
  themselves. zenoh-pico over UDP unicast (or batching the node's sensor puts) would remove most of them.
- The heartbeat (JSON, ~150 B frame) is folded into the "node->PC (>60 B)" row with the pictures.

## Not in this capture

- Frames between two SAM nodes, or node→PC frames on the T1S side: the gateway's `capture` records only frames
  addressed to it or broadcast/multicast (its bridge forwards the rest without recording them); they appear on the
  PC side instead.
- The gateway's own transmissions (its PTP replies and follow-ups) — `capture` records received frames only.
