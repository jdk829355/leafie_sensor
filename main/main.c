#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_crt_bundle.h"
#include "esp_efuse.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "bootloader_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "secrets.h"

// TODO: 테스트용 값. 실제 서비스 결정 시 재검토.
#define MOCK_SERVER_BASE_URL "http://192.168.35.115:8080" // claim/ping용 로컬 백엔드
#define TELEMETRY_BASE_URL "https://1itdoce5k6.execute-api.ap-northeast-2.amazonaws.com/main" // API Gateway (telemetry 전용)

#define NVS_NAMESPACE "leafie"
#define NVS_KEY_DEVICE_TOKEN "device_token"
#define NVS_KEY_PROV_POP "prov_pop"
#define NVS_NAMESPACE_WIFI "nvs.net80211" // esp_wifi가 STA 자격증명을 저장하는 namespace

// docs/factory-reset.md: BOOT 버튼(GPIO9, 누르면 LOW). 놓을 때 3초 이상이면 Wi-Fi 재설정, 10초가 되면 공장 초기화.
#define RESET_BUTTON_GPIO GPIO_NUM_9
#define STATUS_LED_GPIO GPIO_NUM_8 // 온보드 LED. 극성과 무관하게 깜빡임으로만 피드백한다.
#define BUTTON_WIFI_RESET_MS 3000
#define BUTTON_FACTORY_RESET_MS 10000

// docs/state-machine.md: Wi-Fi가 이 시간 이상 연속으로 끊겨 있으면 장기 실패로 본다.
#define WIFI_LONG_FAILURE_US (5LL * 60 * 1000 * 1000)

#define CLAIM_MAX_RETRY 5
static const int CLAIM_BACKOFF_MS[CLAIM_MAX_RETRY] = { 2000, 4000, 8000, 16000, 32000 };

static const char *TAG = "leafie";

// docs/state-machine.md의 상위 상태머신. CLAIMING/ACTIVE는 claim subprocess가
// 아직 없어서 도달하지 않지만, 다음 단계에서 이어붙일 자리로 남겨둔다.
typedef enum {
    DEVICE_STATE_BOOT,
    DEVICE_STATE_PROVISIONING,
    DEVICE_STATE_CONNECTING,
    DEVICE_STATE_WIFI_CONNECTED,
    DEVICE_STATE_WAITING_CLAIM,
    DEVICE_STATE_CLAIMING,
    DEVICE_STATE_ACTIVE,
} device_state_t;

static const char *device_state_name(device_state_t state)
{
    switch (state) {
    case DEVICE_STATE_BOOT: return "BOOT";
    case DEVICE_STATE_PROVISIONING: return "PROVISIONING";
    case DEVICE_STATE_CONNECTING: return "CONNECTING";
    case DEVICE_STATE_WIFI_CONNECTED: return "WIFI_CONNECTED";
    case DEVICE_STATE_WAITING_CLAIM: return "WAITING_CLAIM";
    case DEVICE_STATE_CLAIMING: return "CLAIMING";
    case DEVICE_STATE_ACTIVE: return "ACTIVE";
    default: return "UNKNOWN";
    }
}

static device_state_t s_state = DEVICE_STATE_BOOT;

static void device_set_state(device_state_t next)
{
    ESP_LOGI(TAG, "state: %s -> %s", device_state_name(s_state), device_state_name(next));
    s_state = next;
}

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static char s_device_id[13]; // 12 hex chars + null terminator

// BLE device-info 응답용 (docs/provisioning.md). BLE를 여는 쪽에서 모드를 정한다.
static const char *s_ble_mode = "PROVISIONING";
static bool s_has_device_token;
static volatile bool s_wifi_connected;

static void generate_device_id(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "deviceId: %s", s_device_id);
}

// docs/provisioning.md: BLE provisioning 접속 PIN(PoP). 숫자 8자리.
// 첫 부팅 때 한 번 만들어 NVS에 저장하고, 이후에는 그대로 쓴다. 기기 라벨에 인쇄하는 값이라
// 공장 초기화로 지우지 않는다. 라벨을 만들거나 잃어버렸을 때 확인할 수 있도록 부팅마다 로그로 출력한다.
static char s_prov_pop[9];

