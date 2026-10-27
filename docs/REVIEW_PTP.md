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
