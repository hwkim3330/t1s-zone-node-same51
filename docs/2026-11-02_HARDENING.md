# 2026-11-02 하드닝 — 코드 리뷰 6건 (브랜치 `hardening`)

코드 리뷰가 지적한 여섯 가지를 고치고 잰 기록. 실험은 전부 예비 보드 **5번(…486, 192.168.100.75, fc:c2:3d:3a:0c:d2)** 에서만 했다.
3·4번(.73/.74)은 두 PC CARLA/Autoware 데모가 도는 중이라 마지막에 OTA 로만 올린다(맨 아래 "올린 기록").
수치는 이 날 벤치에서 잰 것만 적었다. 재지 못한 것은 "미측정"으로 둔다.

| # | 지적 | 한 일 | 결과 |
|---|---|---|---|
| 1 | 버스 셸·SMP 무인증, MCUboot 개발 키 | HMAC 인증 셸(`ash`) + 허용 목록 + 끄는 Kconfig, SMP/UDP 출발지·arming 필터, 자체 서명 키를 저장소 밖에 | 시험 14개 중 13개 기대대로, 1개(출발지 위조)는 PC NAT 때문에 다른 방법으로 확인. 키 교체는 SWD 필요라 **아직 어느 보드에도 안 들어감** |
| 2 | ID 3·5 가 둘 다 zone-left | 역할을 MAC 표에 명시(5 = `zone-rear`), `zone role` 로 바꾸고 저장, 충돌 감지, 콘솔은 id 로 주소 지정 | 5번 = zone-rear 로 동작 확인, 기존 키 소비자 영향 없음 |
| 3 | 표준 PTP 상호운용 | 평가 + IEEE 1588(UDP) 스택을 5번에 올려 봄 | **실패**: 그 이미지가 5번을 버스에서 떨어뜨림(아래). 막힌 점 정리 |
| 4 | WS2812 비트뱅이 인터럽트를 프레임당 ~3 ms 막음 | SERCOM0 PAD3(PA11) SPI + DMA, `panel spitest` 로 파형 자가검증 | IRQ 지연 최대 **3.21 ms → 30 µs**, ≥0.5 ms 인 경우 3640/74665 → 0/74727 |
| 5 | 이더넷 드라이버만 뜨면 바로 confirm | 건강 검사(PLCA·Zenoh·첫 하트비트·SMP 도달) 후 confirm, 180 s 안에 안 되면 미확정 재부팅 | 일부러 망가뜨린 이미지 → **202.8 s 뒤 이전 이미지로 자동 복귀** 실측. 부팅 중 멈추는 이미지는 못 잡던 구멍을 찾아 고침 |
| 6 | zenoh-pico 경로가 이 PC 에 묶임 | `west.yml`(Zephyr f5e05c84 + zenoh-pico 1.10.1 e1ab223a + zcbor 9164bd1), 저장소를 Zephyr 모듈로 | 새 워크스페이스에서 빌드 성공(294208 B, 지금 이미지보다 128 B 큼) |

---

## 1. 원격 셸 · 펌웨어 업데이트 보안

### 위협 (전)
버스(T1S + ESP 브리지 너머 벤치 LAN)에 붙은 누구나:
- `t1s/sam<id>/cmd` 에 `sh t1s reg 10001 0`, `sh t1s plca 0 8`(두 번째 코디네이터), `sh zenoh tcp/x:7447`(라우터 바꾸고 저장), `sh t1s forget` …
- UDP 1337 로 slot1 지우기·임의 이미지 올리기·test 표시·리셋(이미지는 MCUboot 가 서명 검사하지만 **키가 MCUboot 공개 개발 키**라 누구나 서명 가능).

### 버스 셸 (zone_zenoh.c)
- `ash <ctr> <tag> <명령>` — `tag` = HMAC-SHA256(키, `"<bn>|<id>|<ctr>|<명령>"`) 앞 16바이트 hex.
  `bn` = 노드가 부팅마다 TRNG 로 만든 32비트 nonce(하트비트 `"bn"`), `id` = PLCA id, `ctr` = 그 노드에서 계속 커지는 10진 카운터(PC 의 ms 시각).
  → 엿들은 명령은 **다시도(ctr), 다른 노드에도(id), 재부팅 뒤에도(bn)** 안 통한다. 비교는 상수 시간.
