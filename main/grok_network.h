#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* All APIs belong to one application worker; they must not run on the LVGL task.
 * Start once per application lifetime. Credentials remain in RAM; failed NVS
 * initialization never erases stored data. gateway_url is an HTTPS origin with
 * a DNS/IPv4 host and optional port, without a path or trailing slash.
 */
esp_err_t grok_network_start(const char *ssid, const char *password,
                             const char *gateway_url, const char *device_token);

/* Call regularly using monotonic microseconds (esp_timer_get_time()). */
void grok_network_service(int64_t now_us);

/* Requires both an IPv4 address and an SNTP-synchronized, plausible clock. */
bool grok_network_ready(void);

/* One POST to /device/poll, with no automatic retry or redirect. Request and
 * response bodies are at most 1023 bytes. On success response is NUL-terminated;
 * on failure it is empty. A caller must validate the returned JSON schema.
 */
esp_err_t grok_network_exchange(const char *request_json, char *response,
                                size_t capacity);

/* Stable ASCII status code. This does not expose credentials or server text. */
const char *grok_network_status(void);
