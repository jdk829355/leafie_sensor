AGENTS.md

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
````

일반적인 사용자/식물/기기 관리 API는 기존 FastAPI 서버가 담당한다.

---

## 2. Hardware

MCU:

```text
ESP32-C3 (SuperMini 계열 보드, HW-466AB)
```

연결 센서:

```text
조도 센서 (BH1750)
토양 수분 센서 (Capacitive Soil Moisture Sensor V2.0.0)
```

인터페이스 및 GPIO pin:

```text
조도 센서 (BH1750)
→ I2C
→ SDA: GPIO4
→ SCL: GPIO5

토양 수분 센서 (Capacitive Soil Moisture Sensor V2.0.0)
→ ADC
→ GPIO0
```

핀 선정 근거:

```text
GPIO2, GPIO8, GPIO9는 ESP32-C3의 strapping pin이므로 상시 배선 대상에서 제외한다.
GPIO8/GPIO9는 보드 실크스크린상 기본 I2C(SDA/SCL)로 표기되어 있으나,
온보드 LED(GPIO8) / BOOT 버튼(GPIO9)과 공유되어 사용하지 않는다.
ADC는 ESP32-C3에서 GPIO0~GPIO4(ADC1)만 지원하므로 이 범위 내에서 선택한다.
```

센서 동작 설정:

```text
토양 수분 센서
→ ADC1_CH0(GPIO0), ADC_ATTEN_DB_12, 12bit(0~4095)
→ 값이 클수록 건조하다 (공기 중 약 3900, 물에 담그면 하락)
→ 펌웨어는 raw 값만 올리고 %로 변환하지 않는다 (마른 값/젖은 값 기준은 서버에서 정한다)

조도 센서 (BH1750)
→ I2C 주소 0x23 (ADDR 핀 미연결), 연속 고해상도 모드(0x10)
→ lux = raw(16bit) / 1.2
```

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

### 보드 특성: Wi-Fi TX 출력

ESP32-C3 SuperMini는 기본 TX 출력(20dBm)에서 AP로 보낸 인증 프레임이 닿지 않아,
수신 신호가 강한데도(RSSI -30~-40) `reason=2`(AUTH_EXPIRE)로 계속 접속에 실패한다.
`WIFI_EVENT_STA_START` 핸들러에서 `esp_wifi_set_max_tx_power(34)`(8.5dBm, 단위 0.25dBm)로
낮춰야 접속된다. provisioning 경로와 재접속 경로 모두 이 핸들러를 거치므로 한 곳에서 처리한다.

### 빌드 설정

`sdkconfig`는 ESP-IDF 버전이 바뀌면 재생성되어 값이 유실될 수 있다.
아래 값은 `sdkconfig.defaults`에 고정한다.

```text
CONFIG_BT_ENABLED=y, CONFIG_BT_NIMBLE_ENABLED=y     BLE provisioning
CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y   NETWORK_PROV_SECURITY_1 사용
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y           BLE + Wi-Fi 바이너리가 기본 앱 파티션(1MB)을 넘음
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192                telemetry HTTPS(TLS)가 app_main 스택에서 실행됨
```

`main/secrets.h`(버전 관리 제외)에 `TELEMETRY_API_KEY`를 둔다. 파일이 없으면 빌드가 실패한다.

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
provisioning PoP (BLE 접속 PIN, 9번 섹션)
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

---

# 6. Main Device State Machine

상위 상태머신은 기기의 전체 lifecycle만 관리한다.

현재 상태:

```text
BOOT
PROVISIONING
CONNECTING
WIFI_CONNECTED
WAITING_CLAIM
CLAIMING
ACTIVE
```

흐름:

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

`ACTIVE`에서 Wi-Fi가 끊겨도 상태는 `ACTIVE`로 유지한다. 재접속만 계속 시도하고 텔레메트리는 실패하면 버린다(19번 섹션).
`CONNECTING`으로 되돌리지 않는 이유는 `CONNECTING`이 BLE와 5분 규칙(아래)의 대상 상태이기 때문이다.

버튼(8번 섹션)은 어느 상태에서도 동작하는 global event이며 상태 전이가 아니라 재부팅이다.

`CONNECTING`에서의 재시도는 별도의 `RECONNECTING` 상태로 만들지 않는다.

재시도 시 행동이 달라지는 요구가 생기기 전까지 `CONNECTING` 내부에서 처리한다.

Wi-Fi 연결이 오래 실패할 때의 처리(아래 "Wi-Fi 장기 실패 처리")도 새 state를 만들지 않고 이 규칙 안에서 다룬다.

---

## 7. Main State Transition Table

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

### Wi-Fi 장기 실패 처리 (확정, 토큰 없는 경우만 구현됨, 기기 시험 전)

