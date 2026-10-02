# Factory Reset

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

공유기나 Wi-Fi 비밀번호를 바꾸는 것은 공장 초기화가 아니다. 그 경우는 [state-machine.md](state-machine.md)의 "Wi-Fi 장기 실패 처리"로
`deviceToken`을 유지한 채 Wi-Fi 정보만 새로 받는다. 서버에서 기기를 삭제(unclaim)할 필요가 없다.

## 트리거 (확정, 구현됨, 기기 시험 전)

BOOT 버튼(GPIO9, 누르면 LOW) 누름 시간으로 구분한다.
어느 상태에서도 동작해야 하므로 부팅 직후부터 버튼을 감시한다. 보드에 이미 있는 버튼을 입력으로 읽는 것이며
센서를 새로 배선하는 것이 아니다([hardware.md](hardware.md)의 GPIO9 상시 배선 제외 원칙과 충돌하지 않는다).

```text
3초 이상 ~ 10초 미만에서 놓음   Wi-Fi 재설정  Wi-Fi 정보만 지우고 재부팅. deviceToken 유지.
10초 이상 누름 (누르는 중)       공장 초기화   Wi-Fi 정보와 deviceToken 삭제 후 재부팅.
```

- Wi-Fi 재설정은 놓을 때 판정한다. 누르는 중에 3초에서 실행하면 10초까지 누르려는 사람이 중간에 재설정된다.
- LED(GPIO8) 피드백: 3초가 지나면 2번 깜빡인다(지금 놓으면 Wi-Fi 재설정). 10초가 되면 5번 깜빡이고 초기화한다.
- Wi-Fi 재설정 후에는 `deviceToken`이 있으므로 BLE(PROVISIONING)로 Wi-Fi만 받으면 claim 없이 바로 `ACTIVE`가 된다.

## 서버와의 관계

기기에서 공장 초기화를 해도 서버는 기기를 `CLAIMED`로 기억한다. 서버는 `CLAIMED` 기기에 대한
새 claim을 거부(`409`)하므로 소유자 변경이나 재등록은 앱에서 기기를 삭제한 뒤 공장 초기화하고 새 claim을 만드는 순서로 한다.
이 거부는 남이 기기를 초기화해도 소유권을 가져갈 수 없게 하는 보호이기도 하다.

PoP는 기기에 붙은 라벨에 인쇄된 값이다. 지우면 라벨과 어긋나 기기를 쓸 수 없게 된다.

## 재부팅

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
