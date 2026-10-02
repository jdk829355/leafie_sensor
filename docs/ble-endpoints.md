# BLE Responsibilities / Custom Endpoint

BLE는 기본적으로 초기 설정 과정에만 사용한다.

현재 BLE를 통해 필요한 논리 기능:

```text
기기 발견
Wi-Fi provisioning
deviceId 전달
claimToken 전달
```

claim 결과는 BLE로 전달하지 않는다.

앱은 `GET /api/v1/sensor-device-claims/{claimToken}`([claim-api.md](claim-api.md))를 polling하여 claim 결과를 서버로부터 직접 확인한다.

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

(Wi-Fi credential은 있지만 deviceToken이 없는 경우, [state-machine.md](state-machine.md) 참고)

claimToken을 받을 수 있어야 하므로, 이 경로에서도 BLE가 켜져 있어야 한다.

## Custom endpoint

```text
device-info
→ App → ESP 요청
→ ESP → App 응답: { "deviceId": "..." }

claim
→ App → ESP 요청: { "claimToken": "..." }
→ ESP → App 응답: { "status": "ok" }
→ ESP 내부적으로 START_CLAIM(claimToken) 이벤트 발생
```

`device-info` 응답에는 `state`, `hasDeviceToken`도 포함된다. 현재 형식은 [provisioning.md](provisioning.md)를 본다.

payload는 JSON을 사용한다.

기본 제공 endpoint(`prov-session`, `prov-config` 등)와 달리 custom endpoint의 payload는 protocomm이 opaque byte로 취급하므로, protobuf 없이 JSON으로 직접 파싱한다.

직접 GATT service/characteristic을 추가하는 것은 실제 요구사항이 확인된 경우에만 수행한다.
