#pragma once

#include "esp_err.h"
#include <stdbool.h>

/*
 * WiFi Manager
 *
 * Credential load order (first non-empty wins):
 *   1. NVS namespace "smartload_cfg", then legacy namespace, keys "wifi_ssid" / "wifi_pass"
 *   2. /wifi.json  {"ssid":"...","password":"..."}
 *   3. DEFAULT_WIFI_SSID / DEFAULT_WIFI_PASS  (build-time fallback)
 *
 * Usage:
 *   wifi_mgr_init();        — register event handlers, init driver
 *   wifi_mgr_connect();     — load creds, start, block ≤30 s for IP
 *   wifi_mgr_is_connected() — non-blocking state poll
 */

/** Initialise WiFi driver and register event handlers.
 *  Calls esp_netif_init() and esp_event_loop_create_default() internally
 *  (safe to call even if already done — errors are ignored). */
esp_err_t wifi_mgr_init(void);

/** Load credentials, start WiFi STA, block until IP acquired or 30 s timeout.
 *  @return ESP_OK on IP obtained, ESP_ERR_NOT_FOUND if no credentials,
 *          ESP_FAIL on auth failure, ESP_ERR_TIMEOUT on timeout. */
esp_err_t wifi_mgr_connect(void);

/** Disconnect and stop WiFi. */
esp_err_t wifi_mgr_disconnect(void);

/** Non-blocking connected state. */
bool wifi_mgr_is_connected(void);

/** Persist new credentials to NVS and /wifi.json.
 *  Does NOT reconnect — caller should call wifi_mgr_connect() again if needed. */
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password);