- 키: 빌드 때 `~/.config/t1s-keys/bus.key`(64 hex, `$T1S_BUS_KEY_FILE` / `-Dnode_BUS_KEY_FILE=`) → `build/…/bus_key/bus_key.h`. **저장소엔 없다.**
  키 파일 없이 빌드하면 모든 명령 거부(fail closed). 하트비트 `"kid"` = 키 지문(키 hex 문자열의 SHA-256 앞 8자리, 지금 `3e76aca3`).
- `APP_BUS_SHELL_ALLOW`(기본): `t1s status;t1s diag;t1s sleep;t1s wake;panel;ptp$;zenoh$;zone;ota;irqlat;kernel uptime$;kernel stacks$;kernel thread stacks$;net iface$;net ping`.
  빠진 것: 레지스터 쓰기(`t1s reg`), 신원 변경(`t1s plca/ip/save/forget`), 라우터 변경(`zenoh <loc>`), `ptp lock`, 재부팅. `$` = 정확히 그 명령만.
- `ecu/all/cmd` 로 온 셸 명령은 거부(id 에 묶인 명령이라). `led`/`ping` 은 그대로(무해).
- Kconfig: `APP_BUS_SHELL`(n = 셸 자체 없음, `release.conf`), `APP_BUS_SHELL_AUTH`(n = 옛 `sh <cmd>`, 벤치 전용).
- PC 쪽: `tools/bus_auth.py`(서명 + `python3 tools/bus_auth.py <id> <명령>`), 콘솔 `zone.sh()` 와 `smp_udp.py` 가 사용. 하트비트 `"auth"` 0/1/2 로 옛 펌웨어엔 `sh` 를 보낸다.

### SMP/UDP 1337 (zone_guard.c)
Zephyr 패킷 필터의 IPv4 입력 훅에서 SMP 서버보다 먼저 거른다(Zephyr 패치 없음):
- 출발지 `APP_SMP_ALLOWED_SRC`(기본 192.168.100.51) 만, 그리고
- **arming 된 동안만**(`ota arm [s]`, 기본 300 s, 인증 셸 또는 USB 콘솔). 부팅하면 닫힘. `ota close`, `ota src <ip>|any`(재부팅까지).
- `tools/smp_udp.py` 가 노드마다 먼저 Zenoh 로 `ota arm 600` 을 보낸다(하트비트에 `"ota"` 가 없는 옛 펌웨어는 건너뜀). 업데이트 뒤 목록 조회 전에도 다시 연다.
- `release.conf` 는 SMP 전송 자체를 뺀다(그땐 SWD 로만 업데이트).

### 시험 (5번, 0.14.0, `/tmp` 의 sectest.py)
| # | 보낸 것 | 응답 |
|---|---|---|
| 1 | 인증 없는 `sh t1s status` | `sh:(denied: authentication required (ash))` |
| 2 | tag 한 자리 바꿈 | `denied: bad tag` |
| 3 | 올바른 `ash … t1s status` | 상태 출력 |
| 4 | 3 을 그대로 다시(재전송) | `denied: stale counter (replay?)` |
| 5 | `t1s plca 1 8`, `zenoh tcp/1.2.3.4:7447` (서명은 정상) | `denied: not on the allow list` |
| 6 | 올바른 명령을 `ecu/all/cmd` 로 | `denied: not on ecu/all` |
| 7 | 다른 id(6) 로 서명 | `denied: bad tag` |
| 8 | 옛 boot nonce 로 서명 | `denied: bad tag` |
| 9–10 | `ota close` 뒤 SMP 요청 | 응답 없음(필터가 버림, `dropped: closed` 증가) |
| 11–12 | `ota arm 120` 뒤 SMP 요청 | 응답 |
| 13 | 다른 출발지(10.42.0.1 에 bind) | 응답 — **시험 실패가 아니라 PC 가 NAT**: NM 공유 연결의 MASQUERADE 가 출발지를 .51 로 바꿈(dumpcap 으로 확인). raw 소켓(root) 없이는 위조 출발지를 못 만든다 |
| 13' | 대신 `ota src 192.168.100.99` 로 허용 출발지를 바꾸고 .51 에서 요청 | 응답 없음, `dropped: source 1` → 출발지 검사 동작 |

