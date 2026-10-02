# Provisioning (BLE Wi-Fi 설정, ID/PIN, device-info)

앱 개발자용으로 풀어 쓴 설명은 [app-ble-guide.md](app-ble-guide.md)에 있다.

Provisioning의 목적은 ESP가 사용할 Wi-Fi credential을 앱으로부터 전달받는 것이다.

기본 흐름:

```text
ESP provisioning mode 진입
    ↓
BLE advertising
    ↓
App이 ESP 발견
    ↓
BLE connection
    ↓
Wi-Fi credentials 전달
    ↓
ESP가 Wi-Fi 연결 시도
    ↓
성공
    ↓
CONNECTING / WIFI_CONNECTED
```

Provisioning과 claim은 서로 다른 개념이다.

```text
Provisioning
= ESP가 네트워크에 연결될 수 있도록 설정하는 과정

Claim
= ESP를 특정 사용자 계정의 기기로 등록하는 과정
```

Wi-Fi 연결 성공이 곧 claim 성공을 의미하지 않는다.

## BLE 기기 이름, ID, PIN(PoP)

BLE 접속에는 서로 다른 두 값이 쓰인다.

```text
ID  (deviceId)
= 어느 기기인지 구분하는 이름표. 비밀이 아니다. BLE 이름에 그대로 노출된다.

PIN (= PoP, Proof of Possession)
= 그 기기에 접속할 자격을 증명하는 비밀. 기기 NVS와 기기에 붙은 라벨에만 있다.
```

사용자에게 보이는 이름은 PIN이고, 코드와 ESP-IDF 용어는 PoP다. 같은 값이다.
`deviceToken`, `claimToken`은 이 값들과 다르다. 그 두 개는 서버와의 HTTPS 구간에서만 쓰이고,
ID와 PIN은 앱과 기기 사이의 BLE 구간에서만 쓰인다.

**BLE 기기 이름 (확정)**

```text
PROV_<deviceId>      예: PROV_D40592E7D168
```

앱은 `PROV_` 접두사로 기기를 찾고, 뒤쪽 `deviceId`로 어느 기기인지 구분한다.

**PIN(PoP) 방식 (확정, 구현됨, 기기 시험 전)**

```text
형식: 숫자 8자리 (예: 48201937)
생성: 첫 부팅 때 esp_random()으로 한 번 생성해 NVS에 저장한다. 이미 있으면 새로 만들지 않는다.
공개: 시리얼 로그로 출력한다. 기기를 만드는 사람이 이 값을 라벨에 옮겨 적는다.
초기화: 공장 초기화(factory-reset.md)로 지우지 않는다.
```

기기에 붙이는 라벨은 ID와 PIN을 모두 적는다.

```text
Leafie 센서
ID : D40592E7D168
PIN: 48201937
```

앱 사용 흐름:

```text
BLE 검색 → PROV_<deviceId> 목록 표시
    ↓
사용자가 라벨의 ID와 같은 기기를 선택 (근처에 한 대뿐이면 생략 가능)
    ↓
사용자가 라벨의 PIN을 입력 (QR 스캔은 쓰지 않는다)
    ↓
앱이 PIN을 PoP로 사용해 protocomm 세션을 연다 (NETWORK_PROV_SECURITY_1)
```

PIN을 모르는 상대는 `deviceId`를 알아도 provisioning 세션을 열 수 없다. 이 PIN이 없으면
BLE 범위 안의 누구든 자기 계정으로 만든 `claimToken`을 밀어 넣어 아직 claim되지 않은 기기를
먼저 가져가거나 Wi-Fi 자격증명을 바꿔치기할 수 있다.

PIN 생성 시점에는 Wi-Fi/BLE가 아직 꺼져 있어 `esp_random()`이 진짜 난수가 아니다. 그래서 생성할 때만
`bootloader_random_enable()`/`bootloader_random_disable()`로 SAR ADC 잡음 엔트로피를 켠다. 토양 센서 ADC 초기화보다 먼저 끝나야 한다.

고정 PoP(`leafie_pop`)는 코드에서 제거했다. `tools/phone_sim.py`는 `--pop` 또는 `PROV_POP`으로 PIN을 받는다.

## BLE `device-info` 응답

앱은 BLE에 연결하고 PIN으로 세션을 연 뒤 `device-info`로 기기가 어떤 상황인지 알 수 있다(BLE 방송에는 이름
`PROV_<deviceId>`만 있다). `device-info`는 `PROVISIONING`, `WAITING_CLAIM` 두 BLE 세션 모두에 등록한다.

```json
{ "deviceId": "D40592E7D168", "state": "WAITING_CLAIM", "hasDeviceToken": false }
```

