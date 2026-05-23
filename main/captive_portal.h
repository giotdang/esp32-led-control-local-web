#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_err.h"

/**
 * @brief Start the captive portal HTTP server on port 80.
 *        Serves WiFi config form and handles POST /configure.
 *        Only call after AP mode is started.
 */
esp_err_t captive_portal_start(void);

/**
 * @brief Stop the captive portal server.
 */
esp_err_t captive_portal_stop(void);

#endif // CAPTIVE_PORTAL_H
