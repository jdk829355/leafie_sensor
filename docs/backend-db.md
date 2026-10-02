# Backend Database

Claim에 필요한 최소 핵심 테이블:

```text
sensor_devices
sensor_device_claims
```

## sensor_devices

기기의 현재 상태를 나타낸다.

예상 필드:

```text
id
owner_user_id
sensor_token_hash
status
firmware_version
claimed_at
last_seen_at
created_at
```

`owner_user_id`는 claim 전까지 nullable할 수 있다.

## sensor_device_claims

기기 claim "시도" 또는 "작업"을 나타낸다.

예상 필드:

```text
id
device_id
user_id
claim_token_hash
status
expires_at
created_at
completed_at
```

예상 status:

```text
PENDING
COMPLETED
EXPIRED
CANCELLED
```

`sensor_device_claims.user_id`와 `sensor_devices.owner_user_id`는 의미가 다르다.

```text
sensor_device_claims.user_id
= claim을 요청한 사용자

sensor_devices.owner_user_id
= 현재 실제 기기 소유자
```

claim 성공 전에는 다음 상태가 가능하다.

```text
sensor_device_claims.user_id = user42
sensor_device_claims.status = PENDING

sensor_devices.owner_user_id = NULL
```

claim 성공 이후에만:

```text
sensor_devices.owner_user_id = user42
```

로 변경한다.

## 구현된 스키마 (Leafie PR #94)

위 필드는 다음 테이블로 확정했다. 푸시 설치 테이블 `device_tokens`와 이름이 겹치지 않게
센서 기기에는 `sensor_`를 붙인다. 요청·응답 필드 `deviceId`, `deviceToken` 이름은 바꾸지 않는다.
`deviceToken`의 해시는 `sensor_token_hash`다.

```text
sensor_devices         센서 기기. id는 deviceId(12자리 대문자 hex)
sensor_device_claims   claim 시도
plant_sensor_devices   식물과 기기의 1:1 연결
sensor_readings        telemetry 측정값
```

* `sensor_devices.id`가 deviceId이며 `owner_user_id`는 claim 전에 NULL이다.
* `sensor_devices.status`는 `UNCLAIMED` / `CLAIMED`다. `CLAIMED`이면 `owner_user_id`, `sensor_token_hash`,
  `claimed_at`이 모두 채워지고, `UNCLAIMED`이면 소유자와 토큰 해시가 NULL이어야 한다 (CHECK 제약).
* 식물 연결은 `sensor_devices`에 컬럼을 두지 않고 `plant_sensor_devices`로 분리한다.
  식물 하나에 기기 하나, 기기 하나에 식물 하나다.
* `sensor_device_claims`의 status는 위 값 그대로이며, 기기당 `PENDING`은 하나만 허용한다.
* `sensor_devices.last_seen_at`은 백엔드가 갱신한다. 수집 Lambda의 DB 역할은 `sensor_readings` INSERT만 가능하다.
* `sensor_devices`는 센서 장치이며 백엔드의 푸시 수신용 `device_tokens`와 무관하다.

컬럼과 제약은 Leafie 저장소 `docs/erd.md`가 기준이다. telemetry 수신 경로는
`docs/sensor-telemetry.md`가 기준이다.
