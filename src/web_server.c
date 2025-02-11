#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "WebServer";

// HTML-страница
static const char *INDEX_HTML = "<!DOCTYPE html>\
<html>\
<head>\
    <meta charset='UTF-8'>\
    <title>ESP32 Control</title>\
    <script>\
        function loadSettings() {\
            fetch('/status').then(response => response.json()).then(data => {\
                document.getElementById('ssid').value = data.ssid;\
                document.getElementById('pass').value = data.password;\
                document.getElementById('ssid_label').innerText = 'Текущий SSID: ' + data.ssid;\
                document.getElementById('pass_label').innerText = 'Текущий пароль: ' + data.password;\
                document.getElementById('power').checked = data.power;\
                document.getElementById('speed').value = data.speed;\
                document.getElementById('speed_label').innerText = 'Скорость: ' + data.speed;\
            });\
        }\
        function saveWiFi() {\
            var ssid = document.getElementById('ssid').value;\
            var pass = document.getElementById('pass').value;\
            fetch('/wifi', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ssid: ssid, password: pass})})\
                .then(response => response.text())\
                .then(data => alert(data));\
        }\
        function sendCommand() {\
            var power = document.getElementById('power').checked ? 1 : 0;\
            var speed = parseInt(document.getElementById('speed').value, 10);  \
            document.getElementById('speed_label').innerText = 'Скорость: ' + speed;\
            console.log('Sending: ', JSON.stringify({power: power, speed: speed}));  \
            fetch('/control', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({power: power, speed: speed})});\
        }\
        window.onload = loadSettings;\
    </script>\
</head>\
<body>\
    <h2>Настройки Wi-Fi</h2>\
    <label id='ssid_label'>Текущий SSID: </label><br>\
    <input id='ssid' placeholder='SSID'><br>\
    <label id='pass_label'>Текущий пароль: </label><br>\
    <input id='pass' type='password' placeholder='Пароль'><br>\
    <h2>Управление</h2>\
    <input type='checkbox' id='power' onclick='sendCommand()'> ВКЛ/ВЫКЛ <br>\
    <label id='speed_label'>Скорость: 1</label><br>\
    <input type='range' id='speed' min='1' max='7' value='3' oninput='sendCommand()'> <br>\
    <button onclick='saveWiFi()'>Сохранить</button>\
</body>\
</html>";

/**
 * Обработчик главной страницы
 */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * Обработчик сохранения настроек Wi-Fi
 */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
        return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return ESP_FAIL;

    const char *ssid = cJSON_GetObjectItem(root, "ssid")->valuestring;
    const char *password = cJSON_GetObjectItem(root, "password")->valuestring;

    // Сохраняем в NVS
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK)
    {
        nvs_set_str(nvs_handle, "wifi_ssid", ssid);
        nvs_set_str(nvs_handle, "wifi_pass", password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "Wi-Fi settings saved", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * Обработчик управления устройством
 */
static esp_err_t control_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
        return ESP_FAIL;

    ESP_LOGI("WebServer", "Received JSON: %s", buf); // Логируем JSON

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        return ESP_FAIL;

    int power = cJSON_GetObjectItem(root, "power")->valueint;
    int speed = cJSON_GetObjectItem(root, "speed")->valueint;

    // Проверяем, что скорость в пределах 1-7
    if (speed < 1 || speed > 7)
    {
        ESP_LOGW("WebServer", "Invalid speed value: %d. Setting default (1)", speed);
        speed = 1;
    }

    ESP_LOGI("WebServer", "Saving Speed in NVS: %d", speed);

    // Сохраняем параметры в NVS
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK)
    {
        esp_err_t err1 = nvs_set_i32(nvs_handle, "power", power);
        esp_err_t err2 = nvs_set_i32(nvs_handle, "speed", speed);
        if (err1 != ESP_OK || err2 != ESP_OK)
        {
            ESP_LOGE("WebServer", "Failed to save settings in NVS");
        }
        else
        {
            nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    else
    {
        ESP_LOGE("WebServer", "Failed to open NVS");
    }

    cJSON_Delete(root);
    httpd_resp_send(req, "Command received", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * Обработчик запроса статуса (отправляет сохранённые данные)
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    nvs_handle_t nvs_handle;
    char ssid[32] = "";
    char password[64] = "";
    int power = 0;
    int speed = 3; // 3 по умолчанию

    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK)
    {
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(password);
        nvs_get_str(nvs_handle, "wifi_ssid", ssid, &ssid_len);
        nvs_get_str(nvs_handle, "wifi_pass", password, &pass_len);
        nvs_get_i32(nvs_handle, "power", &power);
        if (nvs_get_i32(nvs_handle, "speed", &speed) != ESP_OK)
        {
            speed = 3;
        }

        ESP_LOGI("WebServer", "Loaded Speed from NVS: %d", speed);
        nvs_close(nvs_handle);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "password", password);
    cJSON_AddNumberToObject(root, "power", power);
    cJSON_AddNumberToObject(root, "speed", speed);

    char *response = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);

    free(response);
    cJSON_Delete(root);
    return ESP_OK;
}
/**
 * Запуск веб-сервера
 */
void start_web_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &(httpd_uri_t){"/", HTTP_GET, index_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){"/wifi", HTTP_POST, wifi_post_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){"/control", HTTP_POST, control_post_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){"/status", HTTP_GET, status_get_handler});
    }
}