#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

#define WIFI_CREDS_SSID_MAX     33
#define WIFI_CREDS_PASS_MAX     65
#define WIFI_STA_TIMEOUT_MS     15000
#define WIFI_AP_SSID            "ESP32-Blink-Config"
#define WIFI_AP_PASS            NULL  // Open AP
#define WIFI_AP_CHANNEL         1
#define WIFI_AP_MAX_CONN        4

typedef struct {
    char ssid[WIFI_CREDS_SSID_MAX];
    char password[WIFI_CREDS_PASS_MAX];
} wifi_creds_t;

/**
 * @brief Load WiFi credentials from NVS.
 *        Returns ESP_ERR_NVS_NOT_FOUND if none saved.
 */
esp_err_t wifi_manager_load_creds(wifi_creds_t *creds);

/**
 * @brief Save WiFi credentials to NVS (namespace "wifi").
 */
esp_err_t wifi_manager_save_creds(const wifi_creds_t *creds);

/**
 * @brief Erase WiFi credentials from NVS.
 */
esp_err_t wifi_manager_clear_creds(void);

/**
 * @brief Try STA connection with timeout.
 *        Returns ESP_OK on success, ESP_ERR_TIMEOUT on fail.
 */
esp_err_t wifi_manager_sta_connect(const wifi_creds_t *creds, int timeout_ms);

/**
 * @brief Start SoftAP mode.
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief Stop WiFi (STA + AP) and clean up.
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Initialise WiFi subsystem (NVS, event loop).
 *        Call once at boot.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Scan networks and return a JSON-friendly string.
 *        Caller must free() the returned pointer.
 */
char *wifi_manager_scan_networks(void);

/**
 * @brief Check if STA currently has an IP.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get current STA IP as string (returns "" if not connected).
 */
const char *wifi_manager_get_ip_str(void);

#endif // WIFI_MANAGER_H
