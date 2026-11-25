#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * led_manager.h
 *
 * Controls the WS2812B LED strip for the ChromaVertex beacon.
 *
 * Equivalent ESPHome yaml:
 *
 *   light:
 *     - platform: esp32_rmt_led_strip
 *       pin: GPIO4
 *       num_leds: 16
 *       chipset: WS2811
 *       channel_colors: GRB
 *
 * Physical layout:
 *   16 LEDs total — even indices (0,2,4...14) are active,
 *   odd indices (1,3,5...15) are covered by opaque separators
 *   and are always set to off.
 *
 * Sequences 1-32:  de Bruijn colour sequences (4 colours, 8 positions)
 * Sequences 33-38: all 8 active LEDs in a single solid colour (CMYGRB)
 */

#define BEACON_NUM_LEDS      16
#define BEACON_ACTIVE_LEDS   8
#define BEACON_LED_PIN       GPIO_NUM_4
#define BEACON_NUM_SEQUENCES 38

/**
 * Initialise the LED strip and start the 500ms update timer.
 * Must be called after app_main() initialisation.
 */
void led_manager_init(void);

/**
 * Set the active sequence (1-38).
 * Takes effect on the next timer tick (within 500ms).
 */
void led_manager_set_sequence(int sequence_number);

/**
 * Returns the currently active sequence number (1-38).
 */
int led_manager_get_sequence(void);

/**
 * Returns a human-readable description of a sequence number.
 * Caller provides buffer; returns pointer to buf.
 */
const char* led_manager_sequence_name(int seq, char* buf, int buf_len);

#ifdef __cplusplus
}
#endif
