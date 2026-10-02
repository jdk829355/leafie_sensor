# Backend Claim API

현재 예상 API:

```http
POST /api/v1/sensor-devices/{deviceId}/claims
POST /api/v1/sensor-device-claims/{claimToken}/complete
GET  /api/v1/sensor-device-claims/{claimToken}
```

FastAPI 기준 경로다. 푸시 설치용 `POST /api/v1/devices`와 구분한다.
telemetry의 `POST /devices/{deviceId}/telemetry`는 API Gateway라 이 경로를 바꾸지 않는다.

테이블과 스키마는 [backend-db.md](backend-db.md)를 본다.

## POST /api/v1/sensor-devices/{deviceId}/claims

호출 주체:

```text
Mobile App
```

인증:

```text
User JWT
```

목적:

```text
사용자가 특정 device를 claim하려는 작업 생성
claimToken 발급
```

## POST /api/v1/sensor-device-claims/{claimToken}/complete

호출 주체:

```text
ESP32
```

목적:

```text
ESP가 claimToken을 서버에 제출
claim 검증 완료
device 소유자 확정
deviceToken 발급
```

요청 본문:

```json
{ "deviceId": "D40592E7D168" }
```

* `Content-Type: application/json`. `claimToken`은 경로에, `deviceId`는 본문에 담는다.
* 서버는 이 `deviceId`가 claim이 묶인 `deviceId`와 같은지 검증한다.
  불일치는 재시도 불가 오류([claim-state-machine.md](claim-state-machine.md)의 `deviceId mismatch`)다.
* `deviceId`는 `device-info` endpoint로 앱에 준 값과 같은 12자리 대문자 hex(AGENTS.md의 Device Identity)다.

이 API는 네트워크 응답 유실 후 ESP가 재시도할 수 있도록 idempotent하게 설계해야 한다.

**재시도 정책 (확정)**: 이미 `COMPLETED`된 claim이 원래 발급 시각 기준 5분(claim TTL)
이내에 같은 `claimToken`으로 다시 호출되면, 서버는 새 `deviceToken`을 발급하고 이전
`deviceToken`을 무효화한다. 5분이 지난 뒤의 재호출은 `410`이다. 동시에 같은 claim이
완료되면(예: 응답 유실로 인한 재시도와 겹침) 서버가 하나만 실제로 소유권을 확정하고,
나머지 요청도 동일한 재발급 경로를 타 유효한 `deviceToken`을 받는다.

ESP 입장에서 이 정책은 상태 머신에 영향이 없다: 매 성공 응답마다 받은 `deviceToken`을
그대로 NVS에 덮어써 저장하면 된다([claim-state-machine.md](claim-state-machine.md) 그대로, 별도 state 불필요 —
AGENTS.md Design Principles의 3, 4번). 이전에 NVS 저장을 시도했다가 실패한 `deviceToken`은 애초에 저장되지
않았으므로 무효화 여부를 신경 쓸 필요가 없다.

## GET /api/v1/sensor-device-claims/{claimToken}

앱이 claim 결과를 확인하기 위해 정상 claim flow에서도 이 API를 주기적으로 polling한다.

주 목적:

```text
claim 결과 확인 (정상 flow)
복구
재연결
디버깅
```

인증은 User JWT다(앱이 호출). 다른 사용자의 claim은 존재 여부를 노출하지 않고 `404`다.
`status`가 `PENDING`이어도 `expires_at`이 지났으면 서버는 `EXPIRED`로 보고한다.
