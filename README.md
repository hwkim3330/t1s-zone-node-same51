# t1s-zone-node-same51

10BASE-T1S 존 노드 펌웨어 (Zephyr). 보드 조합:

| 부품 | 품번 | 역할 |
|---|---|---|
| SAM E51 Curiosity Nano | EV76S68A (ATSAME51J20A, 120 MHz Cortex-M4F, 1 MB / 256 KB) | MCU + 온보드 nEDBG (CMSIS-DAP, USB `03eb:2175`, CDC 콘솔) |
| Curiosity Nano Base for Click boards | AC164162 | mikroBUS 소켓 3개 |
| Two-Wire ETH Click | MIKROE-5543 | **LAN8651B1** 10BASE-T1S MAC-PHY (SPI, OPEN Alliance TC6) + AT24MAC402 (EUI-48) |

전시 데모(`ioniq6-t1s-demo`)의 front / rear / interior / drive 노드 4개가 모두 이 조합이다.

## 상태 (2026-10-27, 벤치 1대)

| 단계 | 결과 |
|---|---|
| 목표 전압 | nEDBG 측정 **3.32 V** (`pymcuprog getvoltage`). KIT-INFO `STATUS.TXT` 의 "board voltage is too low" 는 캐시된 옛 내용 |
| 보드 정의 | `boards/microchip/same51_cnano` (out-of-tree, HWMv2). XOSC32K → DPLL0 → 120 MHz, 콘솔 SERCOM5 = nEDBG CDC (`/dev/ttyACM0`, 115200) |
| 플래시 | OpenOCD + nEDBG, `west flash` 로 동작. blinky 콘솔·LED 확인 |
| 클릭 위치 | **mikroBUS 슬롯 3** (`apps/probe` 가 세 슬롯을 SPI 로 훑어서 찾음) |
| LAN865x | 드라이버 바인딩, `OA_ID=0x11`, `PHYID=0x0007C1B4`, `DEVID=0x00086512` → **LAN8651 rev B1** |
| MAC | 클릭의 AT24MAC402 (I2C 0x58, reg 0x9A) → `fc:c2:3d:39:e9:b8` |
| PLCA | node id 1 / count 8 적용 확인. **PST=0** (비컨 없음 — 버스에 코디네이터가 없다). `t1s plca 0` 으로 코디네이터를 시키면 PST=1 로 바뀌는 것까지 확인 |
| IPv4 | 192.168.100.71/24 정적. ARP/ICMP 가 드라이버로 나가는 것(tx 카운터)까지 확인, 상대가 없어 ping 응답 없음 |

### 콘솔 발췌

```
=== mikroBUS slot probe, round 0 ===
slot 1: IRQ_N after reset=high OA_ID=0x00000000(-5) ...
slot 2: IRQ_N after reset=high OA_ID=0x00000000(-5) ...
slot 3: IRQ_N after reset=low OA_ID=0x00000011(0) PHYID=0x0007c1b4(0) DEVID=0x00086512(0) STATUS0=0x00000040(0)
slot 3: LAN865x found: chip 0x8651 rev 2 (B1)
RESULT: Two-Wire ETH click (LAN865x) in mikroBUS slot 3
I2C (shared mikroBUS SCL/SDA) scan: 0x30 0x50 0x58
AT24MAC402 @0x58 EUI-48 fc:c2:3d:39:e9:b8
```

```
<inf> net_config: IPv4 address: 192.168.100.71
<inf> zone_node: LAN865x bound: lan865x@2, iface 1
<inf> zone_node: MAC from AT24MAC402: fc:c2:3d:39:e9:b8 (0)
<inf> zone_node: PLCA node 1/8 applied (0)
t1s: chip 8651 rev 2 | PLCA on id 1 count 8 to 32 burst 0/128 | PST=0 (no beacon: coordinator absent or no peer) | BMSR=0805 link=1 | STATUS0=00000000
net: 192.168.100.71 up=1 carrier=1 | bytes rx 0 tx 134 | ipv4 rx 0 tx 4 drop 0 | icmp rx 0 tx 2
```

