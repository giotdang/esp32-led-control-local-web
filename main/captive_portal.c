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

// Embedded HTML file symbols (from EMBED_TXTFILES in CMakeLists.txt)
extern const uint8_t wifi_config_html_start[]     asm("_binary_wifi_config_html_start");
extern const uint8_t wifi_config_html_end[]       asm("_binary_wifi_config_html_end");
extern const uint8_t configure_success_html_start[] asm("_binary_configure_success_html_start");
extern const uint8_t configure_success_html_end[]   asm("_binary_configure_success_html_end");

/* ── Helper: serve an embedded text file ─────────────────────── */

static esp_err_t serve_embedded_html(httpd_req_t *req,
                                     const uint8_t *start,
                                     const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)start, end - start);
}

/* ── DNS redirect (captive portal detection) ────────────────── */

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

    int opt = 1;
    setsockopt(dns_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dns_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr = { .s_addr = INADDR_ANY },
    };
    if (bind(dns_sock, (struct sockaddr *)&dns_addr, sizeof(dns_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed (port 53)");
        close(dns_sock);
        dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on port 53");

    while (1) {
        int len = recvfrom(dns_sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&dest_addr, &socklen);
        if (len < 12) continue;

        uint16_t qdcount = (buf[4] << 8) | buf[5];

        // Build response header
        uint8_t resp[512];
        memset(resp, 0, sizeof(resp));
        resp[0] = buf[0]; resp[1] = buf[1];  // Transaction ID
        resp[2] = 0x81; resp[3] = 0x80;      // Standard response, no error
        resp[4] = (qdcount >> 8) & 0xFF; resp[5] = qdcount & 0xFF;
        resp[6] = 0x00; resp[7] = 0x01;      // 1 answer

        // Copy query section
        int qoff = 12;
        while (qoff < len && buf[qoff] != 0) qoff++;
        qoff += 5; // Skip null + QTYPE(2) + QCLASS(2)
        memcpy(resp + 12, buf + 12, qoff - 12);

        // Answer section: pointer to query name + A record
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

static esp_err_t config_root_handler(httpd_req_t *req)
{
    return serve_embedded_html(req,
                               wifi_config_html_start,
                               wifi_config_html_end);
}

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    char *scan_json = wifi_manager_scan_networks();
    if (!scan_json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, scan_json, strlen(scan_json));
    free(scan_json);
    return ESP_OK;
}

/* URL-decode a string in-place */
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if (*src == '%' && (a = src[1]) && (b = src[2])) {
            if (a >= 'a') a -= 'a' - 'A';
            if (b >= 'a') b -= 'b' - 'A';
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

static esp_err_t configure_handler(httpd_req_t *req)
{
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    ESP_LOGI(TAG, "Form data: %s (len=%d)", content, ret);

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
        int vlen = amp ? (int)(amp - vstart) : (int)strlen(vstart);
        if (vlen >= (int)sizeof(value)) vlen = sizeof(value) - 1;
        strncpy(value, vstart, vlen);
        value[vlen] = '\0';
        url_decode(value, value);

        if (strcmp(name, "ssid") == 0)
            strncpy(ssid, value, sizeof(ssid) - 1);
        if (strcmp(name, "password") == 0)
            strncpy(password, value, sizeof(password) - 1);

        key = amp ? amp + 1 : NULL;
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving: SSID=\"%s\" Pass=\"***\"", ssid);

    wifi_creds_t creds;
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    strncpy(creds.password, password, sizeof(creds.password) - 1);

    esp_err_t err = wifi_manager_save_creds(&creds);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save creds: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }

    // Serve success page (the JS will read ssid from query param)
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
                    (const char *)configure_success_html_start,
                    configure_success_html_end - configure_success_html_start);

    // Reboot after delay
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
    return ESP_OK;
}

/* ── HTTP URI definitions ───────────────────────────────────── */

static const httpd_uri_t root_uri = {
    .uri = "/", .method = HTTP_GET, .handler = config_root_handler,
};
static const httpd_uri_t scan_uri = {
    .uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_handler,
};
static const httpd_uri_t configure_uri = {
    .uri = "/configure", .method = HTTP_POST, .handler = configure_handler,
};

/* ── Start / Stop ───────────────────────────────────────────── */

esp_err_t captive_portal_start(void)
{
    // Start DNS task (redirect all domains to ESP)
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
    httpd_register_uri_handler(server, &scan_uri);
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