Wi-Fi가 연속으로 끊겨 있는 시간이 **5분**을 넘으면 `deviceToken` 유무에 따라 다르게 처리한다.
연결에 성공하면 이 시간은 0으로 되돌린다. 서버 오류(5xx 등)로 claim이나 업로드가 실패하는 것은 대상이 아니다.
Wi-Fi가 끊긴 상태만 센다.

| deviceToken | 5분 이상 연속 끊김 시 동작 |
|---|---|
| 없음 (claim 전: `CONNECTING`, `WAITING_CLAIM`) | 지킬 것이 없으므로 Wi-Fi 정보를 지우고 `PROVISIONING`으로 돌아간다. |
| 있음 (`ACTIVE` 계열) | **자동으로 처리하지 않는다.** 재접속을 계속 시도하며, 복구는 사용자가 앱의 "Wi-Fi 다시 설정"과 기기 버튼(8번 섹션, 3초)으로 한다. |

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
  API 키나 Authorizer 설정 오류 한 번이 전체 기기의 등록을 지울 수 있기 때문이다. 소유 이전은 공장 초기화(8번)로 한다.
```

Wi-Fi가 필요한 다른 상태에서도 `WIFI_DISCONNECTED`가 발생할 수 있다. global event로 만들지 않고 위 표와 같이
상태별로 처리한다: `CONNECTING`은 재시도, `ACTIVE`는 재접속만 하고 상태 유지, 토큰이 없는 상태는 5분 규칙을 적용한다.

---

# 8. Factory Reset

공장 초기화는 특정 상태가 아니라 global event로 취급한다.

공장 초기화 시 최소한 다음 정보를 삭제한다.

```text
Wi-Fi credentials
deviceToken
진행 중인 claim 관련 임시 데이터
```

다음 값은 삭제하지 않는다.

```text
deviceId (MAC에서 계산되므로 어차피 동일)
provisioning PoP
```

공유기나 Wi-Fi 비밀번호를 바꾸는 것은 공장 초기화가 아니다. 그 경우는 9번 섹션의 "Wi-Fi 장기 실패 처리"로
`deviceToken`을 유지한 채 Wi-Fi 정보만 새로 받는다. 서버에서 기기를 삭제(unclaim)할 필요가 없다.

**트리거 (확정, 구현됨, 기기 시험 전)**: BOOT 버튼(GPIO9, 누르면 LOW) 누름 시간으로 구분한다.
어느 상태에서도 동작해야 하므로 부팅 직후부터 버튼을 감시한다. 보드에 이미 있는 버튼을 입력으로 읽는 것이며
센서를 새로 배선하는 것이 아니다(2번 섹션의 GPIO9 상시 배선 제외 원칙과 충돌하지 않는다).

```text
3초 이상 ~ 10초 미만에서 놓음   Wi-Fi 재설정  Wi-Fi 정보만 지우고 재부팅. deviceToken 유지.
10초 이상 누름 (누르는 중)       공장 초기화   Wi-Fi 정보와 deviceToken 삭제 후 재부팅.
```

- Wi-Fi 재설정은 놓을 때 판정한다. 누르는 중에 3초에서 실행하면 10초까지 누르려는 사람이 중간에 재설정된다.
- LED(GPIO8) 피드백: 3초가 지나면 2번 깜빡인다(지금 놓으면 Wi-Fi 재설정). 10초가 되면 5번 깜빡이고 초기화한다.
- Wi-Fi 재설정 후에는 `deviceToken`이 있으므로 BLE(PROVISIONING)로 Wi-Fi만 받으면 claim 없이 바로 `ACTIVE`가 된다.

**서버와의 관계**: 기기에서 공장 초기화를 해도 서버는 기기를 `CLAIMED`로 기억한다. 서버는 `CLAIMED` 기기에 대한
새 claim을 거부(`409`)하므로 소유자 변경이나 재등록은 앱에서 기기를 삭제한 뒤 공장 초기화하고 새 claim을 만드는 순서로 한다.
이 거부는 남이 기기를 초기화해도 소유권을 가져갈 수 없게 하는 보호이기도 하다.

PoP는 기기에 붙은 라벨에 인쇄된 값이다. 지우면 라벨과 어긋나 기기를 쓸 수 없게 된다.

이후 ESP를 reboot한다.

```text
FACTORY_RESET
    ↓
persistent configuration 삭제
    ↓
REBOOT
    ↓
BOOT
    ↓
Wi-Fi credential 없음
    ↓