static void load_or_create_prov_pop(void)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));

    size_t len = sizeof(s_prov_pop);
    esp_err_t err = nvs_get_str(handle, NVS_KEY_PROV_POP, s_prov_pop, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // esp_random()은 Wi-Fi/BLE가 켜져 있을 때만 진짜 난수다. 이 시점에는 아직 RF가 꺼져 있으므로
        // SAR ADC 잡음 엔트로피를 잠깐 켠다. ADC(토양 센서)와 RF보다 먼저 쓰고 바로 끈다.
        bootloader_random_enable();
        uint32_t random_value = esp_random();
        bootloader_random_disable();
        snprintf(s_prov_pop, sizeof(s_prov_pop), "%08lu", (unsigned long)(random_value % 100000000UL));
        ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_PROV_POP, s_prov_pop));
        ESP_ERROR_CHECK(nvs_commit(handle));
        ESP_LOGI(TAG, "provisioning PIN generated");
    } else {
        ESP_ERROR_CHECK(err);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "label -> ID: %s  PIN: %s", s_device_id, s_prov_pop);
}

// network_prov_mgr_init()을 claim listener 용도로 다시 호출하면 Wi-Fi STA 설정이
// 지워지는 경우가 있어, 연결 성공 시점의 설정을 별도로 보관해 필요하면 복구한다.
static wifi_config_t s_saved_wifi_config;
static bool s_has_saved_wifi_config = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // SuperMini는 기본 TX 출력(20dBm)에서 인증 프레임이 AP에 닿지 않아
        // reason=2(AUTH_EXPIRE)로 계속 실패한다. 8.5dBm(34 * 0.25)으로 낮춰야 붙는다.
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_ERR_WIFI_SSID && s_has_saved_wifi_config) {
            ESP_LOGW(TAG, "Wi-Fi STA config lost, restoring saved config");
            esp_wifi_set_config(WIFI_IF_STA, &s_saved_wifi_config);
            err = esp_wifi_connect();
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d, ssid=%.*s, rssi=%d, bssid=" MACSTR "), reconnect result: %s",
                 event->reason, event->ssid_len, (const char *)event->ssid, event->rssi,
                 MAC2STR(event->bssid), esp_err_to_name(err));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void prov_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base != NETWORK_PROV_EVENT) {
        return;
    }
    switch (event_id) {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "Provisioning started");
        break;
    case NETWORK_PROV_WIFI_CRED_RECV:
        ESP_LOGI(TAG, "Wi-Fi credentials received");
        device_set_state(DEVICE_STATE_CONNECTING);
        break;
    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "Provisioning failed: %s",
                 *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR ? "Wi-Fi password incorrect" : "Wi-Fi AP not found");
        // 상태를 FAIL에서 되돌려야 앱이 같은 BLE 연결에서 자격증명을 다시 보낼 수 있다.
        // 이 호출은 틀린 자격증명도 지운다.
        device_set_state(DEVICE_STATE_PROVISIONING);
        network_prov_mgr_reset_wifi_sm_state_on_failure();
        break;
    }
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning successful");
        break;
    case NETWORK_PROV_END:
        // deinit은 여기서 하지 않는다. network_prov_mgr_stop_provisioning() +
        // network_prov_mgr_wait()를 부르는 코드와 동시에 deinit을 호출하면
        // manager 내부 세마포어가 깨져서 크래시난다. 호출부에서 순서대로 처리한다.
        ESP_LOGI(TAG, "Provisioning ended");
        break;
    default:
        break;
    }
}

static esp_err_t device_info_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                      uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)inbuf;
    (void)inlen;
    (void)priv_data;

    char *resp = NULL;
    int len = asprintf(&resp, "{\"deviceId\":\"%s\",\"state\":\"%s\",\"hasDeviceToken\":%s}",
                       s_device_id, s_ble_mode, s_has_device_token ? "true" : "false");
    if (len < 0) {
        return ESP_ERR_NO_MEM;
    }

    *outbuf = (uint8_t *)resp;
    *outlen = len;
    return ESP_OK;
}

