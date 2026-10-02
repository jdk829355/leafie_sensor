# 앱 개발자용 기기 연결 가이드 (BLE)

앱이 Leafie 센서 기기를 찾고, Wi-Fi를 설정하고, 사용자 계정에 등록(claim)할 때 알아야 할 내용이다.
설계의 기준은 `AGENTS.md`이며, 이 문서는 앱 쪽에 필요한 부분만 뽑아 정리한 것이다. 둘이 다르면 `AGENTS.md`가 맞다.

> 상태: 펌웨어는 구현되어 빌드까지 확인했지만 **실제 기기에서는 아직 시험하지 않았다.** 시험 결과에 따라 일부 내용이 바뀔 수 있다.

## 1. 용어

| 이름 | 의미 | 쓰이는 구간 |
|---|---|---|
| `deviceId` (ID) | 기기 식별자. 비밀이 아니다. 12자리 대문자 hex (예: `D40592E7D168`) | BLE 이름, 서버 API |
| PIN (= PoP) | BLE 접속 자격을 증명하는 비밀. 숫자 8자리. 기기 라벨에 인쇄되어 있다 | 앱 ↔ 기기 (BLE) |
| `claimToken` | 서버가 발급하는 임시 토큰(TTL 5분, 64자). 등록 과정에서만 쓴다 | 앱 → 기기(BLE) → 서버 |
| `deviceToken` | claim 성공 후 서버가 기기에 주는 장기 토큰. **앱은 볼 수 없다** | 기기 ↔ 서버 |

기기 라벨 예시:

```text
Leafie 센서
ID : D40592E7D168
PIN: 48201937
```

## 2. 전체 흐름

```text
1. BLE 검색          PROV_<deviceId> 방송을 찾는다
2. 기기 선택         라벨의 ID와 같은 기기를 고른다 (주변에 한 대뿐이면 생략 가능)
3. PIN 입력          라벨의 PIN을 사용자가 입력한다 (QR 스캔은 쓰지 않는다)
4. 세션 열기         PIN을 PoP로 protocomm 세션을 연다 (Security 1)
5. device-info 읽기  기기가 어떤 상황인지 확인한다
6. Wi-Fi 전달        기기가 스캔한 목록에서 고르거나 SSID를 직접 입력한다
7. claim 시작        서버에서 claimToken을 받아 기기에 전달한다
8. 결과 확인         서버의 claim 상태를 polling한다
```

6번은 `state == "PROVISIONING"`일 때만, 7번은 `hasDeviceToken == false`일 때만 한다. 자세한 분기는 3절을 본다.

## 3. `device-info`로 분기하기

세션을 연 뒤 `device-info` 엔드포인트를 읽는다. 기기는 `PROVISIONING`과 `WAITING_CLAIM`, 두 BLE 세션 모두에 이 엔드포인트를 등록한다.

```json
{ "deviceId": "D40592E7D168", "state": "WAITING_CLAIM", "hasDeviceToken": false }
```

| state | hasDeviceToken | 의미 | 앱이 할 일 |
|---|---|---|---|
| `PROVISIONING` | `false` | 처음 등록. Wi-Fi 정보 없음 | Wi-Fi 전달 → 연결 후 claim |
| `PROVISIONING` | `true` | 버튼으로 Wi-Fi를 재설정한 기기 | Wi-Fi만 전달. claim 불필요 |
| `WAITING_CLAIM` | `false` | Wi-Fi는 연결됨. claim 대기 | claimToken 전달 |

판단 규칙은 두 줄이다.

```text
Wi-Fi 정보 입력 창  : state == "PROVISIONING" 이면 띄운다 (hasDeviceToken과 무관)
claim 단계 필요 여부 : hasDeviceToken == false 이면 Wi-Fi 연결 뒤 claim까지 진행한다
                     true 이면 Wi-Fi만 받으면 기기가 바로 ACTIVE가 되므로 claim은 하지 않는다
```

BLE 방송에는 이름 `PROV_<deviceId>`만 있다. 상태는 세션을 연 뒤 `device-info`로만 알 수 있다.

## 4. Wi-Fi 설정

### 목록