`link=1` 은 PHY BMSR 의 링크 비트인데 T1S PHY 는 상대 없이도 1 을 낸다. 버스가 실제로 살아 있는지는 **PST**(PLCA status, 0x04CA03 bit 15)로 본다.
드라이버는 캐리어를 항상 켜 두고 패킷 카운터를 세지 않으므로 바이트/IPv4 카운터는 IP 스택 것이다.

## mikroBUS 슬롯 ↔ 핀 (AC164162 위의 EV76S68A)

출처: *SAM E51 Curiosity Nano User Guide* DS70005432B 그림 4-1(핀아웃)·4-2(Curiosity Nano Base 빠른 참조), sec. 4.2(LED0 PA14, SW0 PA15, 둘 다 active-low);
*Curiosity Nano Base for Click boards User Guide* 50002839B.

| mikroBUS | 슬롯 1 | 슬롯 2 | 슬롯 3 | 주변장치 |
|---|---|---|---|---|
| AN | PA02 | PB03 | PB08 | ADC0 AIN0 / AIN15 / AIN2 |
| RST | PA07 | PA06 | PB06 | GPIO |
| CS | PA18 | PB05 | PB04 | GPIO (SPI `cs-gpios` 인덱스 0/1/2) |
| SCK | PA17 | PA17 | PA17 | SERCOM1 PAD1 (공유) |
| MISO | PA19 | PA19 | PA19 | SERCOM1 PAD3 (공유) |
| MOSI | PA16 | PA16 | PA16 | SERCOM1 PAD0 (공유) |
| PWM | PA10 | PA11 | PB09 | TCC0 WO2 / TCC0 WO3 / (TC4 WO1 — Zephyr PWM 드라이버 없음) |
| INT | PA04 | PB14 | PB07 | EIC EXTINT4 / 14 / 7 |
| RX | PB13 | PA09 | PB13 | SERCOM4 PAD1 / SERCOM0 PAD1 / SERCOM4 PAD1 |
| TX | PB12 | PA08 | PB12 | SERCOM4 PAD0 / SERCOM0 PAD0 / SERCOM4 PAD0 |
| SCL | PA13 | PA13 | PA13 | SERCOM2 PAD1 (공유) |
| SDA | PA12 | PA12 | PA12 | SERCOM2 PAD0 (공유) |

주의: **슬롯 1 과 3 은 UART 핀이 같다**(SERCOM4). PWM 은 슬롯 1·2 만 TCC 로 나온다.
기타: LED0 = PA14 (active low), SW0 = PA15 (active low), 콘솔 = PB16 TX / PB17 RX (SERCOM5 → nEDBG CDC).

### 디바이스트리 라벨 (클릭 오버레이용)

- 커넥터 넥서스: `&mikrobus_1`, `&mikrobus_2`, `&mikrobus_3` (`mikro-bus` 바인딩 번호: 0 AN, 1 RST, 2 CS, 3 SCK, 4 MISO, 5 MOSI, 6 PWM, 7 INT, 8 RX, 9 TX, 10 SCL, 11 SDA)
- 버스: `&mikrobus_N_spi` (= `&mikrobus_spi`, SERCOM1, 자식 `reg = <N-1>`), `&mikrobus_N_i2c` (= `&mikrobus_i2c`, SERCOM2, 400 kHz),
  `&mikrobus_1_serial` / `&mikrobus_3_serial` (SERCOM4), `&mikrobus_2_serial` (SERCOM0), `&mikrobus_pwm` (TCC0, ch 2 = 슬롯 1, ch 3 = 슬롯 2)
- UART·PWM 은 기본 `disabled` — 쓰는 클릭의 오버레이에서 켠다. 예 (슬롯 2 PWM, WS2812 데이터 등):

```dts
&mikrobus_pwm {
	status = "okay";
	pinctrl-0 = <&tcc0_mikrobus_2_pwm>;
};
```

