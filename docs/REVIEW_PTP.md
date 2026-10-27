# Review request: LAN8651 hardware timestamps and gPTP on Zephyr 3.7

What to review: `zephyr-patches/0001-lan865x-frame-timestamps.patch` (against Zephyr 3.7,
`drivers/ethernet/oa_tc6.[ch]`, `eth_lan865x.c`) and `apps/node/src/ptp.c`.
References: OPEN Alliance 10BASE-T1x MAC-PHY Serial Interface (OA-TC6) v1.1, Microchip LAN8650/1 datasheet
DS60001734F, and the ESP32 driver this was ported from (`elite-t1s-hat/firmware/t1s_node/src/lan865x`, LOCAL PATCH).

Hardware: SAM E51 Curiosity Nano + MIKROE-5543 (LAN8651 B1), SPI 15 MHz, PLCA follower; coordinator is an ESP32 + LAN8651.

## Questions (please answer each with a datasheet / spec section)

1. **Receive stamp position.** With OA_CONFIG0.FTSE=1, FTSS=1, the patch assumes the 8-byte stamp
   (seconds 32 bit BE, nanoseconds 32 bit BE) is prepended to the frame data of the chunk whose footer has
   SV=1 and RTSA=1, and strips it after the whole frame is assembled. Is that the layout? Can the stamp
   straddle a chunk boundary (SWO near the chunk end), and is RTSP worth checking?
2. **Concatenated frames.** When one chunk ends a frame (EV) and starts the next (SV, EBO <= SWO), the patch
   assigns that footer's RTSA to the *next* frame. Correct?
3. **Egress capture.** TSC=01 in the first chunk's data header asks for capture A; the stamp is read from
   TTSCAH/TTSCAL (MMS0 0x10/0x11) after STATUS0.TTSCAA (bit 8), cleared by writing 1. Is capture A the right
   choice when frames are back-to-back, and can TTSCAA be lost if the driver's EXST handler clears STATUS0
   first? (The patch reads the stamp inside `oa_tc6_check_status` before clearing.)
4. **Packet matcher.** MMS4 0x43/0x44/0x45 and 0x53/0x54/0x55 = 0x00FF/0xFFFF/0 with TXMCTL/RXMCTL = 2,
   "match every frame at the SFD". Are these the right registers and is 2 (TXME/RXME) the right value?
   Does matching every frame cost anything besides 8 bytes of SPI per received frame?
5. **TSU discipline.** `rate_adjust` writes TISUBN (fraction in 2^-24 ns: low byte in 31:24, high 16 bits in
   15:0) then TI. `adjust` uses MAC_TA (bit 31 = subtract, 29:0 ns). `get` reads TN then TSL. Are the field
   layouts and the read latch order right? Is the TSH (upper 16 bits of seconds) handling sufficient?
6. **gPTP over PLCA.** Anything specific to 802.1AS on a half-duplex PLCA segment (Pdelay turnaround with
   the transmit opportunity wait, neighborPropDelayThresh 100 us default) that the Zephyr gPTP defaults
   get wrong?
7. **Zephyr integration.** Promiscuous mode is turned on for 01:80:C2:00:00:0E. Would adding that address to
   the LAN8651 hash/specific filter be cleaner, and where? Is waiting for the egress stamp synchronously in
   `lan865x_port_send` (up to 20 ms, polling STATUS0) acceptable, or should it be deferred?

Known open items (not to review): ESP32 side as gPTP grandmaster; ptp4l interoperability over D10/LAN9360.

## Added 2026-10-27 evening (after the first hardware runs)

Measured since: our UDP 5007 lock SAM <-> ESP gateway, hardware stamps on both ends, 15 min soak: offset mean 0.2 ns,
sd 9.6 ns, max 41 ns (in-loop); re-lock within 40 s after the master reboots. Please check these fixes and the new code:

8. **Capture overflow.** After a late frame (left after the reader's 5 ms wait), the next capture set STATUS1.TTSCOFA
   (bit 21) and the chip never captured again; IMASK1 (0x3FFE0003) masks it, so EXST never told the driver. The fix
   writes STATUS1 bits 21..26 and STATUS0.TTSCAA (write-1-to-clear) after every capture attempt (`t1s_take_tx_stamp`,
   and the gPTP path in `lan865x_port_send`). Can clearing at that point ever drop a capture that is still in flight
   for the *next* request? Is unmasking TTSCAA / TTSCOFA (and handling them in the IRQ thread) the better design?
9. **Socket timeouts.** `CONFIG_NET_CONTEXT_RCVTIMEO` is off by default in Zephyr 3.7, which makes `SO_RCVTIMEO`
   silently ineffective (the lock thread blocked for ever on one lost reply). Any other default of that kind a
   UDP/TCP time-sync or zenoh-pico client should expect?
10. **Promiscuous mode.** `net_promisc_mode_on()` queues a copy of every received frame for an application reader;
    with none, the RX packet slab was empty after 25 s. We now set LAN8651 MAC_NCFGR.CAF directly for gPTP's
    01:80:C2:00:00:0E. Is that the intended Zephyr pattern, or should the driver program the multicast hash / a
    specific address filter (MAC_SAB2/SAT2) instead?
11. **ESP gateway** (`elite-t1s-hat` firmware, `bridge.h`, `startGateway` / `gwInput` / `gwGate`): node mode's lwIP
    netif stays on the LAN8651 glue; T1S and W5500 inputs go through a learning bridge that hands frames addressed to
    this board (own MAC, group) to lwIP; lwIP's own frames are redirected in the LAN865x TX gate by source MAC (group /
    unknown also out of the W5500, learned-on-W5500 only there). Look for loops, duplicate delivery, or a frame that
    reaches neither side.
12. **zenoh-pico 1.10 on Zephyr** treats `recv() == 0` (peer closed) as "no data" (`tcp_zephyr.c`), and the built-in
    auto-reconnect went silent after a bus outage; the app now runs its own receive watchdog. Is that a known issue /
    is there a supported way to detect a dead TCP link in zenoh-pico?