| state | hasDeviceToken | 의미 | 앱이 할 일 |
|---|---|---|---|
| `PROVISIONING` | `false` | 처음 등록(Wi-Fi 정보 없음) | Wi-Fi 전달 → 이후 claim |
| `PROVISIONING` | `true` | 버튼으로 Wi-Fi를 재설정한 기기 | Wi-Fi만 전달. claim 불필요 |
| `WAITING_CLAIM` | `false` | Wi-Fi는 연결됨. claim 대기 | claimToken 전달 |

앱의 판단 규칙:

```text
Wi-Fi 정보 입력 창  : state == "PROVISIONING" 일 때 띄운다 (hasDeviceToken과 무관).
claim 단계 필요 여부 : hasDeviceToken == false 이면 Wi-Fi 연결 뒤 claim까지 진행한다.
                     true 이면 Wi-Fi만 받으면 ACTIVE가 되므로 claim은 하지 않는다.
```

claimToken을 보낸 뒤에는 서버의 claim 상태와 BLE 방송을 함께 본다. claim이 성공하면 기기는 BLE를 다시 켜지 않으므로
`PROV_<deviceId>` 방송의 재등장은 기기 쪽 실패(`CLAIM_FAILED` 또는 재부팅)를 뜻한다.

| 서버 `GET claim` | BLE 방송 | 의미 |
|---|---|---|
| `COMPLETED` | 없음 | 성공 |
| `PENDING` | 다시 나타남 | 기기가 포기함. 연결해 `device-info.state`로 분기한다 |
| `PENDING` | 없음 | `CLAIMING` 진행 중(최대 약 90초). 약 2분이 지나도 같으면 오류로 안내한다 |

방송이 다시 나타나 `device-info`를 읽었을 때:

| state | 의미 | 앱 동작 |
|---|---|---|
| `WAITING_CLAIM` | Wi-Fi 정보는 남아 있고 claim만 실패 | 새 claim을 발급하고(서버가 이전 PENDING을 취소) 토큰을 다시 보낸다 |
| `PROVISIONING` (`hasDeviceToken: false`) | Wi-Fi 정보가 지워짐(5분 규칙 또는 사용자의 버튼 조작) | Wi-Fi 정보를 먼저 전달하고, 연결된 뒤 **새 claim**으로 진행한다 |

`PROVISIONING`으로 돌아간 기기는 이전 claimToken을 이미 버렸다(RAM에만 있었음). 앱은 항상 새 claim을 발급한다.

**예외: `WAITING_CLAIM`인데 Wi-Fi가 이미 불량인 경우.** Wi-Fi가 붙은 뒤 공유기나 비밀번호가 바뀌었고 5분 규칙이 아직
동작하기 전이면, 기기는 Wi-Fi 정보를 들고 있어서 `device-info`로 구분할 수 없다. 이때 claim은 계속 실패한다.
앱은 같은 기기에서 claim이 **2~3번 연속 실패**하면 "Wi-Fi 설정이 바뀌었을 수 있습니다. 기기 버튼을 3초 눌러
Wi-Fi를 다시 설정하세요"로 안내한다. 5분이 지나면 기기가 스스로 `PROVISIONING`으로 돌아가기도 한다([state-machine.md](state-machine.md)).

## Wi-Fi 실패 시 재수신

PROVISIONING 중 비밀번호가 틀리거나 AP를 못 찾으면 앱은 상태 조회에서 `AuthError`/`NetworkNotFound`를 받는다.
기기는 `CRED_FAIL`에서 `network_prov_mgr_reset_wifi_sm_state_on_failure()`를 호출하므로 앱은 같은 BLE 연결에서
Wi-Fi 정보를 다시 보낼 수 있다. 재부팅은 필요 없다.

## Wi-Fi 목록 스캔

앱이 보여 줄 Wi-Fi 목록은 기기가 스캔한 결과를 쓴다. ESP-IDF provisioning 매니저의 `prov-scan` 엔드포인트가
BLE로 목록(SSID, RSSI, 채널, 보안 방식)을 돌려준다. 별도 구현은 없다.

* CLI(`esp_prov.py`) 시험에서 기기가 보낸 SSID 목록을 받아 선택한 네트워크로 연결되는 것을 확인했다.
  그 시험은 PIN 도입 전 펌웨어(고정 PoP)로 했다. PIN 적용 후 세션에서의 스캔은 기기 시험 때 다시 확인한다.
* iOS는 일반 앱이 주변 Wi-Fi 목록을 읽을 수 없어 앱 스캔은 쓰지 않는다. 비밀번호는 사용자가 입력한다.
* 목록에 없는 숨김 SSID는 앱에서 직접 입력할 수 있게 한다.
* ESP32-C3는 2.4GHz 전용(802.11 b/g/n)이다. 5GHz 네트워크는 스캔 목록에도 나오지 않는다.