### MCUboot 서명 키
- 키 생성: `tools/keys.sh` → `~/.config/t1s-keys/mcuboot-rsa-2048.pem`(700/600, 저장소 밖; `.gitignore` 에 `*.pem *.key t1s-keys/ bus_key.h`).
  종류는 지금과 같은 RSA-2048(MCUboot 설정 변경 없음).
- 빌드 연결: `apps/node/Kconfig.sysbuild` 가 `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` 기본값을 환경변수 **`T1S_SIGNING_KEY`** 로 바꾼다.
  `tools/build_node.sh <build dir> <version>` 은 키 파일이 있으면 그걸 쓰고, `T1S_SIGNING_KEY=dev` 면 개발 키.
- 실측: 프로젝트 키로 서명한 0.14.0 을 **개발 키 MCUboot** 인 5번에 OTA → 업로드·test 표시·리셋까지 되고 MCUboot 가 서명 불일치로 거부,
  6.7 s 뒤 그대로 0.13.11 로 부팅, slot1 은 지워짐. (키 교체 후엔 반대로 개발 키 이미지가 이렇게 거부된다.)
- **노드별 MCUboot 키 (2026-11-02 현재): 3 = 개발 키, 4 = 개발 키, 5 = 개발 키.** 공개키는 MCUboot 안에 컴파일돼 있어 OTA 로는 못 바꾸고
  SWD(USB nEDBG) 로 MCUboot 를 다시 구워야 한다. 5번은 오늘 USB 에 없었다(`/sys/bus/usb` 에 Microchip 장치 없음, 콘솔의 "USB …460" 은 예전 기록)
  → **어느 보드도 아직 바꾸지 못했다.** 3·4번은 지시대로 개발 키에 둔다. 그래서 3·4·5번용 OTA 이미지는 당분간 `T1S_SIGNING_KEY=dev` 로 만든다.
- 바꾸는 절차(한 보드만 USB 에 꽂고 — 두 대면 openocd 가 죽는다, README 함정 4):
  ```
  tools/build_node.sh build/node-prod 0.14.3+0            # 프로젝트 키(기본)
  openocd -f boards/microchip/same51_cnano/support/openocd.cfg -c init -c 'reset init' \
      -c "program build/node-prod/mcuboot/zephyr/zephyr.hex verify" \
      -c "program build/node-prod/node/zephyr/zephyr.signed.hex verify reset" -c shutdown
  ```
  이후 그 보드엔 프로젝트 키 이미지만 들어간다(개발 키 이미지는 위 실측처럼 거부). slot1 의 옛 이미지는 개발 키라 롤백 대상이 못 된다 →
  바꾼 직후 같은 이미지를 OTA 로 한 번 더 올려 slot1 도 프로젝트 키로 채운다. 어떤 보드가 어느 키인지는 이 문서 표를 고친다.

### 남은 위험 (안 한 것)
- Zenoh 평면 자체(`vehicle/**` put)는 여전히 무인증 — zenohd ACL / 게이트웨이 몫(`docs/VEHICLE_API.md` 리뷰 2번). UDP 5007 시간 동기도 무인증.
- 버스 키 하나를 모든 노드가 공유(노드별 키는 나중에), 기밀성 없음(명령은 평문), PC 의 키 파일은 평문.
- MCUboot 다운그레이드 방지 없음(서명된 옛 이미지는 다시 올릴 수 있다).

## 2. 역할 충돌 (4존)

- 전: `zone_role()` = 저장된 `panel half` 또는 PLCA id 홀짝 → 3·5 둘 다 `zone-left`. 실측: 두 노드가 `ecu/zone-left/status` 를 번갈아 내서
  콘솔의 역할→id 표가 흔들리고, 콘솔에서 5번 버튼(`POST /api/zone/sh {"id":5}`)이 "응답 없음". 5번은 zone-left 몫인 램프 상태 키
  (`vehicle/body/lights/beam/*` 등)까지 같이 냈다(소유자 둘).
