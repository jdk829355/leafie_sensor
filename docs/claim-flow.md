# Device Claim: 흐름과 토큰

Claim은 별도 subprocess로 관리한다. 내부 상태와 오류 정책은 [claim-state-machine.md](claim-state-machine.md),
서버 API는 [claim-api.md](claim-api.md)를 본다.

상위 상태머신은 claim의 세부 retry 과정을 알 필요가 없다.

상위에서는 다음 결과만 처리한다.

```text
CLAIM_SUCCESS
CLAIM_FAILED
```

## Claim Flow

정상적인 claim 흐름:

```text
App
 │
 │ 사용자가 기기 연결 시작
 │
 │ POST claim request
 ▼
FastAPI Server
 │
 │ 사용자 JWT 검증
 │ claim 생성
 │ claimToken 발급
 ▼
App
 │
 │ BLE
 │ START_CLAIM(claimToken)
 ▼
ESP
 │
 │ HTTPS
 │ claim completion request
 ▼
FastAPI Server
 │
 │ claimToken 검증
 │ deviceId 검증
 │ 사용자 ↔ device 연결
 │ deviceToken 발급
 ▼
ESP
 │
 │ deviceToken NVS 저장
 ▼
CLAIM_SUCCESS
 │
 ▼
ACTIVE
```

중요한 신뢰 모델:

```text
App
→ 사용자를 증명

ESP
→ 기기를 증명

Server
→ 사용자와 기기를 연결하는 최종 신뢰 주체
```

ESP는 `userId`를 직접 신뢰하거나 저장할 필요가 없다.

앱이 ESP에게 단순히 다음과 같은 값을 보내고 이를 신뢰하게 만들지 말 것.

```text
claimed = true
userId = ...
```

Claim 완료 여부는 반드시 서버의 응답으로 결정한다.

## Claim Tokens

현재 claim protocol에서는 다음 두 token을 구분한다.

### claimToken

등록 작업을 진행하기 위해 FastAPI 서버가 발급하는 임시 token.

특성:

```text
short-lived
single claim 용도
deviceId에 binding
사용자 JWT 검증 이후 발급
TTL: 5분
```

앱은 `claimToken`을 BLE를 통해 ESP에게 전달한다.

ESP는 이 값을 FastAPI 서버에 제출한다.

### deviceToken

claim이 최종 성공한 뒤 서버가 ESP에게 발급하는 장기 인증 credential.

ESP는 이를 NVS에 저장한다.

이후 telemetry 전송 등 기기 인증에 사용한다.

ESP는 deviceToken을 opaque string으로만 취급한다.

내부 구조를 해석하거나 검증하지 않고, NVS 저장과 HTTPS 요청 시 전달 용도로만 사용한다.

구체적인 포맷(랜덤 토큰, JWT 등)은 백엔드(FastAPI 서버) 결정 사항이며 이 레포지토리에서 확정하지 않는다.

```text
claimToken
= 등록 과정에서만 사용

deviceToken
= 등록 완료 이후 기기 인증에 사용
```

현재 설계에서는 별도의 `claimNonce` 또는 `claimSessionId`를 필수 요소로 사용하지 않는다.

명확한 요구사항 없이 새로 추가하지 말 것.
