#include <string.h> // ⚠️ Добавили для strlen() и strcpy()
#include "wi-fi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_WIFI_RETRIES 3 // ⚠️ Количество попыток подключения

static const char *TAG = "WiFiManager";
static int wifi_retries = 0; // Счётчик попыток

/**
 * @brief Обработчик Wi-Fi событий
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "Trying to connect to Wi-Fi...");
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            if (wifi_retries < MAX_WIFI_RETRIES)
            {
                wifi_retries++;
                ESP_LOGW(TAG, "Wi-Fi connection failed (%d/%d). Retrying...", wifi_retries, MAX_WIFI_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(2000)); // ⚠️ Ждём 2 секунды перед новой попыткой
                esp_wifi_connect();
            }
            else
            {
                ESP_LOGE(TAG, "Failed to connect after %d attempts. Resetting Wi-Fi credentials!", MAX_WIFI_RETRIES);

                // ⚠️ Очищаем ТОЛЬКО SSID и пароль
                nvs_handle_t nvs_handle;
                if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK)
                {
                    nvs_erase_key(nvs_handle, "wifi_ssid");
                    nvs_erase_key(nvs_handle, "wifi_pass");
                    nvs_commit(nvs_handle);
                    nvs_close(nvs_handle);
                    ESP_LOGI(TAG, "Wi-Fi credentials erased from NVS.");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to open NVS for Wi-Fi reset.");
                }
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "Connected to Wi-Fi successfully!");
        wifi_retries = 0; // ⚠️ Сбрасываем счётчик попыток после успешного подключения
    }
}

/**
 * @brief Инициализация Wi-Fi (AP + STA)
 */
void wifi_init()
{
    ESP_LOGI(TAG, "Initializing Wi-Fi...");

    // ⚠️ Инициализируем NVS (нужен для хранения Wi-Fi данных)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_dhcps_stop(ap_netif); // Останавливаем DHCP-сервер

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 0, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 0, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif); // Запускаем DHCP-сервер

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Loading Wi-Fi credentials from NVS...");

    // ⚠️ Загружаем сохранённые данные Wi-Fi из NVS
    nvs_handle_t nvs_handle;
    char ssid[32] = "";
    char password[64] = "";
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);

    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK)
    {
        nvs_get_str(nvs_handle, "wifi_ssid", ssid, &ssid_len);
        nvs_get_str(nvs_handle, "wifi_pass", password, &pass_len);
        nvs_close(nvs_handle);
    }

    wifi_config_t wifi_config_sta = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };

    strcpy((char *)wifi_config_sta.sta.ssid, ssid);
    strcpy((char *)wifi_config_sta.sta.password, password);

    wifi_config_t wifi_config_ap = {
        .ap = {
            .ssid = "ESP32_AP",
            .ssid_len = strlen("ESP32_AP"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK},
    };

    ESP_LOGI(TAG, "Starting Wi-Fi in AP+STA mode...");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // ⚠️ Включаем одновременно STA + AP
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (strlen(ssid) > 0)
    {
        ESP_LOGI(TAG, "Connecting to saved Wi-Fi: SSID=%s", ssid);
        esp_wifi_connect();
    }
    else
    {
        ESP_LOGW(TAG, "No Wi-Fi credentials found. AP mode only.");
    }
}