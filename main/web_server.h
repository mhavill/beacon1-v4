#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * web_server.h
 *
 * Lightweight HTTP server providing a control UI for the beacon.
 *
 * Equivalent ESPHome yaml:
 *
 *   web_server:
 *     port: 80
 *     auth:
 *       username: <beacon1_web_username>
 *       password: <beacon1_web_password>
 *
 * Endpoints:
 *   GET  /          — Control page with sequence selector
 *   POST /set       — Set sequence number (form param: seq=N)
 *   GET  /status    — JSON status response
 */

/**
 * Start the HTTP server.
 * Must be called after WiFi is connected.
 */
void web_server_start(void);

#ifdef __cplusplus
}
#endif
