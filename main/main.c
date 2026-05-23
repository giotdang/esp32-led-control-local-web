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
#define HOSTNAME "esp32-blink"

static const char *TAG = "LED_WEB";

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

// ── HTTP Server handlers ──────────────────────────────────────

static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html = "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32 LED Controller</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; background: #f5f5f5; }"
        ".container { background: white; border-radius: 8px; padding: 30px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
        "h1 { color: #333; text-align: center; }"
        ".ip-badge { text-align: center; font-size: 13px; color: #666; margin-bottom: 20px; }"
        ".mode-section { margin: 30px 0; }"
        ".mode-btn { padding: 12px 24px; margin: 10px 5px; font-size: 16px; border: none; border-radius: 5px; cursor: pointer; transition: all 0.3s; }"
        ".mode-btn.active { background: #4CAF50; color: white; }"
        ".mode-btn.inactive { background: #ddd; color: #666; }"
        ".control-section { margin: 30px 0; padding: 20px; background: #f9f9f9; border-radius: 5px; display: none; }"
        ".control-section.active { display: block; }"
        ".toggle-btn { padding: 15px 40px; font-size: 18px; width: 100%; border: none; border-radius: 5px; cursor: pointer; margin: 10px 0; }"
        ".toggle-btn.on { background: #ff6b6b; color: white; }"
        ".toggle-btn.off { background: #95e1d3; color: #333; }"
        ".slider-section { margin: 20px 0; }"
        ".slider { width: 100%; height: 8px; border-radius: 5px; background: #ddd; outline: none; -webkit-appearance: none; }"
        ".slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 20px; height: 20px; border-radius: 50%; background: #4CAF50; cursor: pointer; }"
        ".slider::-moz-range-thumb { width: 20px; height: 20px; border-radius: 50%; background: #4CAF50; cursor: pointer; border: none; }"
        ".slider-label { display: flex; justify-content: space-between; margin-top: 10px; font-size: 14px; color: #666; }"
        ".status { text-align: center; margin: 20px 0; padding: 10px; background: #e3f2fd; border-radius: 5px; font-weight: bold; }"
        ".reset-link { display: block; text-align: center; margin-top: 20px; color: #999; font-size: 12px; }"
        "</style>"
        "</head><body>"
        "<div class='container'>"
        "<h1>💡 ESP32 LED Controller</h1>"
        "<div class='mode-section'>"
        "<h2>Mode Selection</h2>"
        "<button class='mode-btn active' id='manualBtn' onclick='setMode(\"manual\")'>Manual</button>"
        "<button class='mode-btn inactive' id='autoBtn' onclick='setMode(\"auto\")'>Auto</button>"
        "</div>"
        "<div class='control-section active' id='manualSection'>"
        "<h2>Manual Control</h2>"
        "<button class='toggle-btn off' id='ledToggle' onclick='toggleLED()'>LED OFF</button>"
        "</div>"
        "<div class='control-section' id='autoSection'>"
        "<h2>Auto Mode - Blink Frequency</h2>"
        "<div class='slider-section'>"
        "<input type='range' min='100' max='2000' value='1000' class='slider' id='frequencySlider' oninput='updateFrequency()'>"
        "<div class='slider-label'><span>100ms</span><span id='freqValue'>1000ms</span><span>2000ms</span></div>"
        "</div>"
        "</div>"
        "<div class='status' id='status'>🔄 Loading...</div>"
        "<a href='/' class='reset-link' onclick='fetch(\"/api/reset-wifi\").then(()=>alert(\"WiFi reset! Reconnect to ESP32-Blink-Config\"))'>🔄 Reset WiFi Settings</a>"
        "</div>"
        "<script>"
        "let currentMode = 'manual';"
        "let isLedOn = false;"
        "async function setMode(mode) {"
        "  currentMode = mode;"
        "  document.getElementById('manualBtn').className = mode === 'manual' ? 'mode-btn active' : 'mode-btn inactive';"
        "  document.getElementById('autoBtn').className = mode === 'auto' ? 'mode-btn active' : 'mode-btn inactive';"
        "  document.getElementById('manualSection').className = mode === 'manual' ? 'control-section active' : 'control-section';"
        "  document.getElementById('autoSection').className = mode === 'auto' ? 'control-section active' : 'control-section';"
        "  await fetch('/api/mode', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({mode: mode})});"
        "  await updateStatus();"
        "}"
        "async function toggleLED() {"
        "  isLedOn = !isLedOn;"
        "  updateToggleButton();"
        "  try {"
        "    await fetch('/api/led', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({state: isLedOn})});"
        "    await updateStatus();"
        "  } catch (e) {"
        "    isLedOn = !isLedOn;"
        "    updateToggleButton();"
        "  }"
        "}"
        "function updateToggleButton() {"
        "  const btn = document.getElementById('ledToggle');"
        "  btn.textContent = isLedOn ? 'LED ON' : 'LED OFF';"
        "  btn.className = 'toggle-btn ' + (isLedOn ? 'on' : 'off');"
        "}"
        "async function updateFrequency() {"
        "  const value = document.getElementById('frequencySlider').value;"
        "  document.getElementById('freqValue').textContent = value + 'ms';"
        "  await fetch('/api/frequency', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({interval: parseInt(value)})});"
        "  await updateStatus();"
        "}"
        "async function updateStatus() {"
        "  try {"
        "    const resp = await fetch('/api/status');"
        "    const data = await resp.json();"
        "    const modeText = data.mode === 'manual' ? '🖱️ Manual Control' : '⚙️ Auto Mode (' + data.interval + 'ms)';"
        "    const ledText = data.led ? '🟢 LED ON' : '⚫ LED OFF';"
        "    document.getElementById('status').textContent = modeText + ' | ' + ledText;"
        "    isLedOn = data.led;"
        "    updateToggleButton();"
        "    currentMode = data.mode;"
        "    document.getElementById('manualBtn').className = data.mode === 'manual' ? 'mode-btn active' : 'mode-btn inactive';"
        "    document.getElementById('autoBtn').className = data.mode === 'auto' ? 'mode-btn active' : 'mode-btn inactive';"
        "    document.getElementById('manualSection').className = data.mode === 'manual' ? 'control-section active' : 'control-section';"
        "    document.getElementById('autoSection').className = data.mode === 'auto' ? 'control-section active' : 'control-section';"
        "    document.getElementById('frequencySlider').value = data.interval;"
        "    document.getElementById('freqValue').textContent = data.interval + 'ms';"
        "  } catch (e) {"
        "    document.getElementById('status').textContent = '⚠️ Connection error';"
        "  }"
        "}"
        "(async () => {"
        "  await updateStatus();"
        "  setInterval(updateStatus, 2000);"
        "})();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, strlen(html));
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

// ── LED blink task ─────────────────────────────────────────────

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

// ── Start main web server (after WiFi is connected) ────────────

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

// ── Main entry ─────────────────────────────────────────────────

void app_main(void)
{
    // Initialise NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialise WiFi manager
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
