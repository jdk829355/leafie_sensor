# Claim Sub-State Machine 과 오류 정책

## 내부 상태

```text
CLAIM_REQUESTING
CLAIM_RETRY_WAIT
CLAIM_SUCCESS
CLAIM_FAILED
```

상태 흐름:

```text
                   success
CLAIM_REQUESTING ─────────────→ CLAIM_SUCCESS
       │
       │ retryable failure
       ▼
CLAIM_RETRY_WAIT
       │
       │ retry timer
       ▼
CLAIM_REQUESTING

CLAIM_REQUESTING
       │
       │ permanent failure
       │ or max retry exceeded
       ▼
CLAIM_FAILED
```

## 전이 표

| Current State      | Event / Condition | Guard                  | Action                     | Next State         |
| ------------------ | ----------------- | ---------------------- | -------------------------- | ------------------ |
| `CLAIM_REQUESTING` | 상태 진입             | valid `claimToken`     | FastAPI claim API 호출        | 응답 대기              |
| `CLAIM_REQUESTING` | 서버 성공 응답          | valid `deviceToken` 수신 | deviceToken 저장             | `CLAIM_SUCCESS`    |
| `CLAIM_REQUESTING` | 요청 실패             | retryable              | retry count 증가, backoff 계산 | `CLAIM_RETRY_WAIT` |
| `CLAIM_REQUESTING` | 요청 실패             | permanent failure      | 실패 원인 기록                   | `CLAIM_FAILED`     |
| `CLAIM_REQUESTING` | 요청 실패             | max retry 초과           | 실패 처리                      | `CLAIM_FAILED`     |
| `CLAIM_RETRY_WAIT` | retry timer 만료    | Wi-Fi available        | 동일 claim 재요청               | `CLAIM_REQUESTING` |
| `CLAIM_SUCCESS`    | 상태 진입             | deviceToken 저장 완료      | 상위 FSM에 `CLAIM_SUCCESS` 전달 | subprocess 종료      |
| `CLAIM_FAILED`     | 상태 진입             | -                      | 상위 FSM에 `CLAIM_FAILED` 전달  | subprocess 종료      |

`deviceToken`은 NVS 저장이 성공한 이후에만 claim 성공으로 간주한다.

```text
서버 응답 성공
    ↓
deviceToken 수신
    ↓
NVS 저장 성공
    ↓
CLAIM_SUCCESS
```

## Claim Error Policy

Retry 가능한 오류 예:

```text
HTTP timeout
DNS 일시적 실패
Wi-Fi / network 일시적 장애
HTTP 5xx
HTTP 429
```

Retry 불가능한 오류 예:

```text
claimToken invalid
claimToken expired
deviceId mismatch
이미 다른 사용자가 소유한 device
서버의 명시적인 claim 거절
```

`POST /api/v1/sensor-device-claims/{claimToken}/complete`의 실제 HTTP 상태 코드 매핑
(백엔드 leafie 레포 PR로 확정됨):

```text
404 = claimToken을 찾을 수 없음                    → permanent failure
409 = deviceId mismatch                          → permanent failure
410 = claim 만료(최초 발급 후 5분 경과, 재시도 창 포함) → permanent failure
422 = 요청 본문 검증 실패(deviceId 누락/형식 오류)      → permanent failure
429 = rate limit                                 → retryable
5xx = 서버 오류                                    → retryable
```

모든 `4xx`를 무조건 permanent failure로 처리하지 말 것.

서버 API 계약에 따라 세부 오류 코드를 판단한다.

Retry는 busy loop로 구현하지 않는다.

backoff를 사용한다.

수치:

```text
최대 retry 횟수: 5회
backoff: exponential, 2s → 4s → 8s → 16s → 32s (상한 32s)
```

최초 요청 1회와 재시도 5회, 총 6회 요청이 모두 실패하면 `CLAIM_FAILED`로 처리한다. 대기는 5번(2+4+8+16+32 = 62초)이고
요청 타임아웃(각 5초)을 더하면 최대 약 90초다.
