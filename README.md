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
전체 ping 표(PC 포함). 시리얼 포트를 계속 열어 둔다(ESP 는 포트를 열 때마다 리셋되므로). nEDBG 는 DTR 이 서야 UART 를 넘긴다. **ID·IP 는 셸로 바꾼 런타임 값이라 재부팅하면 빌드 기본값(…464 = 1/.71, …469 = 0/.70)으로 돌아간다** —
…469 가 0 으로 부팅하면 ESP 와 코디네이터 둘이 된다. node3/node4 이미지(`build/node3`, `build/node4`)는 빌드돼 있고 굽기만 남았다.
(sudo 없이는 두 보드 중 하나를 고를 수 없음 → USB 하나 뽑고 `openocd … program` 하거나 `flash_one.sh`.)
기존 키트 버스와 합칠 때 빈 ID 는 3·4·6(2 는 LAN8662 를 뺀 경우만); 4대째는 count 를 늘려야 할 수 있다.

### 송신 멈춤 함정

코디네이터 없이(PST=0) 부팅한 노드는 ARP 몇 개를 칩에 쌓은 뒤 `tx` 바이트가 고정된다. 비컨이 나타나도 저절로 풀리지 않았고,
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
