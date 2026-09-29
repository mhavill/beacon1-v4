/**
 * web_server.cpp
 *
 * HTTP control server for ChromaVertex beacon.
 *
 * Normal mode endpoints (when connected to WiFi):
 *   GET  /          — Control page (sequence selector + OTA upload)
 *   POST /set       — Set LED sequence
 *   GET  /status    — JSON status
 *   POST /ota       — OTA firmware upload
 *   POST /forget    — Clear saved WiFi credentials and reboot
 *
 * Provisioning mode endpoints (when in fallback AP):
 *   GET  /          — WiFi provisioning page (scan + credential entry)
 *   POST /provision — Submit SSID + password, test and save
 */

#include "web_server.h"
#include "led_manager.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "web_server";

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------
static const char* AUTH_USERNAME = "admin";
static const char* AUTH_PASSWORD = "beacon1";

// ---------------------------------------------------------------------------
// Basic auth
// ---------------------------------------------------------------------------
static bool check_auth(httpd_req_t* req)
{
    char credentials[128];
    snprintf(credentials, sizeof(credentials), "%s:%s", AUTH_USERNAME, AUTH_PASSWORD);

    unsigned char encoded[256];
    size_t encoded_len = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
                          (const unsigned char*)credentials, strlen(credentials));
    encoded[encoded_len] = '\0';

    char auth_header[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                     auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    const char* prefix = "Basic ";
    if (strncmp(auth_header, prefix, strlen(prefix)) != 0) return false;
    return strcmp(auth_header + strlen(prefix), (char*)encoded) == 0;
}

