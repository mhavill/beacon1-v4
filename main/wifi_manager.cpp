/**
 * wifi_manager.cpp
 *
 * WiFi manager with self-provisioning for ChromaVertex beacon.
 *
 * Connection sequence on boot:
 *   1. Load saved credentials from NVS (set by previous provisioning)
 *      - Retry up to 3 times with 2s delay (Omada router cold-start timing)
 *   2. Try hardcoded credentials from secrets.h (developer fallback)
 *   3. Start provisioning AP at 192.168.4.1
 *      - Scans available networks
 *      - User selects SSID and enters password via browser
 *      - On success: saves to NVS, reboots
 *
 * Key implementation notes:
 *   - WIFI_STORAGE_RAM: prevents NVS WiFi driver cache conflicting
 *     with our own credential management (fixes 0x200 auth failures)
 *   - esp_wifi_set_max_tx_power() must be called AFTER esp_wifi_start()
 *   - esp_wifi_deinit/init required when switching AP->STA for provisioning
 *   - Omada ER706W rejects first auth after cold start — retry with delay
 */

#include "wifi_manager.h"
#include "secrets.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char* TAG = "wifi_manager";

// ---------------------------------------------------------------------------
// NVS namespace and keys
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE   "wifi_creds"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "password"

// ---------------------------------------------------------------------------
// Provisioning AP
// ---------------------------------------------------------------------------
static const char* AP_SSID     = "ChromaVertex-Setup";
static const char* AP_PASSWORD = "chromavertex";

// ---------------------------------------------------------------------------
// Hardcoded developer credentials from secrets.h
// Set to empty strings for deployed beacons — provisioning handles setup
// ---------------------------------------------------------------------------
static const struct {
    const char* ssid;
    const char* password;
} KNOWN_NETWORKS[] = {
    { WIFI_SSID_HOME,   WIFI_PASS_HOME   },
    { WIFI_SSID_MOBILE, WIFI_PASS_MOBILE },
    { WIFI_SSID_IPHONE, WIFI_PASS_IPHONE },
};
static const int NUM_NETWORKS = sizeof(KNOWN_NETWORKS) / sizeof(KNOWN_NETWORKS[0]);

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_wifi_event_group;
static const int CONNECTED_BIT  = BIT0;
static const int FAIL_BIT       = BIT1;
static bool      s_connected    = false;
static bool      s_provisioning = false;

#define MAX_SCAN_RESULTS 16
static wifi_ap_record_t s_scan_results[MAX_SCAN_RESULTS];
static uint16_t         s_scan_count = 0;

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "Disconnected, reason: %d", d->reason);
        s_connected = false;
        xEventGroupSetBits(s_wifi_event_group, FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    }
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static bool nvs_load_credentials(char* ssid, size_t ssid_len,
                                  char* password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_err_t ssid_err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);
    nvs_close(handle);

    ESP_LOGI(TAG, "NVS load — SSID: %s, PASS: %s",
             esp_err_to_name(ssid_err), esp_err_to_name(pass_err));

    bool ok = (ssid_err == ESP_OK) && (pass_err == ESP_OK) && (strlen(ssid) > 0);
    if (ok) ESP_LOGI(TAG, "Loaded saved credentials for SSID: '%s'", ssid);
    return ok;
}

static bool nvs_save_credentials(const char* ssid, const char* password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_err_t ssid_err   = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    esp_err_t pass_err   = nvs_set_str(handle, NVS_KEY_PASS, password);
    esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);

    bool ok = (ssid_err == ESP_OK) && (pass_err == ESP_OK) && (commit_err == ESP_OK);
    if (ok) {
        ESP_LOGI(TAG, "Saved credentials for SSID: '%s'", ssid);
    } else {
        ESP_LOGE(TAG, "NVS save failed — SSID:%s PASS:%s commit:%s",
                 esp_err_to_name(ssid_err),
                 esp_err_to_name(pass_err),
                 esp_err_to_name(commit_err));
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Try to connect to one specific network — single attempt
// ---------------------------------------------------------------------------
static bool try_connect(const char* ssid, const char* password)
{
    if (!ssid || strlen(ssid) == 0) return false;

    ESP_LOGI(TAG, "Trying: '%s'", ssid);
    xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT | FAIL_BIT);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid,     ssid,     sizeof(cfg.sta.ssid)     - 1);
    strncpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Power settings MUST be after esp_wifi_start()
    esp_wifi_set_max_tx_power(34);   // 8.5 dBm — prevents LDO sag on SuperMini
    esp_wifi_set_ps(WIFI_PS_NONE);   // no modem sleep — fixes C3 bugs

    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        CONNECTED_BIT | FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(10000)
    );

    if (bits & CONNECTED_BIT) return true;

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    return false;
}

