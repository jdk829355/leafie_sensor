# Hardware / 보드 / 빌드 설정

## MCU

```text
ESP32-C3 (SuperMini 계열 보드, HW-466AB)
```

## 연결 센서

```text
조도 센서 (BH1750)
토양 수분 센서 (Capacitive Soil Moisture Sensor V2.0.0)
```

## 인터페이스 및 GPIO pin

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

## 센서 동작 설정

```text
토양 수분 센서
→ ADC1_CH0(GPIO0), ADC_ATTEN_DB_12, 12bit(0~4095)
→ 값이 클수록 건조하다 (공기 중 약 3900, 물에 담그면 하락)
→ 펌웨어는 raw 값만 올리고 %로 변환하지 않는다 (마른 값/젖은 값 기준은 서버에서 정한다)

조도 센서 (BH1750)
→ I2C 주소 0x23 (ADDR 핀 미연결), 연속 고해상도 모드(0x10)
→ lux = raw(16bit) / 1.2
```

## 보드 특성: Wi-Fi TX 출력

ESP32-C3 SuperMini는 기본 TX 출력(20dBm)에서 AP로 보낸 인증 프레임이 닿지 않아,
수신 신호가 강한데도(RSSI -30~-40) `reason=2`(AUTH_EXPIRE)로 계속 접속에 실패한다.
`WIFI_EVENT_STA_START` 핸들러에서 `esp_wifi_set_max_tx_power(34)`(8.5dBm, 단위 0.25dBm)로
낮춰야 접속된다. provisioning 경로와 재접속 경로 모두 이 핸들러를 거치므로 한 곳에서 처리한다.

## 빌드 설정

`sdkconfig`는 ESP-IDF 버전이 바뀌면 재생성되어 값이 유실될 수 있다.
아래 값은 `sdkconfig.defaults`에 고정한다.

```text
CONFIG_BT_ENABLED=y, CONFIG_BT_NIMBLE_ENABLED=y     BLE provisioning
CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y   NETWORK_PROV_SECURITY_1 사용
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y           BLE + Wi-Fi 바이너리가 기본 앱 파티션(1MB)을 넘음
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192                telemetry HTTPS(TLS)가 app_main 스택에서 실행됨
```

`main/secrets.h`(버전 관리 제외)에 `TELEMETRY_API_KEY`를 둔다. 파일이 없으면 빌드가 실패한다.