- 후: 역할 = **저장값(`zone role <name>`) > MAC 표(`main.c bench[]`, 3 zone-left · 4 zone-right · 5 zone-rear) > Kconfig `APP_ROLE` > 옛 규칙**.
  `zone role <name>|auto` 는 저장하고 Zenoh 키를 다시 선언한다(재부팅 불필요, 인증 셸로도 됨). 패널 절반 기본값은 역할에서(zone-left 왼쪽,
  zone-right 오른쪽, 그 밖은 홀짝).
- 충돌 감지: 노드가 자기 `ecu/<role>/status` 를 구독한다(zenohd 는 자기 put 을 되돌려주지 않으니 들리면 남이다) → 로그 + 하트비트 `"conflict":<id>`,
  콘솔 카드에 "역할 충돌".
- id 로 주소: 콘솔 `zone.sh()` 는 이제 `t1s/sam<id>/cmd` 로 보내고 `t1s/*/ack` 에서 받는다(옛 키지만 id 가 유일). `ecu/<role>/…` 키 이름은 그대로.
- 기존 소비자 확인: `tools/console`(ecu/*/status 의 `role` 표시, 이제 id 기준), `tools/zone_to_hmi.py`(role 문자열을 HMI 에 전달만),
  `korea-autoware-sim/sdv/zonelink.py`(`ecu/ads/status` 를 낼 뿐 ecu/<role> 를 읽지 않음). pleos-niro-hmi, sdv-ivn-platform, esp32-t1s-bridge,
  elite-t1s-hat, t1s-zone-bench, pleos 를 grep — `zone-left/right`·`t1s/sam` 소비자 없음. 3·4번 키는 바뀌지 않으니 데모 영향 없음.

## 3. 표준 PTP 상호운용

### 지금 것
`ptp.c` 의 UDP 5007 교환은 1588 의 delay request-response 와 같은 꼴(양끝 LAN8651 하드웨어 스탬프, 두 단계)이지만 메시지 형식이 자체다.
벤치 PC 에서 본 오늘의 값: 3·4·5번 모두 σ 9–13 ns, 최대 30–75 ns(하트비트 `ptp_ns`, 루프 안 값).

### 표준으로 가는 길과 막힌 곳
1. **802.1AS(gPTP, Zephyr 내장)는 버스 구조와 안 맞는다.** 802.1AS-2020 의 매체 종속 계층은 full-duplex 점대점 링크만 정의한다.
   10BASE-T1S PLCA 는 반이중 멀티드롭이라 Pdelay_Req 하나에 응답이 여럿 오고, Zephyr gPTP 는 표준대로 그 포트를 asCapable=false 로 두고
   세 번 연속이면 5분 쉰다(`gptp_md.c` `gptp_md_pdelay_check_multiple_resp`). 버스에 gPTP 노드가 셋 이상이면 동기가 서지 않는다.
   ESP 쪽 `ptp gm`(elite-t1s-hat)도 같은 이유로 경로 지연 없이 Sync 만 따른다. 이 저장소의 `gptp.conf` 는 노드 둘 사이 실험용으로만 의미가 있다.
2. **IEEE 1588(UDP, E2E)** 은 멀티드롭에 맞는 쪽이다(마스터가 Sync/Follow_Up 멀티캐스트, 각 노드가 Delay_Req). Zephyr 3.7 에 실험 스택
   (`subsys/net/lib/ptp`, 보통 시계·UDP IPv4·E2E)이 있고 LAN8651 패치가 하드웨어 스탬프(`SO_TIMESTAMPING`, `tx_timestamping`)를 준다.
   → `apps/node/ptp1588.conf`(도메인 77: 3.7 엔 도메인 Kconfig 가 없어 `ptp.c` 에서 바꿈 — 벤치 LAN 의 다른 PTP 슬레이브가 따라가지 않게).
3. **상대가 없다.** 표준 구현과 맞춰 보려면 버스 위나 PC 에 1588 상대가 있어야 하는데:
   - PC 의 ptp4l: 이더넷(igc)은 하드웨어 스탬프를 지원하지만 `/dev/ptp0` 은 root 전용, UDP 319/320 은 특권 포트(`ip_unprivileged_port_start=1024`),
     L2 는 raw 소켓 — **sudo 없이 불가.**
   - 경로: PC → D10 스위치 → ESP W5500 브리지(PTP 를 모르는 브리지, 지연 흔들림 수십 µs: README 의 W5500 잡음) → T1S. 하드웨어 수준 상호운용은
     상대가 T1S 버스 위에 있어야 의미가 있다(LAN8651 이 달린 다른 구현, 또는 1588 을 아는 게이트웨이).
   - D10 의 PTP 를 켜거나 ESP 게이트웨이를 바꾸는 건 데모 중이라 하지 않았다. 오늘 벤치 LAN 엔 PTP 프레임이 없었다(dumpcap 8 s, 0x88F7·UDP 319/320 0개).
4. **시도 결과 (5번, 0.14.2+77 = 0.14.2 + ptp1588.conf, RAM 85 %)**: OTA 직후부터 **5번이 버스에서 아무 프레임도 내지 않음**(콘솔 캡처 340 s+,
   ping 무응답). 건강 검사의 180 s 미확정 재부팅도, 워치독 리셋도 일어나지 않았다 → 부팅 도중 `main()` 에 닿기 전에 멈춘 것.
   1588 스택은 APPLICATION 96 에서 **협조형 우선순위 1** 스레드를 띄우는데, 그게 막히지 않고 돌면 그 아래 전부가 굶는다(원인 미확인: USB 콘솔 없음).
   그때 워치독은 `main()` 에서 켜졌기 때문에 아무도 리셋하지 않았고, MCUboot 는 다음 리셋이 와야 test 이미지를 되돌린다.
   → **5번은 전원을 한 번 뺐다 꽂아야 돌아온다**(그러면 MCUboot 가 확정 안 된 0.14.2+77 을 버리고 0.14.2 로 부팅 — 예상, 미확인).
   이 사고로 찾은 구멍은 5절에서 막았다(워치독을 POST_KERNEL 에서 켬).
5. 다음에 할 수 있는 것: 5번을 USB 콘솔이 보이는 상태에서 1588 이미지를 다시 띄워 멈춘 곳을 본다(`PTP_LOG_LEVEL_DBG`, 스택 크기, RX 풀);
   1588 상대는 ESP HAT 펌웨어에 1588 UDP 마스터를 넣거나(LAN8651 스탬프 이미 있음) root 가 되는 PC 에서 `ptp4l -4 -E -H`. UDP 5007 잠금은 그대로 둔다.

## 4. WS2812: 비트뱅 → SPI + DMA

### 핀
슬롯 2 의 PWM = **PA11**. 기능 C = **SERCOM0 PAD3**(D = SERCOM2 PAD3 이지만 SERCOM2 는 공유 I2C). SPI 출력은 PAD0 또는 PAD3 만 가능 →
**DOPO=2**(DO = PAD3, SCK = PAD1 = PA09: 핀으로 안 내도 SPI 는 돈다). SERCOM0 은 원래 슬롯 2 UART(PA08/PA09) 인데 패널 클릭은 안 쓴다.
DMA: SERCOM0 TX 트리거 0x05 / RX 0x04, 채널 0/1. Zephyr 3.7 의 spi_sam0 는 `SPI_ASYNC` 를 켜면 **모든** SPI 인스턴스에 DMA 를 요구해서
(`DEVICE_DT_GET` 무조건) 공유 SPI(SERCOM1, LAN8651)에도 채널 2/3 을 적었다 — 그쪽 사용자는 동기 API 라 안 쓴다. `PANEL_SLOT=2` 일 때만 `panel-spi.conf`.

### 부호화와 함정
6.667 MHz(120 MHz / 18, 정확), LED 비트 하나 = SPI 바이트 하나: 0 = `0xC0`(300 ns high), 1 = `0xF8`(750 ns high), 비트 1.2 µs — 비트뱅의 300/800/1250 ns 와 같은 꼴.
**함정**: SERCOM 의 DO 는 전송이 없을 때 **HIGH** 로 쉰다(`panel spitest` 로 측정: idle after 1). WS2812 선이 높게 쉬면 래치가 안 되고 다음 프레임의
첫 에지를 비트로 읽는다. → 프레임 사이엔 핀을 PORT 출력(Low)으로 두고, 프레임 앞 0x00 16바이트(19 µs) 안에서 SERCOM 에 넘기고(인터럽트 3 µs 잠금),
DMA 완료 인터럽트에서 되찾는다(마지막 LED 데이터 뒤 수 µs 의 high 는 아무 LED 에도 안 간다 — 다들 24비트를 이미 받았다).

### 자가검증 `panel spitest`
알려진 12바이트를 SPI 로 내면서 CPU 가 PORT IN 과 사이클 카운터로 핀을 직접 샘플(~75 ns 해상도)해 high 길이를 재고 비트로 되읽는다.
스코프·LED 없이(5번엔 패널이 있는지 모른다) 파형을 증명한다. 0.14.2 결과(3회 연속):
`96 bits decoded, 0 wrong | 0: high 216..366 ns | 1: high 666..816 ns | bit period 1183..1333 ns | idle before 0 after 0 -> PASS`.
(첫 시도의 "40 wrong"·"bit period ..2383" 은 패널 스레드와 버퍼를 같이 쓴 경쟁, 그리고 끝의 0x00 + idle-high 까지 잰 측정 실수 — 고침.)

### 측정 (5번, 같은 이미지 0.14.2 에서 `panel drv bb|spi|off` 만 바꿔, 각 10/10/5 분)
IRQ 지연 = TC4 를 GCLK0/16(133 ns)로 8 ms 마다 EIC 와 같은 우선순위로 인터럽트, ISR 에서 매치 뒤 지난 카운트(`irqlat`).
PTP = 하트비트 `ptp_ns`(1 Hz). ping = PC 에서 50 ms 간격.

| 구동 | IRQ 지연 평균 / 최대 | ≥ 0.5 ms 인 표본 | PTP σ / 최대 | ping 손실 · 평균 / 최대 / mdev | LAN8651 RX/TX 오류 |
|---|---|---|---|---|---|
| 비트뱅 | 92.8 µs / **3214 µs** | 3640 / 74665 (≥2 ms 1608) | 11.8 / 47 ns | 0/11791 · 2.598 / 9.70 / 0.695 ms | 없음 |
| **SPI+DMA** | 0.58 µs / **30 µs** | 0 / 74727 | 12.4 / 69 ns | 3/11795 · 2.507 / 10.70 / 0.558 ms | 없음 |
| 끔 | 0.56 µs / 15.6 µs | 0 / 37247 | 10.9 / 45 ns | 0/5848 · 2.506 / 9.34 / 0.536 ms | 없음 |

읽기: 인터럽트 지연은 비트뱅의 프레임 길이(3 ms) 그대로였고 SPI+DMA 로 사라졌다(남은 30 µs 는 프레임 시작의 3 µs 잠금과 그 밖의 커널 구간).
PTP 는 양끝 하드웨어 스탬프라 셋이 차이 없다(σ 11–12 ns 는 이 벤치의 평소 값). ping 평균·흔들림은 SPI 쪽이 비트뱅보다 조금 낫고(2.598→2.507 ms,
mdev 0.695→0.558) 끔과 같다. SPI 단계의 ping 3개 손실은 ESP 브리지 쪽 손실과 구분하지 못했다(유의성 미확인). `t1s diag` 의 STATUS0/1 오류 비트·TX 중단은
세 단계 모두 0 — 이 부하(47 프레임/s)에선 비트뱅도 LAN8651 수신 버퍼를 넘치게 하진 않았다.
`panel drv bb` 로 언제든 되돌릴 수 있다(저장됨). 실제 LED 위에서의 확인은 3·4번에 올린 뒤 `panel spitest` 와 사람 눈으로(아래).

## 5. OTA 확정을 건강 검사 뒤로

- 전: `main()` 이 이더넷 드라이버 직후 `boot_write_img_confirmed()`. 네트워크·Zenoh 가 안 되는 이미지도 확정돼 그 보드는 SWD 로만 살릴 수 있었다.
- 후(main.c `health_check`): MCUboot 가 **test 로 부팅한 이미지**(`mcuboot_swap_type() == REVERT`)는 다음이 다 될 때만 확정:
  PLCA 비컨, Zenoh 연결 + 첫 하트비트 송신, **필터를 통과한 SMP 요청 하나**(`APP_CONFIRM_NEED_SMP`: 다시 업데이트할 수 있어야 확정 — 버스 키·arming·필터가
  망가진 이미지는 스스로 돌아간다), 선택으로 차량 키 하나(`APP_CONFIRM_NEED_VEHICLE`, 기본 꺼짐: 업데이트 중 차가 꺼져 있을 수 있다).
  `APP_CONFIRM_TIMEOUT_S`(180) 안에 안 되면 로그 후 미확정 재부팅 → MCUboot 가 이전 이미지로. SWD 로 구운 이미지(test 아님)는 예전처럼 바로 확정.
  `smp_udp.py update` 는 리셋 뒤 노드를 arm 하고 슬롯을 조회하므로 정상 업데이트는 그 조회로 확정된다(실측 0.14.0/1/2: 리셋 후 ~20 s 안에 confirmed).
- 하트비트 `"conf"`(확정 여부), 콘솔 카드 "(시험 부팅, 미확정)", 셸 `zone` / `ota` 가 기다리는 조건을 보여준다.
- **복귀 실측** (5번): 라우터 주소를 일부러 틀린(`tcp/192.168.100.51:9`) 0.14.2+99 를 올림 → ping 은 되고 Zenoh 하트비트 없음 →
  마지막 하트비트에서 **202.8 s 뒤** 0.14.2 로 하트비트 복귀(리셋 원인 0x40 소프트웨어, `conf 1`), 슬롯: slot0 0.14.2 active confirmed, slot1 0.14.2.99(미확정).
- **구멍과 수정**: 3절의 1588 이미지는 `main()` 전에 멈춰 건강 검사도 워치독도 못 돌았다(워치독을 main 에서 켰으므로). 0.14.3 부터 워치독을 POST_KERNEL 99
  (`SYS_INIT`, APPLICATION 초기화·main 보다 먼저)에서 켠다: 부팅 중 멈추거나 CPU 를 굶기는 이미지, 치명 오류로 멈춘 이미지도 8 s 뒤 리셋 → MCUboot 복귀.
  **0.14.3 은 아직 아무 보드에서도 안 돌았다**(5번이 멈춰 있어서).

## 6. zenoh-pico 고정 (West)

- `west.yml`(저장소 루트): Zephyr v3.7-branch **f5e05c84**(이 PC 의 트리, VERSION 3.7.3-rc1) import 는 쓰는 모듈만(cmsis, hal_atmel, mcuboot, mbedtls,
  tinycrypt, picolibc), **zcbor 9164bd1**(이 PC 의 체크아웃 = 0.9.1+2; Zephyr 매니페스트가 부르는 75d0880 이 아님 — 실제 배포 이미지와 같게),
  **zenoh-pico e1ab223a = 태그 1.10.1**(`modules/lib/zenoh-pico`).
- `zephyr/module.yml`: 저장소가 Zephyr 모듈(`board_root: .`) → 그 워크스페이스에선 `-DBOARD_ROOT` 없이 sysbuild 가 보드를 찾는다.
- CMake: west 프로젝트 `modules/lib/zenoh-pico` 가 있으면 그걸, 없으면 `ZENOH_PICO_DIR`(기본 예전 경로) — 이 PC 의 기존 `~/zephyrproject` 빌드는 그대로.
- ESP 쪽 vendored 트리는 1.10.1 + raw-Ethernet `#if` 가드 4곳(ESP-IDF 용). Zephyr 빌드엔 필요 없다: 패치 없는 1.10.1 로 앱 빌드 성공.
- 검증: 스크래치에 `west init -l` + `west update`(로컬 미러로 받음) → 패치 적용 → `west build --sysbuild` 성공, 앱 294208 B(기존 294080 B, raw-eth 코드 128 B), MCUboot 29126 B(같음).
  그 워크스페이스로 만든 이미지는 아직 보드에서 안 돌렸다.

---

## 올린 기록 (2026-11-02)

| 시각 | 보드 | 이미지 (서명 키) | 방법 | 결과 |
|---|---|---|---|---|
| 14:40 | 5 | 0.14.0 (프로젝트 키) | OTA | MCUboot 가 서명 거부, 0.13.11 유지 (의도한 시험) |
| 14:41 | 5 | 0.14.0 (개발 키) | OTA | 건강 검사 후 확정, zone-rear |
| 14:47 | 5 | 0.14.1 (개발 키) | OTA | 확정. spitest: idle-high 발견 |
| 14:49 | 5 | 0.14.2 (개발 키) | OTA | 확정, spitest PASS, 27 분 측정 |
| 15:16 | 5 | 0.14.2+99 (일부러 망가뜨림) | OTA | 202.8 s 뒤 0.14.2 로 자동 복귀 |
| 15:22 | 5 | 0.14.2+77 (1588 실험) | OTA | **버스에서 사라짐 — 전원 재투입 필요** |
| 15:35 | 3, 4 | 0.14.2 (개발 키, 5번에서 27 분 돈 것과 같은 해시 4c5a3280) | `smp_udp.py 192.168.100.73,192.168.100.74 update` | 둘 다 35 s 만에 건강 검사 통과·확정, slot1 = 0.13.11(롤백용) |

3·4번 확인(15:36–15:38): 역할 그대로(zone-left / zone-right, MAC 표), `panel spitest` **두 보드 모두 PASS**(실제 패널이 달린 핀에서 같은 파형:
0 = 216–366 ns, 1 = 666–816 ns, 비트 1183–1333 ns, idle 0/0), 차 상태 live(속도·램프 키를 zone-left 가 냄), 스틱·근접 값 들어옴,
PTP 재잠금 후 1분 σ 10 / 19 ns. **LED 를 사람 눈으로는 못 봤다** — 패널 그림이 이상하면 `python3 tools/bus_auth.py <3|4> panel drv bb`(저장)로 비트뱅으로 되돌린다.
3·4번은 0.14.2 라 워치독 조기 무장(0.14.3)은 아직 없다 — 그 구멍은 *새로 올리는 이미지*에 해당하므로 다음 업데이트를 0.14.3 이상으로 하면 된다.

MCUboot 는 세 보드 모두 그대로(개발 키). SWD 로 구운 것 없음.

## 남은 일
1. **5번 전원 재투입**(USB 를 뺐다 꽂기) → 0.14.2 로 돌아오는지 확인 → 0.14.3 OTA, 10 분 이상 관찰.
2. 5번을 USB 에 꽂은 김에: 프로젝트 키 MCUboot 로 다시 굽기(위 절차) + 1588 이미지를 콘솔 보며 다시 띄워 멈춘 곳 찾기.
3. 3·4번 MCUboot 키 교체는 SWD 가 필요 — 데모가 끝난 뒤 한 대씩.
4. 콘솔 재시작(사용자): 새 `zone.py` 가 인증 셸(`ash`)과 id 주소를 쓴다. 지금 떠 있는 콘솔(옛 코드)은 0.14 노드에 `sh` 를 보내므로
   노드 버튼(LED 배치·슬립·진단)이 "denied: authentication required" 를 받는다(표시·업데이트 버튼 밖의 기능). "모든 노드 펌웨어 업데이트" 버튼은
   `smp_udp.py` 를 새로 띄우므로 arming 을 포함해 그대로 된다.
5. 3·4번 LED 패널을 눈으로 확인(SPI 구동). 6. 0.14.3 을 5번에서 10 분 이상 돌린 뒤 3·4번에.
