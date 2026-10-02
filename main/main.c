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
#include "esp_random.h"
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

#define CLAIM_MAX_RETRY 5
static const int CLAIM_BACKOFF_MS[CLAIM_MAX_RETRY] = { 2000, 4000, 8000, 16000, 32000 };

static const char *TAG = "leafie";

// AGENTS.md 6번 섹션의 상위 상태머신. CLAIMING/ACTIVE는 claim subprocess가
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

static void generate_device_id(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "deviceId: %s", s_device_id);
}

// AGENTS.md 9번 섹션: BLE provisioning 접속 PIN(PoP). 숫자 8자리.
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
    case NETWORK_PROV_WIFI_CRED_FAIL:
        ESP_LOGE(TAG, "Provisioning failed");
        break;
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
    int len = asprintf(&resp, "{\"deviceId\":\"%s\"}", s_device_id);
    if (len < 0) {
        return ESP_ERR_NO_MEM;
    }

    *outbuf = (uint8_t *)resp;
    *outlen = len;
    return ESP_OK;
}

// AGENTS.md 13번 섹션의 claim subprocess 내부 상태.
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

// AGENTS.md 18번 섹션: WAITING_CLAIM 상태에서도 claimToken을 받을 수 있어야 하므로
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

    ESP_ERROR_CHECK(network_prov_mgr_endpoint_create("claim"));
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, s_prov_pop, service_name, NULL));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_register("claim", claim_handler, NULL));

    ESP_LOGI(TAG, "Claim BLE listener started, service name: %s", service_name);
}

// AGENTS.md 16번 섹션: POST /api/v1/sensor-device-claims/{claimToken}/complete
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

    // AGENTS.md 15번 섹션: HTTP timeout/DNS/5xx/429는 retryable, 나머지는 permanent.
    if (status == 429 || (status >= 500 && status < 600)) {
        return CLAIM_REQ_RETRYABLE;
    }

    return CLAIM_REQ_PERMANENT;
}

// AGENTS.md 13/14번 섹션의 claim subprocess. 성공하면 device_token_out에 deviceToken을 채우고 true.
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
                state = (retry_count >= CLAIM_MAX_RETRY) ? CLAIM_FAILED : CLAIM_RETRY_WAIT;
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

// AGENTS.md 19번 섹션: 측정 -> 즉시 HTTPS 업로드 -> sleep 반복. 업로드 실패 시 재시도/버퍼링 없음.
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
    wifi_connect_or_provision();
    http_ping_mock_server();

    char device_token[128];
    if (load_device_token(device_token, sizeof(device_token))) {
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
