#pragma once

/* Copy to grok_local_config.h (Git-ignored). Keep the copy and firmware private.
 * Configuration is compiled into firmware; this is not secure secret storage.
 * The URL is an HTTPS origin with no path, query, or trailing slash.
 * DEVICE_ID and DEVICE_TOKEN must match the gateway's configured device.
 * No xAI API key or Grok Bot account/session credential is used here.
 */
#define GROK_WIFI_SSID ""
#define GROK_WIFI_PASSWORD ""
#define GROK_GATEWAY_URL ""
#define GROK_DEVICE_ID "passport-01"
#define GROK_DEVICE_TOKEN ""
