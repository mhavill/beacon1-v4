#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * wifi_manager.h
 *
 * WiFi manager with provisioning support for ChromaVertex beacon.
 *
 * Connection priority:
 *   1. NVS saved credentials (from previous provisioning)
 *   2. Hardcoded credentials from secrets.h
 *   3. Fallback provisioning AP at 192.168.4.1
 *
 * The provisioning AP serves a page that:
 *   - Scans and lists available networks
 *   - Accepts SSID + password input
 *   - Tests the connection
 *   - Saves credentials to NVS on success
 *   - Reboots into normal operation
 */

/**
 * Initialise WiFi. Tries saved/known credentials first.
 * Starts provisioning AP if all fail.
 * Blocks until connected or provisioning AP is active.
 */
void wifi_manager_init(void);

/**
 * Returns true if connected to a station network.
 */
bool wifi_manager_is_connected(void);

/**
 * Clear saved NVS credentials.
 * Forces provisioning AP on next boot.
 */
void wifi_manager_clear_credentials(void);

#ifdef __cplusplus
}
#endif

/**
 * Attempt to connect with provisioned credentials.
 * Saves to NVS on success. Returns true if connected.
 * Called from web server provisioning form handler.
 */
bool wifi_manager_provision(const char* ssid, const char* password);

/**
 * Returns true if currently in provisioning AP mode.
 */
bool wifi_manager_is_provisioning(void);

/**
 * Scan for available networks.
 * Results retrievable via wifi_manager_get_scan_results().
 */
void wifi_manager_scan(void);

/**
 * Get scan results. Returns count, sets *results to internal buffer.
 */
#include "esp_wifi.h"
uint16_t wifi_manager_get_scan_results(wifi_ap_record_t** results);