앱이 보여 줄 Wi-Fi 목록은 **기기가 스캔한 결과**를 쓴다. ESP-IDF의 `prov-scan` 엔드포인트가 SSID, RSSI, 채널, 보안 방식을 돌려준다.

- iOS는 일반 앱이 주변 Wi-Fi 목록을 읽을 수 없으므로 앱 자체 스캔은 쓰지 않는다. 비밀번호는 사용자가 입력한다.
- 목록에 없는 숨김 SSID는 직접 입력할 수 있게 한다.
- 기기는 2.4GHz 전용이다. 5GHz 네트워크는 목록에 나오지 않으니, 사용자의 공유기가 5GHz만 켜져 있으면 안내가 필요하다.

### 실패와 재시도

비밀번호가 틀리거나 AP를 찾지 못하면 상태 조회에서 `AuthError` 또는 `NetworkNotFound`가 온다.
기기가 상태를 초기화하므로 **같은 BLE 연결에서 Wi-Fi 정보를 다시 보내면 된다.** 재부팅이나 재연결은 필요 없다.

## 5. claim

### 순서

```text
앱 → 서버   POST /api/v1/sensor-devices/{deviceId}/claims      (User JWT) → claimToken 발급
앱 → 기기   BLE claim 엔드포인트에 { "claimToken": "..." } 전달 → { "status": "ok" }
기기 → 서버 POST /api/v1/sensor-device-claims/{claimToken}/complete
앱 → 서버   GET  /api/v1/sensor-device-claims/{claimToken}      (polling)
```

- `claimToken`은 BLE로 보내는 payload에 JSON으로 담는다. 64자이다.
- **claim 결과는 BLE로 오지 않는다.** 앱은 서버의 GET으로 확인한다.
- 앱이 기기에 `claimed = true`나 `userId` 같은 값을 보내는 일은 없다. 서버가 최종 판단한다.
- `PENDING`이어도 `expires_at`이 지났으면 서버는 `EXPIRED`로 응답한다.
- claimToken의 TTL은 5분이다.

### 기기의 claim 재시도

기기는 서버 오류나 네트워크 문제 때 최대 6번(최초 1번 + 재시도 5번) 요청한다. 대기는 2, 4, 8, 16, 32초이고 최대 약 90초가 걸린다.
이 시간 동안 서버는 `PENDING`으로 보인다.

### claim 후 상태 판단

claimToken을 보낸 뒤에는 서버의 claim 상태와 BLE 방송을 **함께** 본다. claim이 성공하면 기기는 BLE를 다시 켜지 않는다.
따라서 `PROV_<deviceId>` 방송이 다시 나타나면 기기 쪽 실패(`CLAIM_FAILED` 또는 재부팅)를 뜻한다.

| 서버 `GET claim` | BLE 방송 | 의미 |
|---|---|---|
| `COMPLETED` | 없음 | 성공 |
| `PENDING` | 다시 나타남 | 기기가 포기함. 연결해서 `device-info.state`로 분기한다 |
| `PENDING` | 없음 | 기기가 `CLAIMING` 진행 중(최대 약 90초). 약 2분이 지나도 같으면 오류로 안내한다 |

방송이 다시 나타나 `device-info`를 읽었을 때:

| state | 의미 | 앱 동작 |
|---|---|---|
| `WAITING_CLAIM` | Wi-Fi 정보는 남아 있고 claim만 실패 | **새 claim을 발급**하고(서버가 이전 `PENDING`을 취소한다) 토큰을 다시 보낸다 |
| `PROVISIONING` (`hasDeviceToken: false`) | Wi-Fi 정보가 지워짐(5분 규칙 또는 사용자의 버튼 조작) | Wi-Fi 정보를 먼저 전달하고, 연결된 뒤 **새 claim**으로 진행한다 |

`PROVISIONING`으로 돌아간 기기는 이전 claimToken을 이미 버렸다(RAM에만 있었다). 앱은 항상 새 claim을 발급한다.

### 예외: Wi-Fi가 이미 불량인 `WAITING_CLAIM`

Wi-Fi가 붙은 뒤에 공유기나 비밀번호가 바뀌었고 기기의 5분 규칙이 아직 동작하기 전이면, 기기는 Wi-Fi 정보를 들고 있어서 `device-info`로 구분할 수 없다. 이때 claim은 계속 실패한다.

