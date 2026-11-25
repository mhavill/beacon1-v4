/**
 * led_manager.cpp
 *
 * WS2812B LED strip control for ChromaVertex arena beacon.
 *
 * Implements the addressable_lambda effect from the ESPHome YAML,
 * driven by a 500ms esp_timer callback rather than ESPHome's
 * update_interval mechanism.
 *
 * Colour palette (gamma-balanced CMYG + RED + BLUE):
 *   0 = CYAN
 *   1 = MAGENTA
 *   2 = YELLOW
 *   3 = GREEN
 *   4 = RED       (solid sequences 33-36 extended)
 *   5 = BLUE
 *
 * de Bruijn sequences use colours 0-3 only.
 * Solid colour sequences map seq 33→CYAN, 34→MAGENTA,
 *   35→YELLOW, 36→GREEN, 37→RED, 38→BLUE.
 */

#include "led_manager.h"
#include "hal/gpio_types.h"
#include "led_strip.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "led_manager";

// ---------------------------------------------------------------------------
// de Bruijn sequences — 32 unique 8-position patterns over 4 colours
// Each row is one robot/beacon identity.
// Equivalent to the db[][] array in the ESPHome addressable_lambda.
// ---------------------------------------------------------------------------
static const uint8_t DB[32][8] = {
    { 0, 3, 1, 3, 3, 1, 0, 0 },
    { 0, 1, 0, 0, 1, 3, 2, 3 },
    { 1, 0, 0, 3, 1, 2, 0, 2 },
    { 2, 0, 0, 3, 0, 0, 1, 0 },
    { 2, 3, 0, 3, 3, 1, 0, 1 },
    { 3, 3, 0, 1, 3, 2, 1, 0 },
    { 2, 1, 1, 2, 3, 3, 1, 3 },
    { 3, 1, 2, 1, 3, 3, 1, 2 },
    { 0, 2, 2, 2, 2, 3, 2, 2 },
    { 0, 2, 0, 3, 1, 2, 2, 3 },
    { 3, 2, 3, 0, 3, 1, 1, 3 },
    { 0, 1, 3, 0, 0, 2, 2, 2 },
    { 3, 2, 2, 2, 1, 3, 0, 2 },
    { 0, 0, 2, 1, 2, 3, 3, 0 },
    { 1, 3, 0, 0, 0, 3, 0, 1 },
    { 1, 0, 1, 1, 0, 0, 0, 1 },
    { 2, 1, 1, 2, 2, 1, 1, 0 },
    { 3, 2, 1, 1, 1, 0, 0, 2 },
    { 3, 2, 1, 1, 3, 1, 2, 0 },
    { 0, 1, 2, 1, 1, 0, 1, 2 },
    { 2, 0, 0, 1, 1, 3, 2, 1 },
    { 3, 1, 0, 1, 0, 3, 0, 0 },
    { 0, 2, 2, 1, 0, 1, 3, 3 },
    { 1, 2, 1, 1, 2, 1, 0, 1 },
    { 0, 1, 3, 1, 1, 1, 2, 3 },
    { 0, 3, 2, 0, 3, 2, 2, 3 },
    { 1, 3, 1, 1, 2, 2, 3, 2 },
    { 2, 0, 1, 2, 0, 3, 1, 3 },
    { 2, 0, 2, 2, 3, 3, 3, 0 },
    { 2, 0, 1, 3, 3, 1, 1, 0 },
    { 2, 1, 3, 0, 3, 2, 1, 3 },
    { 1, 3, 3, 3, 2, 1, 2, 2 },
};

// ---------------------------------------------------------------------------
// Colour palette — RGB values for each colour index
// Full brightness for maximum beacon visibility
// ---------------------------------------------------------------------------
static const uint8_t COLOURS[6][3] = {
    {   0, 255, 255 },   // 0 = CYAN
    { 255,   0, 255 },   // 1 = MAGENTA
    { 255, 255,   0 },   // 2 = YELLOW
    {   0, 255,   0 },   // 3 = GREEN
    { 255,   0,   0 },   // 4 = RED
    {   0,   0, 255 },   // 5 = BLUE
};

static const char* COLOUR_NAMES[] = {
    "CYAN", "MAGENTA", "YELLOW", "GREEN", "RED", "BLUE"
};

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static led_strip_handle_t s_strip = NULL;
static volatile int       s_sequence_id = 0;   // 0-based internally

// ---------------------------------------------------------------------------
// Core render — called from timer callback
// Mirrors the addressable_lambda body exactly
// ---------------------------------------------------------------------------
static void render_sequence(void)
{
    int seq = s_sequence_id;   // 0-based

    for (int i = 0; i < BEACON_ACTIVE_LEDS; i++) {
        // Determine colour index for this LED position
        uint8_t c;
        if (seq < 32) {
            c = DB[seq][i];          // de Bruijn pattern
        } else {
            c = (uint8_t)(seq - 32); // solid colour (0-5)
        }

        int led = i * 2;   // even index = active LED

        // Active LED — set colour
        led_strip_set_pixel(s_strip, led,
            COLOURS[c][0],
            COLOURS[c][1],
            COLOURS[c][2]);

        // Separator LED — always off
        led_strip_set_pixel(s_strip, led + 1, 0, 0, 0);
    }

    led_strip_refresh(s_strip);
}

// ---------------------------------------------------------------------------
// Timer callback — equivalent to ESPHome update_interval: 500ms
// ---------------------------------------------------------------------------
static void led_timer_cb(void* arg)
{
    render_sequence();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void led_manager_init(void)
{
    ESP_LOGI(TAG, "Initialising LED strip: %d LEDs on GPIO%d",
             BEACON_NUM_LEDS, BEACON_LED_PIN);

    // Configure strip
    led_strip_config_t strip_cfg = {
        .strip_gpio_num  = BEACON_LED_PIN,
        .max_leds        = BEACON_NUM_LEDS,
        .led_model       = LED_MODEL_WS2812,
        .flags           = { .invert_out = false },
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = 10 * 1000 * 1000,   // 10 MHz
        .mem_block_symbols  = 64,
        .flags              = { .with_dma = false },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    ESP_LOGI(TAG, "LED strip initialised");

    // Initial render
    render_sequence();

    // Start 500ms periodic timer
    // Mirrors ESPHome: update_interval: 500ms
    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_args = {
        .callback        = &led_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "led_update",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 500 * 1000));   // 500ms in µs

    ESP_LOGI(TAG, "LED update timer started (500ms interval)");
}

void led_manager_set_sequence(int sequence_number)
{
    if (sequence_number < 1 || sequence_number > BEACON_NUM_SEQUENCES) {
        ESP_LOGW(TAG, "Sequence %d out of range (1-%d) — ignored",
                 sequence_number, BEACON_NUM_SEQUENCES);
        return;
    }
    s_sequence_id = sequence_number - 1;   // convert to 0-based
    ESP_LOGI(TAG, "Sequence set to %d", sequence_number);
    render_sequence();   // immediate update, don't wait for timer
}

int led_manager_get_sequence(void)
{
    return s_sequence_id + 1;   // return 1-based
}

const char* led_manager_sequence_name(int seq, char* buf, int buf_len)
{
    if (seq < 1 || seq > BEACON_NUM_SEQUENCES) {
        snprintf(buf, buf_len, "Invalid");
    } else if (seq <= 32) {
        snprintf(buf, buf_len, "de Bruijn #%d", seq);
    } else {
        int colour_idx = seq - 33;
        snprintf(buf, buf_len, "Solid %s", COLOUR_NAMES[colour_idx]);
    }
    return buf;
}
