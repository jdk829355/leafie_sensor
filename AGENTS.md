AGENTS.md

이 파일에는 반드시 지켜야 할 규칙과 문서 목록만 둔다. 상세 설계는 아래 "문서 목록"의 `docs/` 파일에 있다.
설계를 바꾸기로 정하면 먼저 해당 문서에 기록하고 구현한다.

## 1. Project Overview

ESP32-C3 기반 식물 관리 센서 펌웨어 프로젝트다.
해당 프로젝트의 서버 및 문서는 [leafie](https://github.com/YuEngMe/Leafie)에 있다.

기기는 다음 기능을 담당한다.

- BLE를 통한 초기 Wi-Fi provisioning
- 기기 claim(사용자 계정 등록)
- 조도 센서 데이터 측정
- 토양 수분 센서 데이터 측정
- Wi-Fi를 통한 센서 데이터 업로드
- 네트워크 장애 시 재연결
- 공장 초기화

기존 모바일 클라이언트와 FastAPI 백엔드는 별도로 개발된다.

센서 데이터 ingestion 경로는 다음 구조를 사용한다.

```text
ESP32-C3
    ↓ HTTPS
API Gateway
    ↓
SQS
    ↓
Lambda
    ↓
DB
```

일반적인 사용자/식물/기기 관리 API는 기존 FastAPI 서버가 담당한다.

---

## 2. 문서 목록

| 문서 | 내용 |
|---|---|
| [docs/hardware.md](docs/hardware.md) | MCU, 센서, GPIO 핀 배정과 근거, 센서 동작 설정, Wi-Fi TX 출력 보드 특성, `sdkconfig.defaults` 고정 값, `secrets.h` |
| [docs/state-machine.md](docs/state-machine.md) | 상위 상태머신(상태, 흐름, 전이 표)과 Wi-Fi 장기 실패(5분 규칙) 처리 |
| [docs/factory-reset.md](docs/factory-reset.md) | BOOT 버튼 3초/10초 동작, 삭제 대상, 서버와의 관계 |
| [docs/provisioning.md](docs/provisioning.md) | BLE 기기 이름, ID/PIN(PoP), `device-info` 응답과 앱 판단 규칙, Wi-Fi 재수신, 목록 스캔 |
| [docs/app-ble-guide.md](docs/app-ble-guide.md) | 앱 개발자용으로 정리한 기기 연결 가이드 |
| [docs/ble-endpoints.md](docs/ble-endpoints.md) | BLE 책임 범위, `wifi_prov_mgr` 사용 방식, custom endpoint(`device-info`, `claim`) |
| [docs/claim-flow.md](docs/claim-flow.md) | claim 흐름, 신뢰 모델, `claimToken`/`deviceToken` |
| [docs/claim-state-machine.md](docs/claim-state-machine.md) | claim subprocess 상태, 전이 표, 오류 정책(HTTP 코드 매핑, backoff) |
| [docs/claim-api.md](docs/claim-api.md) | 백엔드 claim API 3종, idempotent 재시도 정책 |
| [docs/backend-db.md](docs/backend-db.md) | 백엔드 테이블(`sensor_devices` 등)과 구현된 스키마 |
| [docs/telemetry.md](docs/telemetry.md) | 측정/업로드 주기, endpoint, 헤더(Authorizer), body 형식 |

---

## 3. Firmware Framework

ESP-IDF를 사용한다.

기본 원칙:

* ESP-IDF 공식 API를 우선 사용한다.
* Wi-Fi provisioning을 직접 구현하지 않는다.
* ESP-IDF에서 제공하는 provisioning 기능을 우선 사용한다.
* BLE protocol을 처음부터 직접 구현하지 않는다.
* ESP-IDF provisioning custom endpoint로 처리할 수 있는 기능은 이를 우선 고려한다.
* raw GATT service/characteristic이 실제로 필요한 경우에만 별도로 도입한다.

보드 특성(Wi-Fi TX 출력)과 `sdkconfig.defaults` 값은 [docs/hardware.md](docs/hardware.md)에 있다.
`main/secrets.h`(버전 관리 제외)가 없으면 빌드가 실패한다.

---

## 4. Device Identity

각 ESP는 고유한 `deviceId`를 가진다.

`deviceId`는 사용자 ID와 무관하며 기기 자체의 식별자다.

기기마다 수동으로 ID를 입력하는 방식은 사용하지 않는다.

ESP32의 고유 하드웨어 식별값을 이용해 안정적으로 동일한 `deviceId`를 생성하는 방식을 사용한다.

생성 방식:

```text
소스: esp_efuse_mac_get_default()로 읽은 base MAC (6 byte)
형식: 구분자 없는 대문자 hex 문자열 (예: A1B2C3D4E5F6)
```

중요:

```text
deviceId
= 식별자
≠ 인증 정보
```

`deviceId`가 외부에 노출되어도 인증이 우회되어서는 안 된다.

---

## 5. Persistent Data

전원이 꺼져도 유지해야 하는 정보는 NVS에 저장한다.

현재 예상되는 persistent data:

```text
deviceId
provisioning PoP (BLE 접속 PIN, docs/provisioning.md)
Wi-Fi credentials
deviceToken
sensor calibration data
```

Wi-Fi credential은 가능한 ESP-IDF Wi-Fi/provisioning 계층에서 관리한다.

다음 값은 기본적으로 runtime state이며 영구 저장 대상으로 간주하지 않는다.

```text
currentState
retryCount
BLE connection state
lastError
```

claim 중 재부팅 복구를 위한 추가 데이터 저장 여부는 아직 확정하지 않았다.

필요성이 확인되기 전까지 임의로 persistent field를 추가하지 말 것.

`claimToken`은 NVS에 저장하지 않는다. 공장 초기화는 Wi-Fi credentials, `deviceToken`, 진행 중인 claim 임시 데이터를 지우고
`deviceId`와 provisioning PoP는 지우지 않는다(PoP는 라벨에 인쇄된 값이다). 자세한 내용은 [docs/factory-reset.md](docs/factory-reset.md).

---

## 6. 어기면 안 되는 규칙

상태머신:

* 재시도라고 해서 새 state를 만들지 않는다. `CONNECTING` 내부에서 처리한다(`RECONNECTING` 금지).
* `ACTIVE`에서 Wi-Fi가 끊겨도 상태는 `ACTIVE`로 유지하고 재접속만 시도한다.
* Wi-Fi 장기 실패 처리도 새 state를 만들지 않는다.
* 버튼은 상태 전이가 아니라 어느 상태에서도 동작하는 global event이며, 동작은 재부팅이다.
  `ACTIVE → PROVISIONING` 직접 jump 대신 reboot 후 정상 boot flow를 재사용한다.
* `CLAIMING` 중에는 5분 규칙을 보류한다.

토큰과 신뢰:

* 서버가 claim 상태의 source of truth다. 앱이 ESP에 보내는 `claimed = true`, `userId` 같은 값을 신뢰하지 않는다.
* ESP는 `deviceToken`을 opaque string으로만 다룬다. 해석하거나 검증하지 않는다.
* `claimNonce`, `claimSessionId`는 명확한 요구사항 없이 추가하지 않는다.
* `deviceToken`은 NVS 저장이 성공한 뒤에만 claim 성공으로 간주한다.
* `deviceToken`이 있는 기기가 telemetry에서 `403`을 받아도 토큰을 자동으로 지우지 않는다.
  설정 오류 한 번이 전체 기기 등록을 지울 수 있기 때문이다. 소유 이전은 공장 초기화로 한다.
* `deviceToken`이 있는 기기가 오래 끊겨도 BLE를 자동으로 열지 않는다. 광고 중에는 기존 Wi-Fi 재접속이 멈추기 때문이다.
  이유는 [docs/state-machine.md](docs/state-machine.md)에 있다.

BLE / provisioning:

* 고정 PoP를 코드에 두지 않는다. PIN은 첫 부팅 때 생성해 NVS에 저장하고 공장 초기화로 지우지 않는다.
* 별도의 raw GATT service/characteristic을 추가하지 않는다(실제 요구가 확인된 경우만).
* claim 결과는 BLE로 전달하지 않는다. 앱은 서버를 polling한다.

claim / 서버 통신:

* 모든 `4xx`를 permanent failure로 처리하지 않는다. 상태 코드 매핑은 [docs/claim-state-machine.md](docs/claim-state-machine.md).
* retry는 busy loop로 만들지 않고 exponential backoff를 쓴다.
* 서버 요청은 응답 유실을 고려해 idempotent하게 설계한다.

센서 / telemetry:

* 토양 수분은 raw 값만 올린다. %로 변환하지 않는다(기준은 서버가 정한다).
* 측정과 업로드는 1:1로 묶는다. 업로드 실패한 값은 버리고 재시도하지 않으며 로컬 버퍼링은 하지 않는다.
* telemetry body는 세 필드(`created_at`, `lux`, `soilRaw`)를 항상 모두 보내고 값이 없으면 `null`로 보낸다.
* GPIO2, GPIO8, GPIO9는 센서 상시 배선에 쓰지 않는다. BOOT 버튼(GPIO9)과 LED(GPIO8)는 버튼과 피드백 용도로만 읽고 쓴다.

배포 전:

* 텔레메트리 주기를 10초에서 10분(`TELEMETRY_INTERVAL_MS`)으로 되돌린다.
* mock 서버 주소(`MOCK_SERVER_BASE_URL`)를 실제 백엔드로 바꾸고, 로그에 찍히는 `claimToken`/`deviceToken`을 제거한다.

---

## 7. Design Principles

구현 시 다음 원칙을 따른다.

1. 상위 상태머신에는 세부 네트워크 구현을 넣지 않는다.
2. 복잡한 기능은 subprocess/state machine으로 분리한다.
3. 재시도라고 해서 무조건 새로운 state를 만들지 않는다.
4. 재시도 시 의미나 행동이 달라질 때만 별도 state를 만든다.
5. 서버를 claim 상태의 source of truth로 본다.
6. 앱과 ESP는 서로를 직접 신뢰하지 않는다.
7. persistent state와 runtime state를 명확하게 구분한다.
8. 서버 요청은 네트워크 실패와 응답 유실을 고려해 idempotent하게 설계한다.
9. ESP-IDF가 제공하는 기능을 직접 재구현하지 않는다.
10. 아직 결정되지 않은 사항을 임의로 확정하지 않는다.

---

## 8. Implementation Order

권장 구현 순서:

```text
1. ESP-IDF project 기본 실행
2. NVS 초기화
3. deviceId 생성
4. Wi-Fi hardcoded 연결
5. HTTPS 테스트 요청
6. BLE / Wi-Fi provisioning
7. Main state machine
8. Claim subprocess
9. FastAPI claim API 연동
10. deviceToken 저장 및 재부팅 복구
11. 조도 센서 연결
12. 토양 수분 센서 연결
13. Telemetry 전송
14. API Gateway + SQS + Lambda 연동
15. 장애 / 재시도 / factory reset 테스트
```

기능을 한 번에 통합하지 않는다.

각 단계가 독립적으로 동작함을 확인한 뒤 다음 단계로 이동한다.

---

## 9. Non-Goals for Initial Implementation

초기 구현에서는 다음 기능을 요구하지 않는다.

```text
OTA firmware update
BLE 상시 연결
실시간 BLE sensor streaming
복잡한 device credential rotation
여러 사용자 공동 소유
여러 claim credential 동시 지원
고급 offline telemetry buffering
복잡한 FreeRTOS task 구조
```

요구사항이 생기기 전에는 추가하지 않는다.

---

## 10. Open Decisions

telemetry payload 형식은 확정되었다.

현재 확정되지 않은 사항:

* provisioning BLE를 항상 켜 둘지, 부팅 후 일정 시간이나 버튼 입력으로 제한할지(PIN과 별개의 추가 방어).
  `WAITING_CLAIM`에서 BLE를 켜므로 그 설계와 함께 정한다.
* 마지막 claim 실패 원인을 알려 주는 읽기 전용 BLE 엔드포인트(`device-status`). 앱에서 필요가 확인될 때까지 보류한다.
* 앱의 오프라인 판정 기준은 마지막 수신 후 25분이다(텔레메트리 주기 10분 기준). 앱 쪽 규칙이며 기기와 무관하다.
* 라벨 PIN을 잃어버렸을 때의 복구 절차(지금은 시리얼 로그를 보거나 NVS를 지우고 새로 만드는 수밖에 없다).
* telemetry가 `403`(deviceToken 거부)으로 계속 실패할 때 기기의 동작(예: NVS의 deviceToken을 지우고 `WAITING_CLAIM`으로 돌아갈지). 지금은 로그만 남긴다.

(각 문서에 개별적으로 명시된 미확정 항목은 별도.)

구현 과정에서 새로운 미확정 항목이 필요해지면 임의로 결정하기 전에 명시적으로 결정하고 문서에 반영할 것.