// docs/claim-state-machine.md의 claim subprocess 내부 상태.
typedef enum {
    CLAIM_REQUESTING,
    CLAIM_RETRY_WAIT,
    CLAIM_SUCCESS,
    CLAIM_FAILED,
} claim_state_t;

typedef enum {
    CLAIM_REQ_SUCCESS,
    CLAIM_REQ_RETRYABLE,
    CLAIM_REQ_PERMANENT,
} claim_req_result_t;

static EventGroupHandle_t s_claim_event_group;
#define CLAIM_TOKEN_RECEIVED_BIT BIT0
static char s_claim_token[65]; // 백엔드 claimToken은 token_hex(32) = 64자 + null

static esp_err_t claim_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)priv_data;

    cJSON *root = cJSON_ParseWithLength((const char *)inbuf, inlen);
    cJSON *token_item = root ? cJSON_GetObjectItem(root, "claimToken") : NULL;
    if (!cJSON_IsString(token_item)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_claim_token, token_item->valuestring, sizeof(s_claim_token));
    cJSON_Delete(root);

    ESP_LOGI(TAG, "claimToken received via BLE: %s", s_claim_token);
    xEventGroupSetBits(s_claim_event_group, CLAIM_TOKEN_RECEIVED_BIT);

    const char *resp = "{\"status\":\"ok\"}";
    *outbuf = (uint8_t *)strdup(resp);
    *outlen = strlen(resp);
    return ESP_OK;
}

// docs/ble-endpoints.md: WAITING_CLAIM 상태에서도 claimToken을 받을 수 있어야 하므로
// BLE(network_prov_mgr)를 다시 켠다. Wi-Fi는 이미 연결돼 있으므로 여기서는 claim
// endpoint만 등록한다.
static void start_claim_listener(void)
{
    // 재시도 시 claim listener를 다시 켜야 하므로 여기서도 FREE_BTDM은 쓰지 않는다.
    network_prov_mgr_config_t prov_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_config));

    char service_name[32];
    snprintf(service_name, sizeof(service_name), "PROV_%s", s_device_id);

    s_ble_mode = "WAITING_CLAIM";
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_create("device-info"));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_create("claim"));
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, s_prov_pop, service_name, NULL));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_register("device-info", device_info_handler, NULL));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_register("claim", claim_handler, NULL));

    ESP_LOGI(TAG, "Claim BLE listener started, service name: %s", service_name);
}

// docs/claim-api.md: POST /api/v1/sensor-device-claims/{claimToken}/complete
static claim_req_result_t do_claim_complete_request(const char *claim_token,
                                                      char *device_token_out, size_t device_token_out_len)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/api/v1/sensor-device-claims/%s/complete", MOCK_SERVER_BASE_URL, claim_token);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 서버가 claim에 묶인 deviceId와 요청한 기기가 같은지 검증할 수 있도록 deviceId를 함께 보낸다.
    char request_body[48];
    int request_body_len = snprintf(request_body, sizeof(request_body), "{\"deviceId\":\"%s\"}", s_device_id);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, request_body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "claim complete: connection failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return CLAIM_REQ_RETRYABLE;
    }
    if (esp_http_client_write(client, request_body, request_body_len) != request_body_len) {
        ESP_LOGW(TAG, "claim complete: failed to send request body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return CLAIM_REQ_RETRYABLE;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char body[256] = { 0 };
    esp_http_client_read_response(client, body, sizeof(body) - 1);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "claim complete response: status=%d body=%s", status, body);

    if (status == 200) {
        cJSON *root = cJSON_Parse(body);
        cJSON *token_item = root ? cJSON_GetObjectItem(root, "deviceToken") : NULL;
        if (cJSON_IsString(token_item)) {
            strlcpy(device_token_out, token_item->valuestring, device_token_out_len);
            cJSON_Delete(root);
            return CLAIM_REQ_SUCCESS;
        }
        cJSON_Delete(root);
        ESP_LOGE(TAG, "claim complete: 200 response missing deviceToken");
        return CLAIM_REQ_PERMANENT;
    }

    // docs/claim-state-machine.md: HTTP timeout/DNS/5xx/429는 retryable, 나머지는 permanent.
    if (status == 429 || (status >= 500 && status < 600)) {
        return CLAIM_REQ_RETRYABLE;
    }

    return CLAIM_REQ_PERMANENT;
}

