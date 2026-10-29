# Zephyr · Zenoh · OTA — 무엇이 무엇이고, 어디까지 했나

이 저장소의 SAM E51 + LAN8651(10BASE-T1S) 존 노드를 이루는 세 가지를 나누어 적는다. 문서화(외부 검토·보고서)용 정본.
수치·버전은 이 저장소와 벤치에서 확인한 것만 적었고, 확인하지 않은 것은 "미확인"으로 표시했다.

## 한 줄 요약

| 이름 | 무엇인가 | OTA 인가 |
|---|---|---|
| **Zephyr** | 노드의 RTOS (커널·드라이버·네트워크 스택). 리눅스 재단 오픈소스 | 아니다. OTA 를 *담는* 그릇 |
| **zenoh-pico** | 노드가 차량 버스(Zenoh)에 붙는 pub/sub 클라이언트. 램프·스틱·속도 같은 **신호**가 오간다 | 아니다. 데이터 버스 |
| **MCUboot + mcumgr(SMP/UDP)** | 부트로더 + 원격 이미지 관리. **펌웨어를 T1S 너머로 올리고 바꿔 끼운다** | **이것이 OTA**(정확히는 네트워크 펌웨어 업데이트, FOTA) |

즉 "Zephyr 랑 Zenoh 가 OTA 냐" → 아니다. OTA 는 **MCUboot + mcumgr** 이고, Zephyr 위에서 돈다.
Zenoh 는 OTA 와 별개인 **신호 버스**다(차량 API, `docs/VEHICLE_API.md`).

## 구성 (이 저장소 기준)

```
 PC(벤치) ──USB-LAN── D10 스위치 ── ESP T1S 게이트웨이 ══ 10BASE-T1S 2선 버스(PLCA) ══ SAM E51 노드 ×N
   zenohd :7447 (라우터, 최신값 저장)                                       │
   tools/smp_udp.py  (OTA 클라이언트)                                       ├─ Zephyr 3.7 (LTS 계열)*
   tools/console/    (패킷·상태 콘솔)                                       ├─ zenoh-pico 1.10.1 클라이언트 (TCP)
                                                                           ├─ mcumgr SMP 서버 UDP :1337
                                                                           └─ MCUboot (slot0 = 실행, slot1 = 새 이미지)
```
\* 빌드 워크스페이스 `~/zephyrproject` 의 VERSION 파일 기준 3.7.3-rc1.

### 1) Zephyr — RTOS
- 보드 정의·LAN865x(=LAN8651) 드라이버·IPv4/UDP/TCP·gPTP 를 쓴다. 이 저장소의 수정분은 `zephyr-patches/`, 보드는 `boards/`.
- 빌드는 `west build --sysbuild` 로 **MCUboot 와 앱을 한 번에** 만든다(`apps/node/sysbuild.conf`).

### 2) Zenoh (zenoh-pico) — 신호 버스
- 노드는 **클라이언트**로 PC 의 `zenohd` 에 TCP 로 붙는다(`apps/node/src/zone_zenoh.c`). 피어/멀티캐스트 아님.
- 키는 VSS 계열 경로(`vehicle/body/lights/...`, `vehicle/speed` 등) — 정본은 `api/vehicle_api.yaml` → `tools/apigen.py` 가 C 헤더·파이썬 표·문서를 생성.
- `zenohd` 가 최신값을 저장(REST :8834)해서, 늦게 붙은 노드·앱도 현재 상태를 바로 읽는다.
- 펌웨어 바이너리는 **Zenoh 로 보내지 않는다.** (가능은 하지만 지금 구조는 SMP 로 분리)

### 3) OTA — MCUboot + mcumgr SMP/UDP
흐름(`tools/smp_udp.py <ip[,ip...]> update <zephyr.signed.bin>`):
1. 이미지를 384 B 조각으로 UDP :1337 에 올린다 → 노드가 **slot1** 에 쓴다 (조각마다 재시도).
2. slot1 을 **test boot** 로 표시하고 리셋.
3. MCUboot 가 서명을 검사하고 slot0 ↔ slot1 을 **swap** 한다.
4. 새 앱이 네트워크 드라이버까지 올라오면 **스스로 confirm** 한다. confirm 못 하고 다시 리셋되면 MCUboot 가 **이전 이미지로 되돌린다**(롤백).