T1S 클릭은 보드 파일에 박지 않고 `dts/two-wire-eth-click-slot{1,2,3}.overlay` 중 하나를 고른다 (`-DT1S_SLOT=N`, 기본 3).

## 빌드 / 플래시

```bash
cd ~/zephyrproject && source zephyr/zephyr-env.sh
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-0.16.5
R=/home/kim/t1s-zone-node-same51

# 슬롯 찾기 (클릭을 옮겼거나 새 노드일 때)
nice west build -p -b same51_cnano -d $R/build/probe $R/apps/probe && west flash -d $R/build/probe

# 노드 펌웨어 (클릭이 슬롯 3, PLCA id 1)
nice west build -p -b same51_cnano -d $R/build/node $R/apps/node -- -DT1S_SLOT=3 \
    -DCONFIG_APP_PLCA_NODE_ID=1 -DCONFIG_APP_PLCA_NODE_COUNT=8 \
    -DCONFIG_NET_CONFIG_MY_IPV4_ADDR=\"192.168.100.71\"
west flash -d $R/build/node

# 콘솔
picocom -b 115200 /dev/ttyACM0      # 또는 screen / minicom
```

`west flash` 는 `boards/microchip/same51_cnano/support/openocd.cfg` (cmsis-dap + atsame5x) 를 쓴다. 목표 전압 확인: `pymcuprog getvoltage`.

### 셸 명령

| 명령 | 하는 일 |
|---|---|
| `t1s status` | PLCA(EN, id, count, TO, burst, **PST**), PHY BMSR, OA STATUS0, IP 카운터 |
| `t1s plca <id> [count] [to]` | PLCA 재설정 (칩 리셋 포함). `t1s plca csma` = PLCA 끔. **저장 안 됨** — 재부팅하면 Kconfig 값 |
| `net iface`, `net ping <ip>`, `net arp` | Zephyr 기본 네트워크 셸 |

LED0: 켜짐 고정 = PLCA 비컨 수신(버스 동작), 1 Hz 깜빡임 = 코디네이터 대기.

## 이 보드에서 찾은 함정

1. **I2C 는 400 kHz 여야 한다.** `i2c_sam0` 는 BAUD 를 120 MHz GCLK0 에서 계산하는데 100 kHz 면 BAUD 592 > 255 → init 이 `-ERANGE` 로 실패하고,
   첫 전송이 초기화 안 된 세마포어에서 영원히 멈춘다(시스템 전체가 멈춘 것처럼 보임). 보드 파일은 `I2C_BITRATE_FAST`.
2. 업스트림 `spi_sam0`/`i2c_sam0` 는 SERCOM 코어를 GCLK0(120 MHz)에 물린다. 데이터시트 상 SERCOM 코어 최대는 100 MHz 라 규격 밖이다
   (same54_xpro 도 같음). 실측상 4 MHz·15 MHz SPI 모두 정상 — 추후 GCLK 분주 패치 후보.
3. `STATUS.TXT` 는 캐시된 파일이라 오래된 내용이 보인다 (가이드에도 경고 있음).
4. **보드 2대를 꽂으면 `west flash` 가 죽는다.** Ubuntu openocd 0.12 의 cmsis-dap HID 백엔드는 `adapter serial`/`cmsis_dap_serial` 로
   프로브를 고르면 `*** buffer overflow detected ***` 로 abort 한다(usb_bulk 백엔드는 nEDBG 를 못 찾음). 우회: 다른 보드의 USB 를 잠시
   숨기고 단독으로 굽는다 — `tools/flash_one.sh <serial끝자리> <hex>`.

## 네트워크 펌웨어 업데이트 (MCUboot + mcumgr SMP/UDP)

보드마다 **USB 로 한 번만** MCUboot + 앱을 굽고, 그 뒤로는 T1S 너머로 올린다(USB 불필요):