static esp_err_t send_auth_challenge(httpd_req_t* req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Beacon1\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Shared CSS
// ---------------------------------------------------------------------------
static const char* CSS =
    "body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 20px;background:#111;color:#eee;}"
    "h1{color:#0ff;font-size:1.4em;margin-bottom:4px;}"
    "h2{color:#888;font-size:0.95em;font-weight:normal;margin-top:24px;margin-bottom:8px;"
    "   border-top:1px solid #333;padding-top:12px;}"
    "label{display:block;margin-top:16px;color:#aaa;font-size:0.9em;}"
    "input,select{width:100%;padding:10px;font-size:1em;"
    "  background:#222;color:#fff;border:1px solid #444;border-radius:4px;"
    "  box-sizing:border-box;margin-top:4px;}"
    "button{margin-top:12px;width:100%;padding:12px;font-size:1em;"
    "  background:#0ff;color:#000;border:none;border-radius:4px;"
    "  cursor:pointer;font-weight:bold;}"
    "button.warn{background:#f80;}"
    "button.danger{background:#f44;}"
    "button:active{opacity:0.8;}"
    ".status{margin-top:16px;padding:10px;background:#1a1a1a;border-radius:4px;"
    "  font-size:0.85em;color:#888;}"
    ".seq-name{color:#ff0;font-weight:bold;}"
    ".ver{color:#555;font-size:0.8em;}"
    ".msg{margin-top:12px;padding:10px;border-radius:4px;font-size:0.9em;}"
    ".msg.ok{background:#1a3a1a;color:#4f4;}"
    ".msg.err{background:#3a1a1a;color:#f44;}"
    "#ota-progress{width:100%;height:6px;background:#333;border-radius:3px;"
    "  margin-top:8px;display:none;}"
    "#ota-bar{height:6px;background:#f80;border-radius:3px;width:0%;"
    "  transition:width 0.3s;}"
    "#ota-status{margin-top:8px;font-size:0.85em;min-height:1.2em;}";

// ---------------------------------------------------------------------------
// GET / — Provisioning page (shown when in AP mode)
// ---------------------------------------------------------------------------
static esp_err_t handle_provision_page(httpd_req_t* req)
{
    // Trigger a fresh scan
    wifi_manager_scan();

    wifi_ap_record_t* results;
    uint16_t count = wifi_manager_get_scan_results(&results);

    // Build network options for select
    static char options[2048];
    options[0] = '\0';
    for (int i = 0; i < count; i++) {
        char opt[128];
        snprintf(opt, sizeof(opt),
            "<option value='%s'>%s (%d dBm)</option>",
            (char*)results[i].ssid,
            (char*)results[i].ssid,
            results[i].rssi);
        strncat(options, opt, sizeof(options) - strlen(options) - 1);
    }

    static char html[6144];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ChromaVertex Setup</title>"
        "<style>%s</style></head><body>"
        "<h1>&#128268; ChromaVertex Setup</h1>"
        "<p style='color:#aaa;font-size:0.9em'>"
        "Connect this beacon to your WiFi network.<br>"
        "Credentials are saved securely and used on every reboot.</p>"
        "<form method='POST' action='/provision'>"
        "  <label>Select Network</label>"
        "  <select name='ssid' id='ssid_sel' onchange='updateSsid()'>"
        "    <option value=''>-- Select network --</option>"
        "    %s"
        "    <option value='__manual__'>Enter manually...</option>"
        "  </select>"
        "  <label>SSID</label>"
        "  <input type='text' name='ssid_manual' id='ssid_manual' "
        "         placeholder='Network name' style='display:none'>"
        "  <label>Password</label>"
        "  <input type='password' name='password' placeholder='WiFi password'>"
        "  <button type='submit'>Connect &amp; Save</button>"
        "</form>"
        "<script>"
        "function updateSsid(){"
        "  var sel=document.getElementById('ssid_sel').value;"
        "  var manual=document.getElementById('ssid_manual');"
        "  manual.style.display=(sel==='__manual__')?'block':'none';"
        "}"
        "</script>"
        "</body></html>",
        CSS, options
    );

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /provision — Handle provisioning form submission
// ---------------------------------------------------------------------------
static esp_err_t handle_provision(httpd_req_t* req)
{
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_OK;
    }
    buf[received] = '\0';

    // Parse form fields
    char ssid[64]     = {0};
    char password[64] = {0};
    char ssid_sel[64] = {0};

    // Extract ssid (from select)
    char* p = strstr(buf, "ssid=");
    if (p) {
        sscanf(p + 5, "%63[^&]", ssid_sel);
        // URL decode + (space)
        for (char* c = ssid_sel; *c; c++) if (*c == '+') *c = ' ';
    }

    // Extract manual ssid override
    char ssid_manual[64] = {0};
    p = strstr(buf, "ssid_manual=");
    if (p) sscanf(p + 12, "%63[^&]", ssid_manual);

    // Extract password
    p = strstr(buf, "password=");
    if (p) {
        sscanf(p + 9, "%63[^&]", password);
        for (char* c = password; *c; c++) if (*c == '+') *c = ' ';
    }

    // Use manual SSID if selected
    const char* final_ssid = (strcmp(ssid_sel, "__manual__") == 0 || strlen(ssid_sel) == 0)
                              ? ssid_manual : ssid_sel;

    ESP_LOGI(TAG, "Provision request for SSID: '%s'", final_ssid);

    // if (strlen(final_ssid) == 0) {
    //     static char err_html[1024];
    //     snprintf(err_html, sizeof(err_html),
    //         "<!DOCTYPE html><html><head>"
    //         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    //         "<style>%s</style></head><body>"
    //         "<h1>&#128268; ChromaVertex Setup</h1>"
    //         "<div class='msg err'>Please select or enter a network name.</div>"
    //         "<a href='/'><button>Try Again</button></a>"
    //         "</body></html>", CSS);
    //     httpd_resp_set_type(req, "text/html");
    //     httpd_resp_send(req, err_html, HTTPD_RESP_USE_STRLEN);
    //     return ESP_OK;
    // }
    // Empty SSID error
    if (strlen(final_ssid) == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>");
        httpd_resp_sendstr_chunk(req, CSS);
        httpd_resp_sendstr_chunk(req, "</style></head><body>"
            "<h1>&#128268; ChromaVertex Setup</h1>"
            "<div class='msg err'>Please select or enter a network name.</div>"
            "<a href='/'><button>Try Again</button></a>"
            "</body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    // Attempt connection
    bool ok = wifi_manager_provision(final_ssid, password);

    static char result_html[2048];
    if (ok) {
        snprintf(result_html, sizeof(result_html),
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>%s</style></head><body>"
            "<h1>&#128268; ChromaVertex Setup</h1>"
            "<div class='msg ok'>&#10003; Connected to '%s'!<br>"
            "Credentials saved. Rebooting in 3 seconds...</div>"
            "<script>setTimeout(function(){},3000);</script>"
            "</body></html>", CSS, final_ssid);
    } else {
        snprintf(result_html, sizeof(result_html),
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>%s</style></head><body>"
            "<h1>&#128268; ChromaVertex Setup</h1>"
            "<div class='msg err'>&#10007; Could not connect to '%s'.<br>"
            "Check the password and try again.</div>"
            "<a href='/'><button>Try Again</button></a>"
            "</body></html>", CSS, final_ssid);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, result_html, HTTPD_RESP_USE_STRLEN);

    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET / — Normal control page
// ---------------------------------------------------------------------------
static esp_err_t handle_root(httpd_req_t* req)
{
    if (!check_auth(req)) return send_auth_challenge(req);

    int  current_seq = led_manager_get_sequence();
    char seq_name[32];
    led_manager_sequence_name(current_seq, seq_name, sizeof(seq_name));

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);

    static char html[4096];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Beacon1</title>"
        "<style>%s</style></head><body>"
        "<h1>&#127881; ChromaVertex Beacon1</h1>"
        "<div class='ver'>Firmware: %s | %s</div>"

        "<h2>LED Sequence</h2>"
        "<form method='POST' action='/set'>"
        "  <label>Sequence (1 - 38)</label>"
        "  <input type='number' name='seq' min='1' max='38' value='%d'>"
        "  <button type='submit'>Set Sequence</button>"
        "</form>"
        "<div class='status'>"
        "  Active: <span class='seq-name'>%d - %s</span><br>"
        "  1-32: de Bruijn patterns | 33-38: Solid colour (CMYGRB)"
        "</div>"

        "<h2>Firmware Update</h2>"
        "<input type='file' id='fw' accept='.bin'>"
        "<button class='warn' onclick='uploadFw()'>Upload Firmware</button>"
        "<div id='ota-progress'><div id='ota-bar'></div></div>"
        "<div id='ota-status'></div>"

        "<h2>WiFi</h2>"
        "<form method='POST' action='/forget'>"
        "  <button class='danger' type='submit' "
        "    onclick=\"return confirm('Clear saved WiFi and reboot to setup?')\">"
        "    Forget WiFi &amp; Re-provision"
        "  </button>"
        "</form>"

        "<script>"
        "function uploadFw(){"
        "  var f=document.getElementById('fw').files[0];"
        "  if(!f){alert('Select a .bin file first');return;}"
        "  var s=document.getElementById('ota-status');"
        "  var p=document.getElementById('ota-progress');"
        "  var b=document.getElementById('ota-bar');"
        "  s.textContent='Uploading...';"
        "  p.style.display='block';"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/ota',true);"
        "  xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "  xhr.upload.onprogress=function(e){"
        "    if(e.lengthComputable)b.style.width=(e.loaded/e.total*100)+'%%';"
        "  };"
        "  xhr.onload=function(){"
        "    if(xhr.status==200){s.textContent='Done! Rebooting...';b.style.width='100%%';}"
        "    else{s.textContent='Error: '+xhr.responseText;}"
        "  };"
        "  xhr.onerror=function(){s.textContent='Upload failed.';};"
        "  xhr.send(f);"
        "}"
        "</script>"
        "</body></html>",
        CSS,
        app_desc.version, app_desc.date,
        current_seq, current_seq, seq_name
    );

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /set
// ---------------------------------------------------------------------------
static esp_err_t handle_set(httpd_req_t* req)
{
    if (!check_auth(req)) return send_auth_challenge(req);

    char buf[64] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty"); return ESP_OK; }
    buf[received] = '\0';

    int val = 0;
    if (sscanf(buf, "seq=%d", &val) == 1) {
        led_manager_set_sequence(val);
        ESP_LOGI(TAG, "Sequence set to %d via web", val);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /status
// ---------------------------------------------------------------------------
static esp_err_t handle_status(httpd_req_t* req)
{
    if (!check_auth(req)) return send_auth_challenge(req);

    int  seq = led_manager_get_sequence();
    char name[32];
    led_manager_sequence_name(seq, name, sizeof(name));

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);

    static char json[512];
    snprintf(json, sizeof(json),
        "{\"sequence\":%d,\"name\":\"%s\",\"max_sequences\":%d,"
        "\"firmware\":\"%s\",\"date\":\"%s\",\"partition\":\"%s\","
        "\"provisioning\":%s}",
        seq, name, BEACON_NUM_SEQUENCES,
        app_desc.version, app_desc.date, running->label,
        wifi_manager_is_provisioning() ? "true" : "false"
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /forget — Clear WiFi credentials and reboot to provisioning
// ---------------------------------------------------------------------------
static esp_err_t handle_forget(httpd_req_t* req)
{
    if (!check_auth(req)) return send_auth_challenge(req);

    ESP_LOGI(TAG, "Clearing WiFi credentials on user request");
    wifi_manager_clear_credentials();

    httpd_resp_sendstr(req, "Credentials cleared. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /ota
// ---------------------------------------------------------------------------
static esp_err_t handle_ota(httpd_req_t* req)
{
    if (!check_auth(req)) return send_auth_challenge(req);

    ESP_LOGI(TAG, "OTA update started, size: %d bytes", req->content_len);

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_OK;
    }

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    static char buf[1024];
    int remaining = req->content_len;

    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_OK;
        }
        if (esp_ota_write(ota_handle, buf, received) != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_OK;
        }
        remaining -= received;
    }

    if (esp_ota_end(ota_handle) != ESP_OK ||
        esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalise failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA complete — rebooting");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Start server — registers different handlers depending on mode
// ---------------------------------------------------------------------------
void web_server_start(void)
{
    httpd_config_t config     = HTTPD_DEFAULT_CONFIG();
    config.server_port        = 80;
    config.max_uri_handlers   = 12;
    config.stack_size         = 8192;
    config.recv_wait_timeout  = 30;
    config.send_wait_timeout  = 30;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    if (wifi_manager_is_provisioning()) {
        // Provisioning mode — only serve setup page
        httpd_uri_t provision_page = { .uri="/",          .method=HTTP_GET,  .handler=handle_provision_page, .user_ctx=NULL };
        httpd_uri_t provision_post = { .uri="/provision", .method=HTTP_POST, .handler=handle_provision,      .user_ctx=NULL };
        httpd_uri_t status_uri     = { .uri="/status",    .method=HTTP_GET,  .handler=handle_status,         .user_ctx=NULL };

        httpd_register_uri_handler(server, &provision_page);
        httpd_register_uri_handler(server, &provision_post);
        httpd_register_uri_handler(server, &status_uri);

        ESP_LOGI(TAG, "Provisioning server started — http://192.168.4.1");
    } else {
        // Normal mode — full control interface
        httpd_uri_t root_uri   = { .uri="/",       .method=HTTP_GET,  .handler=handle_root,   .user_ctx=NULL };
        httpd_uri_t set_uri    = { .uri="/set",     .method=HTTP_POST, .handler=handle_set,    .user_ctx=NULL };
        httpd_uri_t status_uri = { .uri="/status",  .method=HTTP_GET,  .handler=handle_status, .user_ctx=NULL };
        httpd_uri_t ota_uri    = { .uri="/ota",     .method=HTTP_POST, .handler=handle_ota,    .user_ctx=NULL };
        httpd_uri_t forget_uri = { .uri="/forget",  .method=HTTP_POST, .handler=handle_forget, .user_ctx=NULL };

        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &set_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &ota_uri);
        httpd_register_uri_handler(server, &forget_uri);

        ESP_LOGI(TAG, "Control server started on port 80");
    }
}