실측: 두 노드를 한 번에 `update` → 24 s (2026-10-28, README). 콘솔의 "모든 노드 펌웨어 업데이트" 버튼도 같은 경로.

## "최신 방식인가?" — 업계 OTA 와 비교

| 항목 | 여기 | 양산 차량 OTA 에서 보통 | 판단 |
|---|---|---|---|
| 부트로더/이미지 교체 | MCUboot, 2슬롯 swap + 자동 롤백 | A/B(이중 뱅크) + 롤백 | **같은 방식**. MCUboot 는 Zephyr·MCU 계열의 사실상 표준 |
| 전송 | mcumgr SMP over UDP, 버스 너머 직접 | 클라우드 → 차량 게이트웨이(TCU) → ECU(UDS/DoIP 등) | 구조상 **게이트웨이 뒤 ECU 단계**에 해당. 클라우드·캠페인 관리는 없음 |
| 서명 | **MCUboot 개발 키**(공개된 키) | 제조사 키 + HSM, 키 회전 | **벤치용**. 차에 쓰려면 반드시 교체 |
| 보안 프레임워크 | 없음 | **Uptane**(차량 OTA 보안 표준, IEEE-ISTO) / UNECE **R156**(SUMS)·R155(CSMS) 대응 | 미구현 |
| 암호화·인증된 채널 | 평문 UDP, 인증 없음 | TLS/DTLS, 인증된 진단 세션 | 미구현 |
| 차분(delta) 업데이트 | 전체 이미지 | 대용량 ECU 는 delta 흔함 | MCU 이미지(수백 KB)엔 필수 아님 |
| 다중 노드 | 콤마 목록으로 순차/동시 | 캠페인·순서·의존성 관리 | 단순 |

정리: **MCU 쪽 메커니즘(MCUboot 2슬롯 + 서명 검사 + 롤백 + mcumgr)은 현재 쓰이는 표준 방식**이다.
빠진 것은 그 위의 **보안·관리 계층**(제조사 키, 인증 채널, Uptane 식 메타데이터 검증, 캠페인)이다.
"OTA 를 구현했다"보다는 **"ECU 단 네트워크 펌웨어 업데이트(FOTA)를 구현했다"**가 정확한 표현.

Zenoh 쪽: 차량 내 신호 버스로 Zenoh 를 쓰는 것은 최신 흐름에 속한다(ROS 2 의 `rmw_zenoh`, Eclipse SDV 계열). 다만 양산 차량의
주류는 아직 SOME/IP·DDS(AUTOSAR Adaptive)이고, 이 저장소의 Zenoh 사용은 **연구·데모 단계**다.

## 다음에 할 수 있는 것 (우선순위 순)
1. **서명 키 교체**: MCUboot 개발 키 → 자체 키(`imgtool keygen`), 키는 저장소 밖 보관. 가장 싸고 효과 큼.
2. **SMP 채널 보호**: 버스 위 아무 장치나 올릴 수 있는 상태 → 최소한 게이트웨이(ESP/스위치)에서 :1337 을 관리 호스트만 허용(ACL), 이후 DTLS 또는 SMP over 인증 세션.
3. **이미지 메타데이터 검증**(Uptane 의 축소판): 버전 단조 증가(다운그레이드 금지, MCUboot `downgrade prevention`), 하드웨어 ID 일치.
4. **업데이트 상태를 Zenoh 로 보고**: `ecu/<id>/status` 에 실행 버전·확정 여부를 실어, 콘솔/HMI 가 SMP 를 따로 묻지 않아도 되게.
5. (선택) Zenoh 로 이미지 배포: 여러 노드에 한 번에 — 지금 규모(2~3대)에선 이득 작음.

## 관련 파일
- `apps/node/sysbuild.conf`, `apps/node/sysbuild/mcuboot.conf` — MCUboot 사용, 슬롯 구성
- `apps/node/prj.conf` — mcumgr SMP/UDP, zenoh-pico 설정
- `tools/smp_udp.py` — OTA 클라이언트 (`list` / `upload` / `update` / `reset`)
- `apps/node/src/zone_zenoh.c` — Zenoh 클라이언트, 키 발행·구독
- `api/vehicle_api.yaml`, `docs/VEHICLE_API.md` — 차량 API(신호 버스) 정의
- `README.md` "네트워크 펌웨어 업데이트" 절 — 굽기·업데이트 명령, 함정