// docs/claim-state-machine.md의 claim subprocess. 성공하면 device_token_out에 deviceToken을 채우고 true.
static bool claim_subprocess(const char *claim_token, char *device_token_out, size_t device_token_out_len)
{
    claim_state_t state = CLAIM_REQUESTING;
    int retry_count = 0;

    while (1) {
        switch (state) {
        case CLAIM_REQUESTING: {
            claim_req_result_t result = do_claim_complete_request(claim_token, device_token_out, device_token_out_len);
            if (result == CLAIM_REQ_SUCCESS) {
                state = CLAIM_SUCCESS;
            } else if (result == CLAIM_REQ_RETRYABLE) {
                retry_count++;
                state = (retry_count > CLAIM_MAX_RETRY) ? CLAIM_FAILED : CLAIM_RETRY_WAIT; // 최초 요청 1회 + 재시도 5회
            } else {
                state = CLAIM_FAILED;
            }
            break;
        }
        case CLAIM_RETRY_WAIT: {
            int delay_ms = CLAIM_BACKOFF_MS[retry_count - 1];
            ESP_LOGW(TAG, "claim retry %d/%d in %d ms", retry_count, CLAIM_MAX_RETRY, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            state = CLAIM_REQUESTING;
            break;
        }
        case CLAIM_SUCCESS:
            return true;
        case CLAIM_FAILED:
        default:
            return false;
        }
    }
}

static bool load_device_token(char *out, size_t out_len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_DEVICE_TOKEN, out, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

static void save_device_token(const char *token)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_DEVICE_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    s_has_device_token = true;
}

// docs/factory-reset.md. 지운 뒤 재부팅해 정상 boot flow(Wi-Fi 정보 없음 -> PROVISIONING)를 다시 탄다.
// esp_wifi_restore()는 Wi-Fi 초기화 전에는 쓸 수 없어서 namespace를 직접 지운다.
static void erase_wifi_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void erase_device_token(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_DEVICE_TOKEN); // prov_pop과 deviceId는 남긴다.
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void led_blink(int count)
{
    for (int i = 0; i < count; i++) {
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

// 버튼 감시(8번 섹션)와 Wi-Fi 장기 실패 감시(7번 섹션)를 한 task에서 처리한다.
static void device_monitor_task(void *arg)
{
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&led));

    int64_t pressed_since_us = 0;
    bool wifi_reset_signaled = false;
    int64_t wifi_down_since_us = 0;

    while (1) {
        int64_t now_us = esp_timer_get_time();

        if (gpio_get_level(RESET_BUTTON_GPIO) == 0) {
            if (pressed_since_us == 0) {
                pressed_since_us = now_us;
                wifi_reset_signaled = false;
            }
            int64_t held_ms = (now_us - pressed_since_us) / 1000;
            if (held_ms >= BUTTON_FACTORY_RESET_MS) {
                ESP_LOGW(TAG, "factory reset: erasing Wi-Fi credentials and deviceToken");
                led_blink(5);
                erase_wifi_credentials();
                erase_device_token();
                esp_restart();
            } else if (held_ms >= BUTTON_WIFI_RESET_MS && !wifi_reset_signaled) {
                wifi_reset_signaled = true;
                led_blink(2); // 지금 놓으면 Wi-Fi 재설정, 계속 누르면 공장 초기화
            }
        } else {
            if (pressed_since_us != 0 && wifi_reset_signaled) {
                // 3초 이상 10초 미만에서 놓았다.
                ESP_LOGW(TAG, "Wi-Fi reset: erasing Wi-Fi credentials");
                erase_wifi_credentials();
                esp_restart();
            }
            pressed_since_us = 0;
        }

        // docs/state-machine.md: Wi-Fi가 5분 이상 연속으로 끊겨 있고 deviceToken이 없으면 PROVISIONING으로 돌아간다.
        // CLAIMING 중에는 보류한다(claim 상태머신이 끝난 뒤 확인). deviceToken이 있는 경우는 아직 구현하지 않는다.
        bool watch_state = s_state == DEVICE_STATE_CONNECTING || s_state == DEVICE_STATE_WAITING_CLAIM;
        if (s_wifi_connected || !watch_state || s_has_device_token) {
            wifi_down_since_us = 0;
        } else if (wifi_down_since_us == 0) {
            wifi_down_since_us = now_us;
        } else if (now_us - wifi_down_since_us >= WIFI_LONG_FAILURE_US) {
            ESP_LOGW(TAG, "Wi-Fi down for 5 min without deviceToken: erasing Wi-Fi credentials");
            erase_wifi_credentials();
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void wifi_connect_or_provision(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));

    // BLE는 provisioning 이후 claim 단계에서도 다시 필요하므로, FREE_BTDM으로
    // BT 메모리를 영구 반납하지 않는다 (반납 후 재초기화하면 heap이 깨진다).
    network_prov_mgr_config_t prov_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        device_set_state(DEVICE_STATE_PROVISIONING);

        char service_name[32];
        snprintf(service_name, sizeof(service_name), "PROV_%s", s_device_id);

        s_ble_mode = "PROVISIONING";
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_create("device-info"));
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, s_prov_pop, service_name, NULL));
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_register("device-info", device_info_handler, NULL));

        ESP_LOGI(TAG, "Provisioning started, BLE service name: %s", service_name);
    } else {
        device_set_state(DEVICE_STATE_CONNECTING);

        ESP_LOGI(TAG, "Already provisioned, connecting to Wi-Fi");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    device_set_state(DEVICE_STATE_WIFI_CONNECTED);

    if (esp_wifi_get_config(WIFI_IF_STA, &s_saved_wifi_config) == ESP_OK) {
        s_has_saved_wifi_config = true;
    }

    if (!provisioned) {
        // BLE provisioning이 끝나면 매니저가 자동으로 stop되지만(auto-stop), 완전히
        // 정리될 때까지 기다린 뒤 deinit해야 claim listener를 다시 켤 때 안전하다.
        network_prov_mgr_wait();
        network_prov_mgr_deinit();
    }
}