// ---------------------------------------------------------------------------
// Try to connect with retries
// Needed because Omada ER706W rejects first auth after cold start
// ---------------------------------------------------------------------------
static bool try_connect_with_retry(const char* ssid, const char* password,
                                    int retries, int delay_ms)
{
    for (int i = 0; i < retries; i++) {
        if (i > 0) {
            ESP_LOGW(TAG, "Retry %d/%d for '%s'", i, retries - 1, ssid);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        if (try_connect(ssid, password)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Network scan
// ---------------------------------------------------------------------------
void wifi_manager_scan(void)
{
    ESP_LOGI(TAG, "Scanning for networks...");
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_wifi_scan_start(&scan_cfg, true);
    s_scan_count = MAX_SCAN_RESULTS;
    esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_results);
    ESP_LOGI(TAG, "Found %d networks", s_scan_count);
    esp_wifi_set_mode(WIFI_MODE_AP);
}

uint16_t wifi_manager_get_scan_results(wifi_ap_record_t** results)
{
    *results = s_scan_results;
    return s_scan_count;
}

// ---------------------------------------------------------------------------
// Provisioning AP
// ---------------------------------------------------------------------------
static void start_provisioning_ap(void)
{
    s_provisioning = true;
    ESP_LOGW(TAG, "Starting provisioning AP: '%s'", AP_SSID);

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid,     AP_SSID,     sizeof(ap_cfg.ap.ssid)     - 1);
    strncpy((char*)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len         = strlen(AP_SSID);
    ap_cfg.ap.channel          = 1;
    ap_cfg.ap.authmode         = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection   = 4;
    ap_cfg.ap.beacon_interval  = 100;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_manager_scan();

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Provisioning AP ready");
    ESP_LOGI(TAG, "Connect to '%s' (password: %s)", AP_SSID, AP_PASSWORD);
    ESP_LOGI(TAG, "Then open http://192.168.4.1");
}

// ---------------------------------------------------------------------------
// Reinitialise WiFi driver cleanly
// Required when switching between AP and STA modes during provisioning
// ---------------------------------------------------------------------------
static void wifi_reinit(void)
{
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
}

// ---------------------------------------------------------------------------
// Provision — called from web server form handler
// ---------------------------------------------------------------------------
bool wifi_manager_provision(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Provisioning: trying '%s'", ssid);

    // Full reinit required — AP mode state interferes with STA auth
    wifi_reinit();
    xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT | FAIL_BIT);

    // Try up to 3 times with delay between attempts
    if (try_connect_with_retry(ssid, password, 3, 3000)) {
        ESP_LOGI(TAG, "Provisioning successful");
        nvs_save_credentials(ssid, password);
        return true;
    }

    ESP_LOGW(TAG, "Provisioning failed for '%s' — restarting AP", ssid);
    wifi_reinit();
    start_provisioning_ap();
    return false;
}

// ---------------------------------------------------------------------------
// Clear saved credentials
// ---------------------------------------------------------------------------
void wifi_manager_clear_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved credentials cleared");
    }
}

// ---------------------------------------------------------------------------
// Public state
// ---------------------------------------------------------------------------
bool wifi_manager_is_connected(void)    { return s_connected; }
bool wifi_manager_is_provisioning(void) { return s_provisioning; }

// ---------------------------------------------------------------------------
// Main init
// ---------------------------------------------------------------------------
void wifi_manager_init(void)
{
    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // --- Step 1: NVS saved credentials (retry for router cold-start timing) ---
    char saved_ssid[64] = {0};
    char saved_pass[64] = {0};
    if (nvs_load_credentials(saved_ssid, sizeof(saved_ssid),
                              saved_pass,  sizeof(saved_pass))) {
        if (try_connect_with_retry(saved_ssid, saved_pass, 3, 2000)) {
            ESP_LOGI(TAG, "Connected using saved credentials");
            return;
        }
        ESP_LOGW(TAG, "Saved credentials failed");
    }

    // --- Step 2: Hardcoded credentials from secrets.h ---
    for (int i = 0; i < NUM_NETWORKS; i++) {
        if (!KNOWN_NETWORKS[i].ssid || strlen(KNOWN_NETWORKS[i].ssid) == 0) continue;
        if (try_connect_with_retry(KNOWN_NETWORKS[i].ssid,
                                    KNOWN_NETWORKS[i].password, 2, 2000)) {
            ESP_LOGI(TAG, "Connected to '%s'", KNOWN_NETWORKS[i].ssid);
            return;
        }
        ESP_LOGW(TAG, "Could not connect to '%s'", KNOWN_NETWORKS[i].ssid);
    }

    // --- Step 3: Provisioning AP ---
    start_provisioning_ap();
}