PROVISIONING
```

`ACTIVE → PROVISIONING`으로 직접 jump하는 구현보다 reboot 후 정상 boot flow를 재사용하는 것을 우선한다.

---

# 9. Provisioning

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

### BLE 기기 이름, ID, PIN(PoP)

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
초기화: 공장 초기화(8번)로 지우지 않는다.
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

### BLE `device-info` 응답

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
Wi-Fi를 다시 설정하세요"로 안내한다. 5분이 지나면 기기가 스스로 `PROVISIONING`으로 돌아가기도 한다(7번 섹션).

### Wi-Fi 실패 시 재수신

PROVISIONING 중 비밀번호가 틀리거나 AP를 못 찾으면 앱은 상태 조회에서 `AuthError`/`NetworkNotFound`를 받는다.
기기는 `CRED_FAIL`에서 `network_prov_mgr_reset_wifi_sm_state_on_failure()`를 호출하므로 앱은 같은 BLE 연결에서
Wi-Fi 정보를 다시 보낼 수 있다. 재부팅은 필요 없다.

### Wi-Fi 목록 스캔

앱이 보여 줄 Wi-Fi 목록은 기기가 스캔한 결과를 쓴다. ESP-IDF provisioning 매니저의 `prov-scan` 엔드포인트가
BLE로 목록(SSID, RSSI, 채널, 보안 방식)을 돌려준다. 별도 구현은 없다.

* CLI(`esp_prov.py`) 시험에서 기기가 보낸 SSID 목록을 받아 선택한 네트워크로 연결되는 것을 확인했다.
  그 시험은 PIN 도입 전 펌웨어(고정 PoP)로 했다. PIN 적용 후 세션에서의 스캔은 기기 시험 때 다시 확인한다.
* iOS는 일반 앱이 주변 Wi-Fi 목록을 읽을 수 없어 앱 스캔은 쓰지 않는다. 비밀번호는 사용자가 입력한다.
* 목록에 없는 숨김 SSID는 앱에서 직접 입력할 수 있게 한다.
* ESP32-C3는 2.4GHz 전용(802.11 b/g/n)이다. 5GHz 네트워크는 스캔 목록에도 나오지 않는다.

---

# 10. Device Claim

Claim은 별도 subprocess로 관리한다.

상위 상태머신은 claim의 세부 retry 과정을 알 필요가 없다.

상위에서는 다음 결과만 처리한다.

```text
CLAIM_SUCCESS
CLAIM_FAILED
```

---

## 11. Claim Flow

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

---

# 12. Claim Tokens

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

---

# 13. Claim Sub-State Machine

Claim subprocess 내부 상태:

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

---

## 14. Claim Transition Table

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

---

# 15. Claim Error Policy

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

---

# 16. Backend Claim API

현재 예상 API:

```http
POST /api/v1/sensor-devices/{deviceId}/claims
POST /api/v1/sensor-device-claims/{claimToken}/complete
GET  /api/v1/sensor-device-claims/{claimToken}
```

FastAPI 기준 경로다. 푸시 설치용 `POST /api/v1/devices`와 구분한다.
telemetry의 `POST /devices/{deviceId}/telemetry`는 API Gateway라 이 경로를 바꾸지 않는다.

### POST /api/v1/sensor-devices/{deviceId}/claims

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

---

### POST /api/v1/sensor-device-claims/{claimToken}/complete

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
  불일치는 재시도 불가 오류(15번 섹션의 `deviceId mismatch`)다.
* `deviceId`는 `device-info` endpoint로 앱에 준 값과 같은 12자리 대문자 hex(4번 섹션)다.

이 API는 네트워크 응답 유실 후 ESP가 재시도할 수 있도록 idempotent하게 설계해야 한다.

**재시도 정책 (확정)**: 이미 `COMPLETED`된 claim이 원래 발급 시각 기준 5분(claim TTL)
이내에 같은 `claimToken`으로 다시 호출되면, 서버는 새 `deviceToken`을 발급하고 이전
`deviceToken`을 무효화한다. 5분이 지난 뒤의 재호출은 `410`이다. 동시에 같은 claim이
완료되면(예: 응답 유실로 인한 재시도와 겹침) 서버가 하나만 실제로 소유권을 확정하고,
나머지 요청도 동일한 재발급 경로를 타 유효한 `deviceToken`을 받는다.

ESP 입장에서 이 정책은 상태 머신에 영향이 없다: 매 성공 응답마다 받은 `deviceToken`을
그대로 NVS에 덮어써 저장하면 된다(13번 섹션 그대로, 별도 state 불필요 — 20번 섹션
원칙 3, 4). 이전에 NVS 저장을 시도했다가 실패한 `deviceToken`은 애초에 저장되지
않았으므로 무효화 여부를 신경 쓸 필요가 없다.

---

### GET /api/v1/sensor-device-claims/{claimToken}

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

---

# 17. Backend Database

Claim에 필요한 최소 핵심 테이블:

```text
sensor_devices
sensor_device_claims
```

### sensor_devices

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

### sensor_device_claims

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

### 구현된 스키마 (Leafie PR #94)

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

---

# 18. BLE Responsibilities

BLE는 기본적으로 초기 설정 과정에만 사용한다.

현재 BLE를 통해 필요한 논리 기능:

```text
기기 발견
Wi-Fi provisioning
deviceId 전달
claimToken 전달
```

claim 결과는 BLE로 전달하지 않는다.

앱은 `GET /api/v1/sensor-device-claims/{claimToken}`(16번 섹션)를 polling하여 claim 결과를 서버로부터 직접 확인한다.

claim 시작은 논리적으로 다음 이벤트로 표현한다.

```text
START_CLAIM(claimToken)
```

ESP가 이 이벤트를 받으면:

```text
WAITING_CLAIM
    ↓