static void http_ping_mock_server(void)
{
    esp_http_client_config_t config = {
        .url = MOCK_SERVER_BASE_URL "/ping",
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

// docs/telemetry.md: 측정 -> 즉시 HTTPS 업로드 -> sleep 반복. 업로드 실패 시 재시도/버퍼링 없음.
// TODO: 개발용 10초. 운영 값은 10분(10 * 60 * 1000).
#define TELEMETRY_INTERVAL_MS (10 * 1000)

#define BH1750_ADDR 0x23 // ADDR 핀 미연결/GND
#define BH1750_CMD_POWER_ON 0x01
#define BH1750_CMD_CONT_HRES 0x10 // 연속 측정, 1lx 해상도

static adc_oneshot_unit_handle_t s_adc;
static i2c_master_dev_handle_t s_bh1750;

static void sensors_init(void)
{
    // 토양 수분 센서: GPIO0 = ADC1_CH0
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan_cfg));

    // BH1750: SDA=GPIO4, SCL=GPIO5
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_5,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BH1750_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_bh1750));
}

static bool read_soil_raw(int *raw)
{
    esp_err_t err = adc_oneshot_read(s_adc, ADC_CHANNEL_0, raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "soil read failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool read_lux(double *lux)
{
    uint8_t cmds[] = { BH1750_CMD_POWER_ON, BH1750_CMD_CONT_HRES };
    esp_err_t err = i2c_master_transmit(s_bh1750, &cmds[0], 1, 1000);
    if (err == ESP_OK) {
        err = i2c_master_transmit(s_bh1750, &cmds[1], 1, 1000);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200)); // 고해상도 측정 최대 180ms
        uint8_t buf[2];
        err = i2c_master_receive(s_bh1750, buf, sizeof(buf), 1000);
        if (err == ESP_OK) {
            *lux = ((buf[0] << 8) | buf[1]) / 1.2;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 read failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

// SQS 표준 큐는 순서를 보장하지 않으므로 측정 시각(created_at)을 함께 보낸다.
// SNTP 동기화 전(epoch가 2020-01-01 이전)이면 false.
static bool get_utc_iso8601(char *out, size_t out_len)
{
    time_t now = time(NULL);
    if (now < 1577836800) {
        return false;
    }
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return true;
}

// 센서 읽기에 실패한 항목과 시각 동기화 전 created_at은 null로 보낸다 (서버가 판단).
static void post_telemetry(const char *device_token)
{
    cJSON *root = cJSON_CreateObject();

    char created_at[24];
    if (get_utc_iso8601(created_at, sizeof(created_at))) {
        cJSON_AddStringToObject(root, "created_at", created_at);
    } else {
        cJSON_AddNullToObject(root, "created_at");
    }

    double lux;
    if (read_lux(&lux)) {
        cJSON_AddNumberToObject(root, "lux", round(lux * 10) / 10);
    } else {
        cJSON_AddNullToObject(root, "lux");
    }

    int soil_raw;
    if (read_soil_raw(&soil_raw)) {
        cJSON_AddNumberToObject(root, "soilRaw", soil_raw);
    } else {
        cJSON_AddNullToObject(root, "soilRaw");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[160];
    snprintf(url, sizeof(url), "%s/devices/%s/telemetry", TELEMETRY_BASE_URL, s_device_id);
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", device_token);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "x-api-key", TELEMETRY_API_KEY);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "telemetry %s -> status=%d", body, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "telemetry %s -> upload failed, dropped: %s", body, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(body);
}

static void telemetry_loop(const char *device_token) __attribute__((noreturn));
static void telemetry_loop(const char *device_token)
{
    sensors_init();

    // 첫 동기화만 잠깐 기다린다. 실패해도 백그라운드에서 계속 시도하고, 그동안은 created_at=null.
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP not synced yet");
    }

    while (1) {
        post_telemetry(device_token);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    generate_device_id();
    load_or_create_prov_pop();

    char device_token[128];
    s_has_device_token = load_device_token(device_token, sizeof(device_token));
    xTaskCreate(device_monitor_task, "device_monitor", 4096, NULL, 5, NULL);

    wifi_connect_or_provision();
    http_ping_mock_server();

    if (s_has_device_token) {
        device_set_state(DEVICE_STATE_ACTIVE);
        ESP_LOGI(TAG, "deviceToken found in NVS, device is ACTIVE");
        telemetry_loop(device_token);
    }

    s_claim_event_group = xEventGroupCreate();

    while (1) {
        device_set_state(DEVICE_STATE_WAITING_CLAIM);
        start_claim_listener();

        xEventGroupWaitBits(s_claim_event_group, CLAIM_TOKEN_RECEIVED_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
        network_prov_mgr_stop_provisioning();
        network_prov_mgr_wait();
        network_prov_mgr_deinit();

        device_set_state(DEVICE_STATE_CLAIMING);
        if (claim_subprocess(s_claim_token, device_token, sizeof(device_token))) {
            save_device_token(device_token);
            device_set_state(DEVICE_STATE_ACTIVE);
            ESP_LOGI(TAG, "Claim succeeded, deviceToken saved");
            break;
        }

        ESP_LOGE(TAG, "Claim failed, waiting for a new claim");
    }

    telemetry_loop(device_token);
}