앱은 같은 기기에서 claim이 **2~3번 연속 실패**하면 다음처럼 안내한다.

> Wi-Fi 설정이 바뀌었을 수 있습니다. 기기 버튼을 3초 눌러 Wi-Fi를 다시 설정하세요.

5분이 지나면 기기가 스스로 `PROVISIONING`으로 돌아가기도 한다.

## 6. 기기 버튼과 복구

기기의 BOOT 버튼(GPIO9)으로 사용자가 직접 복구한다. 앱의 안내 문구에 쓸 수 있다.

| 조작 | 동작 | deviceToken |
|---|---|---|
| 3초 이상 ~ 10초 미만 누른 뒤 **놓음** (LED 2번 깜빡임) | Wi-Fi 정보만 삭제하고 재부팅 → `PROVISIONING` | 유지 |
| 10초 이상 누름 (LED 5번 깜빡임) | Wi-Fi 정보와 deviceToken 삭제 후 재부팅 | 삭제 |

- **공유기나 Wi-Fi 비밀번호가 바뀐 경우**: 버튼 3초. `deviceToken`이 남아 있으므로 Wi-Fi만 다시 받으면 claim 없이 `ACTIVE`가 된다. 서버에서 기기를 삭제할 필요가 없다.
- **소유자를 바꾸거나 다시 등록하는 경우**: 앱에서 기기를 삭제(unclaim) → 기기 버튼 10초 → 새 claim 순서다. 기기만 초기화해도 서버는 기기를 `CLAIMED`로 기억하고, 새 claim은 `409`로 거부된다. 이는 남이 기기를 초기화해도 소유권을 가져갈 수 없게 하는 보호다.
- 기기 전원을 켤 때 BOOT 버튼이 눌려 있으면 다운로드 모드로 들어간다. 사용자에게 버튼은 전원이 켜진 뒤에 누르라고 안내한다.

## 7. 기기가 스스로 하는 일

앱이 알아 두면 오류 안내에 도움이 되는 동작이다.

- **토큰이 없는 기기(claim 전)**는 Wi-Fi가 **5분 연속** 끊겨 있으면 Wi-Fi 정보를 지우고 `PROVISIONING`으로 돌아간다. 공유기 재부팅이나 정전 직후에는 몇 분간 연결이 안 되는 것이 정상이라 시간 기준으로 판단한다. `CLAIMING` 중에는 보류한다.
- **토큰이 있는 기기(`ACTIVE`)**는 Wi-Fi가 오래 끊겨도 자동으로 정보를 지우거나 BLE를 열지 않는다. 재접속만 계속 시도한다. 복구는 앱의 "Wi-Fi 다시 설정" 안내와 기기 버튼(3초)으로 한다.
- 기기는 `ACTIVE`에서 서버가 `403`을 줘도 토큰을 지우지 않는다.
- 센서 값은 10분마다 올라간다. 앱의 오프라인 판정은 **마지막 수신 후 25분**이다(앱 쪽 규칙).

## 8. 보안상 지킬 것

- PIN은 앱 서버나 로그에 저장하지 않는다. 세션을 여는 데만 쓴다.
- PIN을 모르면 `deviceId`를 알아도 세션을 열 수 없다. 이것이 PIN의 목적이다. 없으면 BLE 범위 안의 누구든 자기 `claimToken`을 밀어 넣어 미등록 기기를 가져가거나 Wi-Fi 자격증명을 바꿀 수 있다.
- 라벨 PIN을 잃어버렸을 때의 복구 절차는 아직 정해지지 않았다(`AGENTS.md`의 Open Decisions).

## 9. 아직 확정되지 않은 것

- 기기 시험 전이다. PIN을 쓰는 세션에서 Wi-Fi 스캔이 되는지도 시험 때 다시 확인한다.
- 마지막 claim 실패 원인을 알려 주는 읽기 전용 BLE 엔드포인트(`device-status`)는 보류 중이다. 필요하면 요청해 달라.
- BLE 광고를 항상 켜 둘지 시간이나 버튼으로 제한할지는 미정이다.
