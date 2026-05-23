#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

#include "wifi_manager.h"
#include "captive_portal.h"

#define LED_PIN GPIO_NUM_2
static const char *TAG = "LED_WEB";

// Embedded HTML file symbols (from EMBED_TXTFILES in CMakeLists.txt)
extern const uint8_t led_controller_html_start[] asm("_binary_led_controller_html_start");
extern const uint8_t led_controller_html_end[]   asm("_binary_led_controller_html_end");

// LED control state
typedef struct {
    bool is_on;
    bool auto_mode;
    uint32_t blink_interval_ms;
} led_state_t;

static led_state_t led_state = {
    .is_on = false,
    .auto_mode = false,
    .blink_interval_ms = 1000
};

/* ── Helper: serve an embedded text file ─────────────────────── */

static esp_err_t serve_embedded_html(httpd_req_t *req,
                                     const uint8_t *start,
                                     const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)start, end - start);
}

/* ── HTTP handlers ──────────────────────────────────────────── */

static esp_err_t root_handler(httpd_req_t *req)
{
    return serve_embedded_html(req,
                               led_controller_html_start,
                               led_controller_html_end);
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"led\":%s,\"interval\":%lu}",
             led_state.auto_mode ? "auto" : "manual",
             led_state.is_on ? "true" : "false",
             led_state.blink_interval_ms);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t api_mode_handler(httpd_req_t *req)
{
    char buffer[100];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer));
    if (ret <= 0) return ESP_FAIL;

    buffer[ret] = '\0';
    ESP_LOGI(TAG, "Mode request: %s", buffer);

    if (strstr(buffer, "auto")) {
        led_state.auto_mode = true;
        ESP_LOGI(TAG, "Mode: AUTO");
    } else {
        led_state.auto_mode = false;
        ESP_LOGI(TAG, "Mode: MANUAL");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 16);
}

static esp_err_t api_led_handler(httpd_req_t *req)
{
    char buffer[100];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer));
    if (ret <= 0) return ESP_FAIL;

    buffer[ret] = '\0';

    if (strstr(buffer, "\"state\":true")) {
        led_state.is_on = true;
        gpio_set_level(LED_PIN, 1);
        ESP_LOGI(TAG, "LED ON");
    } else {
        led_state.is_on = false;
        gpio_set_level(LED_PIN, 0);
        ESP_LOGI(TAG, "LED OFF");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 16);
}

static esp_err_t api_frequency_handler(httpd_req_t *req)
{
    char buffer[100];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer));
    if (ret <= 0) return ESP_FAIL;

    buffer[ret] = '\0';

    int interval = 1000;
    sscanf(buffer, "{\"interval\":%d}", &interval);
    if (interval >= 100 && interval <= 2000) {
        led_state.blink_interval_ms = interval;
        ESP_LOGI(TAG, "Frequency set to: %lu ms", led_state.blink_interval_ms);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 16);
}

static esp_err_t api_reset_wifi_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "WiFi reset requested via API");
    wifi_manager_clear_creds();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"reset\",\"message\":\"Rebooting to AP mode\"}", 54);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* ── LED blink task ─────────────────────────────────────────── */

static void led_blink_task(void *pvParameter)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LED_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    while (1) {
        if (led_state.auto_mode) {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(led_state.blink_interval_ms / 2 / portTICK_PERIOD_MS);
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(led_state.blink_interval_ms / 2 / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

/* ── Start main web server ──────────────────────────────────── */

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t status_uri = {
            .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);

        httpd_uri_t mode_uri = {
            .uri = "/api/mode", .method = HTTP_POST, .handler = api_mode_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mode_uri);

        httpd_uri_t led_uri = {
            .uri = "/api/led", .method = HTTP_POST, .handler = api_led_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &led_uri);

        httpd_uri_t freq_uri = {
            .uri = "/api/frequency", .method = HTTP_POST, .handler = api_frequency_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &freq_uri);

        httpd_uri_t reset_wifi_uri = {
            .uri = "/api/reset-wifi", .method = HTTP_GET, .handler = api_reset_wifi_handler, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &reset_wifi_uri);

        ESP_LOGI(TAG, "Web server started on port 80");
    }
    return server;
}

/* ── Main entry ─────────────────────────────────────────────── */

void app_main(void)
{
    // Initialise NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialise WiFi manager (event loop, netif, wifi init)
    wifi_manager_init();

    // Try loading saved credentials and connecting
    wifi_creds_t creds;
    ret = wifi_manager_load_creds(&creds);

    if (ret == ESP_OK && strlen(creds.ssid) > 0) {
        ESP_LOGI(TAG, "Found saved WiFi: \"%s\"", creds.ssid);
        ret = wifi_manager_sta_connect(&creds, WIFI_STA_TIMEOUT_MS);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Connected! IP: %s", wifi_manager_get_ip_str());

            // Start LED blink task
            xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, NULL);

            // Start main web server
            start_web_server();
            return;
        }
        ESP_LOGW(TAG, "STA failed, falling back to AP mode");
    } else {
        ESP_LOGI(TAG, "No saved WiFi credentials");
    }

    // Fallback to AP + captive portal
    ESP_LOGI(TAG, "Starting AP mode + Captive Portal...");
    wifi_manager_start_ap();
    captive_portal_start();

    // Still start LED blink task (so LED can indicate AP mode)
    xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, NULL);
}
