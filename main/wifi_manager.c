#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

/* Wi-Fi disconnect reason codes (from esp_wifi_types.h) */
#define REASON_UNSPECIFIED              1
#define REASON_AUTH_EXPIRE              2
#define REASON_AUTH_LEAVE               3
#define REASON_ASSOC_EXPIRE             4
#define REASON_ASSOC_TOOMANY            5
#define REASON_NOT_AUTHED               6
#define REASON_NOT_ASSOCED              7
#define REASON_ASSOC_LEAVE              8
#define REASON_ASSOC_NOT_AUTHED         9
#define REASON_DISASSOC_PWRCAP_BAD      10
#define REASON_DISASSOC_SUPCHAN_BAD     11
#define REASON_IE_INVALID               13
#define REASON_MIC_FAILURE              14
#define REASON_4WAY_HANDSHAKE_TIMEOUT   15
#define REASON_GROUP_KEY_UPDATE_TIMEOUT 16
#define REASON_IE_IN_4WAY_DIFFERS       17
#define REASON_GROUP_CIPHER_INVALID     18
#define REASON_PAIRWISE_CIPHER_INVALID  19
#define REASON_AKMP_INVALID             20
#define REASON_UNSUPP_RSN_IE_VERSION    21
#define REASON_INVALID_RSN_IE_CAP       22
#define REASON_802_1X_AUTH_FAILED       23
#define REASON_CIPHER_SUITE_REJECTED    24
#define REASON_BEACON_TIMEOUT           200
#define REASON_NO_AP_FOUND              201
#define REASON_AUTH_FAIL                202
#define REASON_ASSOC_FAIL               203
#define REASON_HANDSHAKE_TIMEOUT        204

static const char *wifi_reason_str(uint8_t reason)
{
    switch (reason) {
        case REASON_UNSPECIFIED:              return "UNSPECIFIED";
        case REASON_AUTH_EXPIRE:             return "AUTH_EXPIRE";
        case REASON_NOT_AUTHED:              return "NOT_AUTHED";
        case REASON_4WAY_HANDSHAKE_TIMEOUT:  return "4WAY_HANDSHAKE_TIMEOUT";
        case REASON_GROUP_KEY_UPDATE_TIMEOUT:return "GROUP_KEY_UPDATE_TIMEOUT";
        case REASON_IE_IN_4WAY_DIFFERS:      return "IE_IN_4WAY_DIFFERS";
        case REASON_GROUP_CIPHER_INVALID:    return "GROUP_CIPHER_INVALID";
        case REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
        case REASON_AKMP_INVALID:            return "AKMP_INVALID";
        case REASON_UNSUPP_RSN_IE_VERSION:   return "UNSUPP_RSN_IE_VERSION";
        case REASON_INVALID_RSN_IE_CAP:      return "INVALID_RSN_IE_CAP";
        case REASON_802_1X_AUTH_FAILED:      return "802_1X_AUTH_FAILED";
        case REASON_CIPHER_SUITE_REJECTED:   return "CIPHER_SUITE_REJECTED";
        case REASON_BEACON_TIMEOUT:          return "BEACON_TIMEOUT (AP not found)";
        case REASON_NO_AP_FOUND:             return "NO_AP_FOUND";
        case REASON_AUTH_FAIL:               return "AUTH_FAIL (wrong password?)";
        case REASON_ASSOC_FAIL:              return "ASSOC_FAIL";
        case REASON_HANDSHAKE_TIMEOUT:       return "HANDSHAKE_TIMEOUT (wrong password?)";
        default:                             return "OTHER";
    }
}

#include "wifi_manager.h"

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t wifi_event_group;
static const int STA_CONNECTED_BIT = BIT0;
static const int STA_GOT_IP_BIT    = BIT1;
static const int STA_FAIL_BIT      = BIT2;

static bool sta_connected = false;
static char sta_ip_str[16] = "";

/* ── NVS helpers ─────────────────────────────────────────────── */