```
nice west build -p --sysbuild -b same51_cnano -d $R/build/node-v2 $R/apps/node -- -DBOARD_ROOT=$R -DT1S_SLOT=3 \
    -Dnode_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\"0.1.0+1\"
# 처음 한 번(USB, 그 보드만 꽂힌 상태):
openocd -f boards/microchip/same51_cnano/support/openocd.cfg -c init -c 'reset init' \
    -c "program build/node-v2/mcuboot/zephyr/zephyr.hex verify" -c "program build/node-v2/node/zephyr/zephyr.signed.hex verify reset" -c shutdown
# 이후(네트워크, 여러 대는 쉼표로):
tools/smp_udp.py 192.168.100.73,192.168.100.74 update build/node-v2/node/zephyr/zephyr.signed.bin
tools/smp_udp.py 192.168.100.73 list
```

실측(2026-10-27, PC → ESP 브리지 → T1S): 176 KB 3.3 s(≈430–600 kbit/s), 스왑+부팅 포함 ≈17 s. 앱은 네트워크 드라이버가 올라온 뒤
스스로 이미지를 확정한다 → 부팅 못 하는 이미지는 다음 리셋에 MCUboot 가 되돌린다. 서명 키는 MCUboot 개발 키(벤치용).
파티션: boot 64K · slot0/slot1 472K · storage 16K (`same51_cnano.dts`). 버전을 안 올리면 같은 해시라 스왑이 안 일어난다.

## Zenoh (zenoh-pico 1.10.1 클라이언트)

노드는 PC 의 zenohd 에 TCP 클라이언트로 붙는다(ESP 브리지 경유). 라우터는 Kconfig `APP_ZENOH_LOCATOR`(기본 `tcp/192.168.100.51:7448`)
또는 셸 `zenoh <locator>`(즉시 저장). 키:

| 키 | 방향 | 내용 |
|---|---|---|
| `t1s/sam<id>/hello` | 노드→ | 1 Hz `{"id":3,"ip":"192.168.100.73","up":24,"pst":1,"cmds":2}` |
| `t1s/sam<id>/cmd`, `t1s/all/cmd` | →노드 | `led on/off/blink/auto`, `ping` |
| `t1s/sam<id>/ack` | 노드→ | 처리한 명령 / `pong` |

실측(2026-10-27): PC put → ack 수신 **≈3 ms** (zenohd → ESP 브리지 → T1S → SAM → 역방향). 테스트 라우터:
`/opt/zenoh/1.10.1/zenohd --listen tcp/192.168.100.51:7448 --no-multicast-scouting` (시스템 `zenohd.service` 는 EVB 주소 .70 에 묶여 있어
EVB 가 없으면 재시작 루프 — 건드리지 않았다).

