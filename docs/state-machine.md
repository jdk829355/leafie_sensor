# Main Device State Machine

상위 상태머신은 기기의 전체 lifecycle만 관리한다.

## 상태

```text
BOOT
PROVISIONING
CONNECTING
WIFI_CONNECTED
WAITING_CLAIM
CLAIMING
ACTIVE
```

## 흐름

```text
BOOT
 │
 ├─ Wi-Fi credential 없음
 │        ↓
 │   PROVISIONING ←──┐
 │        │          │ 비밀번호 오류 / AP 없음 (CRED_FAIL)
 │        │          │ 같은 BLE 연결에서 Wi-Fi 정보 재수신
 │        │ Wi-Fi 정보 수신
 │        ↓
 └──→ CONNECTING
          │
          ├─ 연결 실패
          │     ↓
          │   retry
          │     ↓
          │ CONNECTING
          │
          └─ 연결 성공
                 ↓
          WIFI_CONNECTED
                 │
                 ├─ deviceToken 있음
                 │        ↓
                 │      ACTIVE
                 │
                 └─ deviceToken 없음
                          ↓
                    WAITING_CLAIM
                          │
                          │ CLAIM_START
                          ↓
                       CLAIMING
                       /      \
             CLAIM_FAILED    CLAIM_SUCCESS
                  ↓               ↓
          WAITING_CLAIM          ACTIVE
```

`ACTIVE`에서 Wi-Fi가 끊겨도 상태는 `ACTIVE`로 유지한다. 재접속만 계속 시도하고 텔레메트리는 실패하면 버린다([telemetry.md](telemetry.md)).
`CONNECTING`으로 되돌리지 않는 이유는 `CONNECTING`이 BLE와 5분 규칙(아래)의 대상 상태이기 때문이다.

버튼([factory-reset.md](factory-reset.md))은 어느 상태에서도 동작하는 global event이며 상태 전이가 아니라 재부팅이다.

`CONNECTING`에서의 재시도는 별도의 `RECONNECTING` 상태로 만들지 않는다.

재시도 시 행동이 달라지는 요구가 생기기 전까지 `CONNECTING` 내부에서 처리한다.

Wi-Fi 연결이 오래 실패할 때의 처리(아래 "Wi-Fi 장기 실패 처리")도 새 state를 만들지 않고 이 규칙 안에서 다룬다.

## 상태 전이 표

| Current State    | Event / Condition   | Action              | Next State       |
| ---------------- | ------------------- | ------------------- | ---------------- |
| `BOOT`           | Wi-Fi credential 없음 | provisioning 시작     | `PROVISIONING`   |
| `BOOT`           | Wi-Fi credential 있음 | Wi-Fi 연결 시작         | `CONNECTING`     |
| `PROVISIONING`   | provisioning 성공     | Wi-Fi 연결 시작         | `CONNECTING`     |
| `PROVISIONING`   | 비밀번호 오류, AP 없음      | 상태머신 reset, Wi-Fi 정보 재수신 | `PROVISIONING`   |
| `CONNECTING`     | 연결 성공               | deviceToken 확인      | `WIFI_CONNECTED` |
| `CONNECTING`     | 연결 실패               | 재시도                  | `CONNECTING`     |
| `CONNECTING`     | 연결 5분 연속 실패, 토큰 없음  | Wi-Fi 정보 삭제 후 재부팅   | `BOOT` → `PROVISIONING` |
| `WIFI_CONNECTED` | deviceToken 있음      | 정상 동작 준비            | `ACTIVE`         |
| `WIFI_CONNECTED` | deviceToken 없음      | claim 시작 대기         | `WAITING_CLAIM`  |
| `WAITING_CLAIM`  | `CLAIM_START` 수신    | claim subprocess 시작 | `CLAIMING`       |
| `CLAIMING`       | `CLAIM_SUCCESS`     | 정상 동작 시작            | `ACTIVE`         |
| `CLAIMING`       | `CLAIM_FAILED`      | claimToken 폐기, 새로운 claim 대기 | `WAITING_CLAIM`  |
| `WAITING_CLAIM`  | Wi-Fi 5분 연속 끊김, 토큰 없음 | Wi-Fi 정보 삭제 후 재부팅   | `BOOT` → `PROVISIONING` |
| `ACTIVE`         | Wi-Fi disconnected  | 재접속만 시도, 상태 유지      | `ACTIVE`         |
| 모든 상태            | 버튼 3초 이상 후 놓음       | Wi-Fi 정보 삭제 후 재부팅   | `BOOT`           |
| 모든 상태            | 버튼 10초 누름           | Wi-Fi 정보와 deviceToken 삭제 후 재부팅 | `BOOT`           |

`CLAIMING` 중에는 5분 규칙을 보류한다. claim 상태머신이 끝난 뒤 조건을 확인한다.

## Wi-Fi 장기 실패 처리 (확정, 토큰 없는 경우만 구현됨, 기기 시험 전)

Wi-Fi가 연속으로 끊겨 있는 시간이 **5분**을 넘으면 `deviceToken` 유무에 따라 다르게 처리한다.
연결에 성공하면 이 시간은 0으로 되돌린다. 서버 오류(5xx 등)로 claim이나 업로드가 실패하는 것은 대상이 아니다.
Wi-Fi가 끊긴 상태만 센다.

| deviceToken | 5분 이상 연속 끊김 시 동작 |
|---|---|
| 없음 (claim 전: `CONNECTING`, `WAITING_CLAIM`) | 지킬 것이 없으므로 Wi-Fi 정보를 지우고 `PROVISIONING`으로 돌아간다. |
| 있음 (`ACTIVE` 계열) | **자동으로 처리하지 않는다.** 재접속을 계속 시도하며, 복구는 사용자가 앱의 "Wi-Fi 다시 설정"과 기기 버튼([factory-reset.md](factory-reset.md), 3초)으로 한다. |

규칙:

```text
- 시간 기준이다. 공유기 재부팅이나 정전 복구 때는 기기가 먼저 켜져 몇 분간 실패하는 것이 정상이다.
- CLAIMING 중에는 동작을 보류한다. claim 상태머신(최대 5회 재시도, 약 90초)이 끝난 뒤 조건을 확인한다.
  CLAIM_FAILED가 되면 claimToken은 폐기하고 WAITING_CLAIM으로 간다(claimToken은 NVS에 저장하지 않는다).
- deviceToken이 있는 기기가 오래 끊겨도 BLE를 자동으로 열지 않는다. 이유: network_prov_mgr_start_provisioning()은 시작할 때
  STA를 끊고 RAM의 STA 설정을 비운 뒤 esp_wifi_disconnect()를 호출한다(manager.c). 그래서 광고하는 동안은 기존 Wi-Fi로의
  재접속이 멈추고, 이를 풀려면 시간 제한 광고 창 같은 별도 설계가 필요하다. 앱의 명시적 UI와 버튼으로 충분하다고 판단해 두지 않는다.
  필요가 확인되면 그때 다시 정한다.
- deviceToken을 가진 기기가 telemetry에서 403을 받아도 토큰을 자동으로 지우지 않는다.
  API 키나 Authorizer 설정 오류 한 번이 전체 기기의 등록을 지울 수 있기 때문이다. 소유 이전은 공장 초기화(factory-reset.md)로 한다.
```

Wi-Fi가 필요한 다른 상태에서도 `WIFI_DISCONNECTED`가 발생할 수 있다. global event로 만들지 않고 위 표와 같이
상태별로 처리한다: `CONNECTING`은 재시도, `ACTIVE`는 재접속만 하고 상태 유지, 토큰이 없는 상태는 5분 규칙을 적용한다.
