/**
 * main.cpp
 *
 * ChromaVertex Arena Beacon — ESP-IDF implementation
 *
 * Direct equivalent of beacon1.yaml (ESPHome).
 * See README.md for a mapping between YAML components and this code.
 *
 * Boot sequence:
 *   1. Initialise LED strip and render default sequence (mirrors on_boot:)
 *   2. Connect to WiFi (tries known networks in order, falls back to AP)
 *   3. Start mDNS (accessible as http://beacon1.local)
 *   4. Start HTTP server (sequence control + OTA firmware upload)
 *   5. LED update timer runs every 500ms in background
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mdns.h"

#include "wifi_manager.h"
#include "led_manager.h"
#include "web_server.h"

static const char* TAG = "main";

// Beacon identity — change per beacon (beacon1, beacon2, beacon3, beacon4)
#define BEACON_HOSTNAME  "beacon1"
#define BEACON_BOOT_SEQ  1          // sequence to display on power-on

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(BEACON_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("ChromaVertex " BEACON_HOSTNAME));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started — http://%s.local", BEACON_HOSTNAME);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ChromaVertex %s ===", BEACON_HOSTNAME);
    ESP_LOGI(TAG, "ESP-IDF v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR,
             ESP_IDF_VERSION_MINOR,
             ESP_IDF_VERSION_PATCH);

    // -----------------------------------------------------------------------
    // Step 1: LEDs
    // -----------------------------------------------------------------------
    ESP_LOGI(TAG, "Initialising LED manager...");
    led_manager_init();
    led_manager_set_sequence(BEACON_BOOT_SEQ);

    // -----------------------------------------------------------------------
    // Step 2: WiFi
    // -----------------------------------------------------------------------
    ESP_LOGI(TAG, "Initialising WiFi...");
    wifi_manager_init();

    // -----------------------------------------------------------------------
    // Step 3: mDNS — http://beacon1.local
    // -----------------------------------------------------------------------
    if (wifi_manager_is_connected()) {
        mdns_start();
    }

    // -----------------------------------------------------------------------
    // Step 4: Web server (sequence control + OTA upload)
    // -----------------------------------------------------------------------
    ESP_LOGI(TAG, "Starting web server...");
    web_server_start();

    // -----------------------------------------------------------------------
    // Idle — LED timer and WiFi events run in background
    // -----------------------------------------------------------------------
    ESP_LOGI(TAG, "Beacon running.");
    ESP_LOGI(TAG, "Control: http://%s.local or http://<ip>", BEACON_HOSTNAME);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