esp_err_t wifi_manager_load_creds(wifi_creds_t *creds)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("wifi", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = WIFI_CREDS_SSID_MAX;
    err = nvs_get_str(handle, "ssid", creds->ssid, &len);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    len = WIFI_CREDS_PASS_MAX;
    err = nvs_get_str(handle, "password", creds->password, &len);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_save_creds(const wifi_creds_t *creds)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", creds->ssid);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_set_str(handle, "password", creds->password);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_clear_creds(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_erase_key(handle, "ssid");
    nvs_erase_key(handle, "password");
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* ── Event handler ──────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
        xEventGroupSetBits(wifi_event_group, STA_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA disconnected (reason %d: %s)",
                 evt->reason, wifi_reason_str(evt->reason));
        sta_connected = false;
        sta_ip_str[0] = '\0';
        xEventGroupSetBits(wifi_event_group, STA_FAIL_BIT);
        // Clear connected bits so future wait works
        xEventGroupClearBits(wifi_event_group, STA_CONNECTED_BIT | STA_GOT_IP_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        sta_connected = true;
        snprintf(sta_ip_str, sizeof(sta_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", sta_ip_str);
        xEventGroupSetBits(wifi_event_group, STA_GOT_IP_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP station connected");
    }
}

/* ── Init / Deinit ──────────────────────────────────────────── */

esp_err_t wifi_manager_init(void)
{
    wifi_event_group = xEventGroupCreate();
    if (!wifi_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
        ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    esp_err_t ret = esp_wifi_stop();
    if (ret == ESP_OK) {
        ret = esp_wifi_deinit();
    }
    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
    sta_connected = false;
    sta_ip_str[0] = '\0';
    return ret;
}

/* ── STA connect with timeout ───────────────────────────────── */

esp_err_t wifi_manager_sta_connect(const wifi_creds_t *creds, int timeout_ms)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ESP_ERR_NO_MEM;

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(wifi_config.sta.ssid, creds->ssid, strlen(creds->ssid));
    memcpy(wifi_config.sta.password, creds->password, strlen(creds->password));

    // Clear event bits before starting (not after, to avoid race)
    xEventGroupClearBits(wifi_event_group,
        STA_CONNECTED_BIT | STA_GOT_IP_BIT | STA_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Wait for either GOT_IP or DISCONNECTED
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        STA_GOT_IP_BIT | STA_FAIL_BIT,
        pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);

    if (bits & STA_GOT_IP_BIT) {
        ESP_LOGI(TAG, "STA connect OK, IP: %s", sta_ip_str);
        return ESP_OK;
    }

    // Timeout or fail — stop STA
    ESP_LOGW(TAG, "STA connect timeout/fail after %dms", timeout_ms);
    esp_wifi_stop();
    if (netif) esp_netif_destroy_default_wifi(netif);
    return ESP_ERR_TIMEOUT;
}

/* ── AP mode ────────────────────────────────────────────────── */

esp_err_t wifi_manager_start_ap(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_ERR_NO_MEM;

    /* Also create STA netif so scanning works while in AP mode */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) return ESP_ERR_NO_MEM;

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(ap_config.ap.ssid, WIFI_AP_SSID, strlen(WIFI_AP_SSID));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=\"%s\" IP=192.168.4.1",
             WIFI_AP_SSID);
    return ESP_OK;
}

/* ── Scan networks ──────────────────────────────────────────── */

char *wifi_manager_scan_networks(void)
{
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;

    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        esp_wifi_scan_stop();
        return strdup("[]");
    }

    ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        esp_wifi_scan_stop();
        return strdup("[]");
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    /* Build JSON array */
    size_t buf_size = ap_count * 64 + 4;
    char *json = malloc(buf_size);
    if (!json) { free(ap_list); esp_wifi_scan_stop(); return strdup("[]"); }

    strcpy(json, "[");
    for (int i = 0; i < ap_count; i++) {
        char entry[128];
        snprintf(entry, sizeof(entry),
            "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}%s",
            ap_list[i].ssid,
            ap_list[i].rssi,
            ap_list[i].authmode,
            (i < ap_count - 1) ? "," : "");
        strlcat(json, entry, buf_size);
    }
    strlcat(json, "]", buf_size);
    free(ap_list);
    esp_wifi_scan_stop();
    return json;
}

/* ── Status helpers ─────────────────────────────────────────── */

bool wifi_manager_is_connected(void)
{
    return sta_connected;
}

const char *wifi_manager_get_ip_str(void)
{
    return sta_ip_str;
}