필요했던 것(기본값으로는 조용히 실패): `NET_MAX_CONN` 12(기본 4 → "Not enough connection contexts"), 패킷/버퍼 풀 증설("Cannot allocate recv
queue"), POSIX mutex 32 / cond 16(기본 ~5 → zenoh 의 `System error: 12` 후 22 반복), 모듈이 안 만들어 주는 `config.h` 를 `apps/node/zenoh-config/`
에 둠. zenoh 재접속 루프가 버퍼를 잡아먹은 이미지에는 SMP 업로드도 안 들어가 → `kernel reboot cold` 직후 올리면 된다.

## 차량 API → 존 램프 (CARLA/Autoware 연동)

```
Autoware(aw-carla) ─ROS 2─ PLEOS 게이트웨이 :8780 (/io WS, /vss) ─ zonelink.py ─ zenohd ─ ESP 브리지 ─ T1S ─ SAM
python3 ~/korea-autoware-sim/sdv/zonelink.py --api http://10.42.0.90:8780 --router tcp/192.168.100.51:7448
```

`zonelink.py` 는 원래 웹 시뮬용 사이드카지만 PLEOS 게이트웨이가 같은 `/io`·`/vss` 형식을 내므로 그대로 붙는다. 노드는 `vehicle/**` 를 구독:
방향지시등 좌/우, 비상등, 브레이크, 후진, 기어, 속도(생존 신호, 3 s 끊기면 램프 소등). 셸 `lamps`.
**출력 핀(미리 배정, `dts/zone-lamps-slot1.overlay`, `-DLAMP_SLOT=1`, 0 = 없음)**: 좌 AN PA02 · 우 RST PA07 · 브레이크 PWM PA10(TCC0 ch2, 미등 감광 예정) ·
후진 INT PA04, 3.3 V 푸시풀 — LED 는 저항/드라이버 클릭 경유. WS2812 스트립은 나중에 슬롯 1 TX PB12(SERCOM4 SPI 데이터) — MOSI 는 T1S 클릭과
공유라 안 됨, 5 V 스트립은 74AHCT125. 슬롯 2 는 모터/디스플레이용으로 비워 둠(PWM TCC0 ch3).
**깜박임은 1.5 Hz, 위상을 LAN8651 PTP 클록에서** 가져온다 → 클록이 잠긴 노드끼리는 정확히 같이 깜박인다. LED0 도 지시등과 같이 깜박임.
검증(2026-10-27): CARLA 주행 중 Autoware 가 켠 좌 지시등이 노드 `lamps` 에 left 1 로 일치, 키 ~30/s. 게이트웨이의 `hazard_lights` 명령은
"Autoware 미연동"(게이트웨이 쪽 미구현).

## 시간 동기 (LAN8651 하드웨어 타임스탬프)

ESP T1S HAT 펌웨어와 **같은 교환·같은 와이어 포맷**(UDP 5007 `SYNC`/`SYNF`, `ptp on`, `ptp lock <ip> [sec] [kp] [ki]`) — 어느 보드든 마스터가 될 수 있다.
Zephyr 의 LAN865x 드라이버엔 타임스탬프가 없어 **`zephyr-patches/0001-lan865x-frame-timestamps.patch` 를 zephyr 트리에 적용해야 빌드된다**
(`git -C ~/zephyrproject/zephyr apply zephyr-patches/0001-*.patch`): FTSE/FTSS 를 SYNC 와 함께 세움(나중에 쓰면 무시됨), 수신 프레임 앞 8바이트
스탬프 제거 + 훅, 송신 첫 청크 헤더 TSC=A, STATUS0.TTSCAA 를 오류로 찍지 않고 TTSCAH/L 을 읽어 둠. ESP 쪽 드라이버의 LOCAL PATCH 와 같은 설계.

`ptp on` = TSU 증분 40 ns + PHY 패킷 매처를 모든 프레임에(MMS4 0x43–0x55, TXMCTL/RXMCTL) + 드라이버 훅. 응답 서버(5007)는 항상 돈다.

**표준 gPTP (IEEE 802.1AS, Zephyr 내장 스택)**: 같은 패치가 LAN8651 TSU 를 Zephyr PTP 클록 장치(`set/get/adjust/rate_adjust`, TI+TISUBN 2^-24 ns)로
등록하고, 수신 프레임에 `pkt->timestamp`, 0x88F7 프레임·`tx_timestamping` 요청에는 송신 캡처 → `net_if_add_tx_timestamp`, 칩 리셋마다 TSU 증분·패킷 매처
설정, `ETHERNET_PTP` 능력을 붙인다. 앱은 `CONFIG_NET_GPTP` + promiscuous(gPTP 의 01:80:C2:00:00:0E 는 MAC 필터에 없음). 셸 `net gptp`, `net gptp 1`.
gPTP 클록 ID 는 net_init 때 링크 주소에서 만들어지므로 EEPROM MAC 을 `SYS_INIT(POST_KERNEL 85)` 에서 드라이버에 넣는다(안 하면 모든 보드가
DT 자리표시 MAC → 같은 클록 ID). 혼자일 땐 Pdelay 응답이 없어 포트 DISABLED 가 정상.

**실측 — SAM(…464) ↔ ESP esp32-5, LAN8651 하드웨어 스탬프 양끝 (2026-10-27, 우리 UDP 5007 교환, `ptp lock 192.168.100.65 60 0.7 0.3`)**:
경로 지연 **19.801 µs ± 1 ns**(측정 잡음이 수 ns), 38 s 뒤 오프셋 **±125 ns 안**, 마지막 15 s 대부분 **±60 ns**, 결정 −26.6 ppm. 송신 캡처 960/960.
기본 이득 0.2/0.02 는 ESP 의 W5500(잡음 ~50 µs) 벤치용이라 여기선 25 s 에 13 µs 까지만 갔다 → 하드웨어 양끝이면 0.7/0.3.
주의: 이 오프셋은 같은 교환으로 잰 **루프 안 값**이다 — 경로 비대칭(PLCA 대기 등)은 안 보인다. 절대 정확도는 두 보드 PPS 를 스코프로
비교해야 한다(DIOA4, HAT Rev D 의 TP1 / 클릭은 미확인). 첫 시도에서 25 s 뒤 SAM 송신 캡처가 계속 빠졌는데(228 missed), gPTP 경고 로그를 줄인 뒤
재현 안 됨 — 장시간 시험 필요. 측정 동안 ESP 는 node 모드(.65), 끝나고 bridge 로 되돌림.

검증(2026-10-27, 보드 1대): FTSE/FTSS 래치 확인(OA_CONFIG0 0x90e6), 수신 스탬프 제거 후에도 ping·Zenoh 정상. **SAM↔SAM 하드웨어 양끝 측정은
두 번째 보드에 MCUboot 를 올린 뒤** — ESP README 가 "다음 단계"로 적어 둔 바로 그 구성.

## 노드 정체성 (ID/IP)

부팅 시 순서: **저장값**(`t1s save`) → **MAC 표**(`main.c` 의 `bench[]`, 클릭 EEPROM 의 EUI-48) → Kconfig. 그래서 모든 보드가 같은
이미지를 쓴다. 셸: `t1s plca <id> [count]`, `t1s ip <a.b.c.d>`, `t1s save`, `t1s forget`. 비컨이 돌아오면(PST 0→1) PLCA 를 자동 재적용한다
(아래 송신 멈춤 함정의 수정).

## ESP + 2노드 버스 (2026-10-27)

ESP T1S HAT(.65) 가 **PLCA 코디네이터 0/8** 이고 SAM E51 은 리프 노드다(이 보드엔 LAN 포트가 없어 게이트웨이가 될 수 없다).
나중에 Microchip 보드 2대를 더 붙인다.

| 보드 (nEDBG 시리얼) | 클릭 | MAC (AT24MAC402) | PLCA | IP |
|---|---|---|---|---|
| ESP T1S HAT (esp32-5) | — | — | **0 = 코디네이터**, count 8 | 브리지 모드: 없음 (node 모드 땐 .65) |
| …000464 (보통 ttyACM0) | 슬롯 3, LAN8651 B1 | fc:c2:3d:39:e9:b8 | 3 | 192.168.100.73 |
| …000469 (보통 ttyACM1) | 슬롯 3, LAN8651 B1 | fc:c2:3d:3a:0f:32 | 4 | 192.168.100.74 |

ping 셋 다 서로 5/5, 1–2 ms. ESP 는 이제 **bridge 모드**(W5500 ⇄ T1S L2 브리지, ESP 자신은 IP 없음):
벤치 PC USB LAN(192.168.100.51) → ESP RJ45 → T1S → .73/.74 ping 3/3, ≈2.1–2.4 ms. 되돌리기 `mode node` + `save` + `reboot`.

**콘솔**: `python3 tools/console/server.py` → http://localhost:8830 — 토폴로지(라이브 PLCA/비컨/IP), 노드별 PLCA 적용·콘솔 명령,
전체 ping 표(PC 포함). 시리얼 포트를 계속 열어 둔다(ESP 는 포트를 열 때마다 리셋되므로). nEDBG 는 DTR 이 서야 UART 를 넘긴다. ID·IP 는 이제 MAC 표로 정해져 재부팅해도 유지된다.
기존 키트 버스와 합칠 때 빈 ID 는 3·4·6(2 는 LAN8662 를 뺀 경우만); 4대째는 count 를 늘려야 할 수 있다.

### 송신 멈춤 함정

(고침: 펌웨어가 PST 0→1 에서 자동 재적용.) 코디네이터 없이(PST=0) 부팅한 노드는 ARP 몇 개를 칩에 쌓은 뒤 `tx` 바이트가 고정된다. 비컨이 나타나도 저절로 풀리지 않았고,
**`t1s plca <id> <count>` 를 다시 적용하자** 바로 송신이 재개됐다. 펌웨어 수정 후보: PST 0→1 전이 때 PLCA 를 자동 재적용.

## PLCA id 주의 (벤치 버스)

기존 벤치 T1S 버스에는 LAN8661 = 1, LAN8662 = 2, ESP HAT = 4, EVB-LAN8670 = 0(코디네이터, "sound mode pc") 이 이미 있다.
그 버스에 이 노드를 물릴 때 **id 1 은 LAN8661 과 충돌**하므로 `t1s plca 5 8` 처럼 빈 id 를 쓸 것. 전시 계획(ARCHITECTURE.md)의 id 는 front 1, rear 2,
interior 3, drive 0(코디네이터), 게이트웨이 4, count 5.

## 다음 단계

1. **T1S 상대 연결 → ping.** EVB-LAN8670-USB(`evb-lan8670-usb/scripts/t1s-up.sh 0 8 192.168.100.70/24` = 코디네이터) 또는 ESP T1S HAT(`plca 0 8` + `save`)를
   같은 2선에 물리면 PST=1 이 되어야 하고 `net ping 192.168.100.70` 이 돌아야 한다. 버스 양 끝 노드만 클릭의 J1/J2 종단 점퍼를 둔다(기본 장착).
2. **zenoh-pico** (ZoneLink 계획: Zenoh 단일 버스 + VSS 키 + priority→PCP).
   zenoh-pico 는 Zephyr 모듈(`zephyr/module.yml`)을 갖고 있다. `ZEPHYR_EXTRA_MODULES` 로 붙이고, 클라이언트 모드로
   Autoware PC 의 `zenohd`(udp/tcp 7447) 에 접속 → `vehicle/<zone>/...` VSS 키 구독(램프·방향지시등·제동) / 상태 발행.
   T1S 는 10 Mb/s 반이중이라 멀티캐스트 스카우팅보다 라우터 주소를 고정하는 편이 낫다.
3. **ZC 프레임** (`elite-t1s-hat/firmware/catalog`, `zc_core.h`) 을 Zenoh 페이로드로 써서 ESP 노드와 같은 카탈로그로 램프/LED 스트립 제어.
4. **gPTP**: LAN8651 은 HW 타임스탬프를 갖지만 Zephyr 의 이 드라이버는 PTP 를 지원하지 않는다 — ESP HAT 쪽 `ptp gm` 구현을 참고해 드라이버 확장 필요.
5. **Autoware 명령 중계**: Autoware PC 의 `/control/command/*`, `/vehicle/status/*` → zenoh-bridge-ros2dds → zenohd → 이 노드.
6. PLCA id / IP 를 `settings`(NVS, `storage_partition`)에 저장해 노드 4대를 같은 바이너리로.

## Voltage rules for the mikroBUS slots

The slots carry **both 3.3 V and 5 V supply pins**, but **every signal pin is 3.3 V logic**. The ATSAME51 I/O is not
5 V tolerant, so never drive a 5 V signal into it.

- **WS2812B strips (5 V):** a "high" needs 0.7 x VDD = 3.5 V. 3.3 V data is out of spec, so put a 74AHCT125 (or similar)
  level shifter in the data line. Power the strip from its own 5 V supply, not the slot's 5 V pin (the base is
  USB-powered, a few hundred mA at most), and share GND only.
- **Servos / ESC:** 3.3 V PWM is normally accepted (threshold about 2 V). Power them from a separate 5-6 V supply,
  because stall and inrush current on the board's rail can reset the node. Hardware PWM is available on slots 1 and 2
  only (slot 3's PWM pin has no TCC).
- **Displays / sensors:** 3.3 V parts connect directly. 5 V outputs need a divider or a level shifter.
