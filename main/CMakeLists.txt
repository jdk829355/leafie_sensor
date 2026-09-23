idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client esp_adc esp_driver_i2c mbedtls efuse network_provisioning cjson)