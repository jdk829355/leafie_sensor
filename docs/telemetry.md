# Telemetry

Claim이 완료되어 `ACTIVE` 상태가 된 기기만 정상 telemetry 업로드를 수행한다.

예상 흐름:

```text
Sensor
 ↓
ESP32
 ↓ HTTPS
API Gateway
 ↓
SQS
 ↓
Lambda
 ↓
DB
```

ESP는 API Gateway 뒤의 구현을 알 필요가 없다.

ESP 관점에서는 HTTPS endpoint로 telemetry를 전송하는 것만 책임진다.

sampling과 upload는 분리하지 않고 1:1로 묶는다.

```text
센서 측정
    ↓
즉시 HTTPS 업로드
    ↓
sleep
    ↓
센서 측정 (반복)
```

측정/업로드 주기: 10분 (`TELEMETRY_INTERVAL_MS`)

개발 중에는 이 값을 10초로 낮춰 쓸 수 있다. 배포 전에 반드시 10분으로 되돌린다.

업로드 실패 시 정책:

```text
해당 측정값은 버리고 재시도하지 않는다.
다음 주기에 새로 측정한 값으로 다시 시도한다.
```

로컬 버퍼링/큐잉은 구현하지 않는다 (AGENTS.md Non-Goals의 "고급 offline telemetry buffering"과 일치).

## Endpoint

```text
POST {TELEMETRY_BASE_URL}/devices/{deviceId}/telemetry
```

* `TELEMETRY_BASE_URL`은 API Gateway(REST API)의 stage까지 포함한 주소다. stage가 빠지면 `403 Forbidden`이 된다.
* claim, ping용 백엔드 주소(`MOCK_SERVER_BASE_URL`)와는 별개이며, telemetry만 API Gateway로 보낸다.
* HTTPS이며 서버 인증서는 ESP-IDF 인증서 번들(`crt_bundle_attach`)로 검증한다.
* 이 경로는 API Gateway에만 있다. FastAPI의 푸시 설치 `POST /api/v1/devices`와 호스트가 달라 경로를 같이 두지 않는다.

## Headers

```text
Content-Type: application/json
Authorization: Bearer <deviceToken>
x-api-key: <공통 API 키>
```

* `x-api-key`는 모든 기기가 같은 키를 쓰는 공통 키다. 기기별 인증이 아니며 Authorizer와 함께 계속 요구된다.
* `Authorization`의 `deviceToken`은 API Gateway Lambda Authorizer(`leafie-telemetry-authorizer`)가 검증한다.
  * 경로의 `device_id` 기기가 `CLAIMED`이고, `SHA-256(deviceToken)`이 `sensor_devices.sensor_token_hash`와 같으면 통과한다.
  * 헤더가 없으면 `401`, 토큰이 틀리거나 기기가 `UNCLAIMED`/미등록이면 `403`이다. 둘 다 SQS에 들어가지 않는다.
  * Authorizer는 읽기 전용 DB 역할 `sensor_authorizer`로 접속한다 (Leafie `docs/sensor-telemetry.md`).
    접속 문자열은 SSM `/leafie/telemetry/authorizer/database-url`(SecureString)에 있다.
  * 결과는 (`Authorization`, 요청 경로) 단위로 60초 캐시된다. 재claim으로 이전 토큰이 무효화돼도 최대 60초는 통과할 수 있다.
  * 코드와 배포 스크립트는 `tools/authorizer/`에 있다.
* ESP는 `401`/`403`도 응답 상태만 로그로 남기고 재시도하지 않는다.

## Body

```json
{ "created_at": "2026-09-24T01:30:00Z", "lux": 123.4, "soilRaw": 3900 }
```

| 필드 | 타입 | 설명 |
|---|---|---|
| `created_at` | string \| null | 측정 시각(UTC, ISO 8601). SNTP 동기화 전이면 `null` |
| `lux` | number \| null | BH1750 조도(lx). 센서 읽기 실패 시 `null` |
| `soilRaw` | integer \| null | 토양 수분 ADC raw(0~4095). 읽기 실패 시 `null` |

규칙:

```text
세 필드를 항상 모두 보낸다. 값이 없으면 필드를 빼지 않고 null로 보낸다.
센서 하나가 실패해도 나머지 값은 전송한다. 판단은 서버가 한다.
created_at은 SNTP(pool.ntp.org)로 시간을 맞춘 뒤에만 채운다 (epoch < 2020-01-01이면 미동기화).
```

`created_at`이 필요한 이유는 SQS 표준 큐가 순서를 보장하지 않기 때문이다.

서버 측 구조(요청 검증 스키마, 테이블, 중복 방지, DLQ)는 Leafie 저장소의
`docs/sensor-telemetry.md`가 기준이다. ESP는 응답 상태를 로그로만 남기고 재시도하지 않는다.
