#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_http_server.h"

#include "captive_portal.h"
#include "wifi_manager.h"

static const char *TAG = "CAPTIVE";

static httpd_handle_t server = NULL;
static int dns_sock = -1;

/* ── DNS redirect (captive portal detection) ────────────────── */

/**
 * Simple DNS server: respond to ANY query with the ESP's AP IP.
 * This tricks phones/laptops into showing the "login required" popup.
 */
static void dns_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    socklen_t socklen = sizeof(dest_addr);
    uint8_t buf[512];

    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "DNS: unable to create socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dns_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr = { .s_addr = INADDR_ANY },
    };
    if (bind(dns_sock, (struct sockaddr *)&dns_addr, sizeof(dns_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed");
        close(dns_sock);
        dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on port 53");

    while (1) {
        int len = recvfrom(dns_sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&dest_addr, &socklen);
        if (len < 12) continue; // Too short for DNS header

        // Respond with the ESP's AP IP (192.168.4.1)
        uint16_t id = (buf[0] << 8) | buf[1];
        uint16_t flags = 0x8180; // Standard response, no error
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        uint16_t ancount = htons(1);

        // Build response header
        uint8_t resp[512];
        memset(resp, 0, sizeof(resp));
        resp[0] = (id >> 8) & 0xFF;
        resp[1] = id & 0xFF;
        resp[2] = (flags >> 8) & 0xFF;
        resp[3] = flags & 0xFF;
        resp[4] = (qdcount >> 8) & 0xFF;
        resp[5] = qdcount & 0xFF;
        resp[6] = (ancount >> 8) & 0xFF;
        resp[7] = ancount & 0xFF;

        // Copy query section
        int qoff = 12;
        while (qoff < len && buf[qoff] != 0) qoff++;
        qoff += 5; // Skip null terminator + QTYPE + QCLASS
        memcpy(resp + 12, buf + 12, qoff - 12);

        // Answer section: CNAME-like pointer to domain name
        int aoff = 12 + (qoff - 12);
        resp[aoff++] = 0xC0; resp[aoff++] = 12;  // Pointer to query name
        resp[aoff++] = 0x00; resp[aoff++] = 0x01; // Type A
        resp[aoff++] = 0x00; resp[aoff++] = 0x01; // Class IN
        resp[aoff++] = 0x00; resp[aoff++] = 0x00;
        resp[aoff++] = 0x00; resp[aoff++] = 0x3C; // TTL 60s
        resp[aoff++] = 0x00; resp[aoff++] = 0x04; // Data length 4
        resp[aoff++] = 192; resp[aoff++] = 168;
        resp[aoff++] = 4;    resp[aoff++] = 1;    // 192.168.4.1

        sendto(dns_sock, resp, aoff, 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    }
}

/* ── HTTP handlers ──────────────────────────────────────────── */

/* Helper: URL-decode a string in-place */
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if (*src == '%' && (a = src[1]) && (b = src[2])) {
            if (a >= 'a') a -= 'a' - 'A';
            if (b >= 'a') b -= 'a' - 'A';
            *dst++ = ((a - (a >= 'A' ? 'A' - 10 : '0')) << 4) |
                      (b - (b >= 'A' ? 'A' - 10 : '0'));
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static esp_err_t config_root_handler(httpd_req_t *req)
{
    char *scan_json = wifi_manager_scan_networks();

    const char *html_fmt =
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 WiFi Config</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;max-width:500px;margin:40px auto;padding:20px;background:#f0f4f8}"
        ".card{background:#fff;border-radius:12px;padding:24px;box-shadow:0 2px 12px rgba(0,0,0,0.1)}"
        "h1{text-align:center;color:#1a1a2e;margin-top:0}"
        "h2{color:#16213e;font-size:16px;margin-bottom:8px}"
        "select,input[type=password]{width:100%%;padding:12px;margin:6px 0 16px;border:2px solid #e0e0e0;border-radius:8px;font-size:15px;box-sizing:border-box}"
        "select:focus,input:focus{border-color:#0f3460;outline:none}"
        "button{width:100%%;padding:14px;background:#0f3460;color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer}"
        "button:hover{background:#1a5276}"
        ".status{text-align:center;color:#666;margin-top:12px;font-size:13px}"
        ".rssi{color:#888;font-size:12px}"
        "</style>"
        "</head><body>"
        "<div class='card'>"
        "<h1>🔧 WiFi Configuration</h1>"
        "<form action='/configure' method='POST'>"
        "<h2>Select Network</h2>"
        "<select name='ssid' required>"
        "<option value=''>-- Scan networks --</option>";

    // Build HTML string on heap
    size_t html_len = strlen(html_fmt) + strlen(scan_json) * 128 + 2048;
    char *html = malloc(html_len);
    if (!html) {
        free(scan_json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    strcpy(html, html_fmt);

    // Parse scan JSON and create <option>s
    // JSON format: [{"ssid":"...","rssi":-50,"auth":3},...]
    char *p = scan_json;
    while (*p) {
        char ssid[64] = "";
        int rssi = 0, auth = 0;
        if (sscanf(p, "{\"ssid\":\"%63[^\"]\",\"rssi\":%d,\"auth\":%d}",
                   ssid, &rssi, &auth) == 3) {
            if (strlen(ssid) > 0) {
                char opt[256];
                const char *lock = (auth >= 3) ? "🔒" : "🔓";
                snprintf(opt, sizeof(opt),
                    "<option value='%s'>%s %s <span class='rssi'>(%ddBm)</span></option>",
                    ssid, lock, ssid, rssi);
                strlcat(html, opt, html_len);
            }
        }
        // Move to next
        p = strchr(p + 1, '{');
        if (!p) break;
    }

    char *tail =
        "</select>"
        "<h2>Password</h2>"
        "<input type='password' name='password' placeholder='Enter WiFi password'>"
        "<button type='submit'>📡 Connect & Reboot</button>"
        "</form>"
        "<div class='status'>Connect to this WiFi, then select your network above</div>"
        "</div>"
        "</body></html>";

    strlcat(html, tail, html_len);
    free(scan_json);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);
    return ESP_OK;
}

static esp_err_t config_configure_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI(TAG, "Form data: %s", content);

    // Parse form-urlencoded: ssid=XXX&password=YYY
    char ssid[WIFI_CREDS_SSID_MAX] = "";
    char password[WIFI_CREDS_PASS_MAX] = "";

    char *key = content;
    while (key && *key) {
        char *amp = strchr(key, '&');
        char *eq  = strchr(key, '=');
        if (!eq) break;

        char name[32], value[128];
        int nlen = eq - key;
        if (nlen >= (int)sizeof(name)) nlen = sizeof(name) - 1;
        strncpy(name, key, nlen);
        name[nlen] = '\0';

        const char *vstart = eq + 1;
        int vlen = amp ? amp - vstart : strlen(vstart);
        if (vlen >= (int)sizeof(value)) vlen = sizeof(value) - 1;
        strncpy(value, vstart, vlen);
        value[vlen] = '\0';
        url_decode(value, value);

        if (strcmp(name, "ssid") == 0) strncpy(ssid, value, sizeof(ssid) - 1);
        if (strcmp(name, "password") == 0) strncpy(password, value, sizeof(password) - 1);

        key = amp ? amp + 1 : NULL;
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving: SSID=\"%s\" Pass=\"%s\"", ssid, password);

    wifi_creds_t creds;
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    strncpy(creds.password, password, sizeof(creds.password) - 1);

    esp_err_t err = wifi_manager_save_creds(&creds);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save creds: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }

    // Send success HTML with reboot instruction
    const char *resp_html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='5'>"
        "<style>"
        "body{font-family:Arial,sans-serif;max-width:500px;margin:40px auto;padding:20px;background:#f0f4f8;text-align:center}"
        ".card{background:#fff;border-radius:12px;padding:24px;box-shadow:0 2px 12px rgba(0,0,0,0.1)}"
        ".check{font-size:64px;margin:16px 0;color:#27ae60}"
        "</style>"
        "</head><body>"
        "<div class='card'>"
        "<div class='check'>✅</div>"
        "<h1>Configuration Saved!</h1>"
        "<p>Rebooting in 3 seconds...</p>"
        "<p>Please reconnect to <strong>%s</strong> after reboot</p>"
        "</div></body></html>";

    size_t resp_len = strlen(resp_html) + strlen(ssid) + 64;
    char *resp_buf = malloc(resp_len);
    if (resp_buf) {
        snprintf(resp_buf, resp_len, resp_html, ssid);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, resp_buf, strlen(resp_buf));
        free(resp_buf);
    }

    // Reboot after a short delay
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();

    return ESP_OK;
}

/* ── Start / Stop ───────────────────────────────────────────── */

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = config_root_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t configure_uri = {
    .uri = "/configure",
    .method = HTTP_POST,
    .handler = config_configure_handler,
    .user_ctx = NULL,
};

esp_err_t captive_portal_start(void)
{
    // Start DNS task
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 5;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &configure_uri);

    ESP_LOGI(TAG, "Captive portal ready: http://192.168.4.1");
    return ESP_OK;
}

esp_err_t captive_portal_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    if (dns_sock >= 0) {
        close(dns_sock);
        dns_sock = -1;
    }
    return ESP_OK;
}