CLAIMING
```

으로 전이한다.

BLE implementation은 `wifi_prov_mgr`(ESP-IDF Wi-Fi provisioning manager)의 protocomm 세션과 custom endpoint를 사용한다.

별도의 raw GATT service/characteristic은 추가하지 않는다.

`wifi_prov_mgr`는 `PROVISIONING` 상태뿐 아니라 `WAITING_CLAIM` 상태에서도 사용한다.

```text
WAITING_CLAIM 상태 진입
    ↓
(이미 실행 중이 아니면) wifi_prov_mgr 재시작 → BLE advertising 시작
```

기기가 재부팅으로 `PROVISIONING`을 거치지 않고 바로 `WAITING_CLAIM`에 도달하는 경우에도

(Wi-Fi credential은 있지만 deviceToken이 없는 경우, 7번 섹션 참고)

claimToken을 받을 수 있어야 하므로, 이 경로에서도 BLE가 켜져 있어야 한다.

Custom endpoint:

```text
device-info
→ App → ESP 요청
→ ESP → App 응답: { "deviceId": "..." }

claim
→ App → ESP 요청: { "claimToken": "..." }
→ ESP → App 응답: { "status": "ok" }
→ ESP 내부적으로 START_CLAIM(claimToken) 이벤트 발생
```

payload는 JSON을 사용한다.

기본 제공 endpoint(`prov-session`, `prov-config` 등)와 달리 custom endpoint의 payload는 protocomm이 opaque byte로 취급하므로, protobuf 없이 JSON으로 직접 파싱한다.

직접 GATT service/characteristic을 추가하는 것은 실제 요구사항이 확인된 경우에만 수행한다.

---

# 19. Telemetry

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

로컬 버퍼링/큐잉은 구현하지 않는다 (22번 섹션 Non-Goals의 "고급 offline telemetry buffering"과 일치).

### Endpoint

```text
POST {TELEMETRY_BASE_URL}/devices/{deviceId}/telemetry
```

* `TELEMETRY_BASE_URL`은 API Gateway(REST API)의 stage까지 포함한 주소다. stage가 빠지면 `403 Forbidden`이 된다.
* claim, ping용 백엔드 주소(`MOCK_SERVER_BASE_URL`)와는 별개이며, telemetry만 API Gateway로 보낸다.
* HTTPS이며 서버 인증서는 ESP-IDF 인증서 번들(`crt_bundle_attach`)로 검증한다.
* 이 경로는 API Gateway에만 있다. FastAPI의 푸시 설치 `POST /api/v1/devices`와 호스트가 달라 경로를 같이 두지 않는다.

### Headers

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

### Body

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

---

# 20. Design Principles

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

# 21. Implementation Order

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

# 22. Non-Goals for Initial Implementation

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

# 23. Open Decisions

19번 섹션의 telemetry payload 형식은 확정되었다.

현재 확정되지 않은 사항:

* provisioning BLE를 항상 켜 둘지, 부팅 후 일정 시간이나 버튼 입력으로 제한할지(9번 섹션의 PIN과 별개의 추가 방어).
  18번 섹션은 `WAITING_CLAIM`에서 BLE를 켜므로 그 설계와 함께 정한다.
* 마지막 claim 실패 원인을 알려 주는 읽기 전용 BLE 엔드포인트(`device-status`). 앱에서 필요가 확인될 때까지 보류한다.
* 앱의 오프라인 판정 기준은 마지막 수신 후 25분이다(텔레메트리 주기 10분 기준). 앱 쪽 규칙이며 기기와 무관하다.
* 라벨 PIN을 잃어버렸을 때의 복구 절차(지금은 시리얼 로그를 보거나 NVS를 지우고 새로 만드는 수밖에 없다).
* telemetry가 `403`(deviceToken 거부)으로 계속 실패할 때 기기의 동작(예: NVS의 deviceToken을 지우고 `WAITING_CLAIM`으로 돌아갈지). 지금은 로그만 남긴다.

(각 섹션에 개별적으로 명시된 미확정 항목은 별도.)

구현 과정에서 새로운 미확정 항목이 필요해지면 임의로 결정하기 전에 명시적으로 결정하고 문서에 반영할 것.