#include "grok_network.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define GROK_ORIGIN_MAX 192
#define GROK_TOKEN_MAX 128
#define GROK_BODY_MAX 1023
#define GROK_HTTP_TIMEOUT_MS 4000
#define GROK_CONNECT_TIMEOUT_US INT64_C(15000000)
#define GROK_RETRY_MIN_US INT64_C(1000000)
#define GROK_RETRY_MAX_US INT64_C(30000000)
#define GROK_TIME_MIN ((time_t)1704067200) /* 2024-01-01 UTC */

#define NET_HAS_IP (1U << 0)
#define NET_DISCONNECTED (1U << 1)
#define NET_LOST_IP (1U << 2)

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_wifi_registered;
static bool s_ip_registered;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_sntp_initialized;
static bool s_sntp_started;
static bool s_clock_synchronized;
static bool s_started;
static bool s_connecting;
static bool s_was_online;
static int64_t s_connect_deadline_us;
static int64_t s_next_connect_us;
static int64_t s_retry_us = GROK_RETRY_MIN_US;
static char s_poll_url[GROK_ORIGIN_MAX + sizeof("/device/poll")];
static char s_authorization[sizeof("Bearer ") + GROK_TOKEN_MAX];
static const char *s_status = "NOT_STARTED";

static bool ascii_alphanumeric(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

static bool valid_origin(const char *url)
{
    if (!url) return false;
    size_t length = strnlen(url, GROK_ORIGIN_MAX + 1);
    if (length <= 8 || length > GROK_ORIGIN_MAX ||
        strncmp(url, "https://", 8) != 0) return false;

    const char *host = url + 8;
    const char *cursor = host;
    size_t label_length = 0;
    while (*cursor && *cursor != ':') {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '.') {
            if (!label_length || cursor[-1] == '-') return false;
            label_length = 0;
        } else {
            if (!ascii_alphanumeric(ch) && ch != '-') return false;
            if (!label_length && ch == '-') return false;
            if (++label_length > 63) return false;
        }
        ++cursor;
    }
    if (!label_length || cursor == host || cursor[-1] == '-') return false;
    if (*cursor == ':') {
        unsigned int port = 0;
        const char *digits = ++cursor;
        while (*cursor) {
            if (*cursor < '0' || *cursor > '9' || cursor - digits >= 5) return false;
            port = port * 10 + (unsigned int)(*cursor++ - '0');
        }
        if (cursor == digits || !port || port > 65535) return false;
    }
    return true;
}

static bool valid_token(const char *token)
{
    if (!token) return false;
    size_t length = strnlen(token, GROK_TOKEN_MAX + 1);
    if (length < 32 || length > GROK_TOKEN_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)token[i];
        if (!ascii_alphanumeric(ch) && ch != '_' && ch != '-') return false;
    }
    return true;
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, NET_HAS_IP);
        xEventGroupSetBits(s_events, NET_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, NET_HAS_IP);
        xEventGroupSetBits(s_events, NET_LOST_IP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event && event->esp_netif == s_sta_netif && event->ip_info.ip.addr) {
            xEventGroupSetBits(s_events, NET_HAS_IP);
        }
    }
}

static void cleanup_startup(void)
{
    /* Unregister before deleting the event group; callbacks use no UI objects. */
    if (s_wifi_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              s_wifi_handler);
        s_wifi_registered = false;
    }
    if (s_ip_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler);
        s_ip_registered = false;
    }
    if (s_sntp_initialized) {
        esp_netif_sntp_deinit();
        s_sntp_initialized = s_sntp_started = false;
    }
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    memset(s_authorization, 0, sizeof(s_authorization));
    memset(s_poll_url, 0, sizeof(s_poll_url));
    /* NVS, esp-netif, and the default event loop are application-global services. */
}

esp_err_t grok_network_start(const char *ssid, const char *password,
                             const char *gateway_url, const char *device_token)
{
    if (s_started) return ESP_ERR_INVALID_STATE;
    if (!ssid || !password || strnlen(ssid, 33) == 0 || strnlen(ssid, 33) > 32 ||
        strnlen(password, 64) > 63 || !valid_origin(gateway_url) ||
        !valid_token(device_token)) {
        s_status = "CONFIG_INVALID";
        return ESP_ERR_INVALID_ARG;
    }

    s_status = "NETWORK_STARTING";
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        s_status = "NVS_FAILED";
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    /* Do not replace a netif owned by a different application subsystem. */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    s_events = xEventGroupCreate();
    if (!s_events) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&netif_config);
    if (!s_sta_netif) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_netif_attach_wifi_station(s_sta_netif);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) goto fail;
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) goto fail;
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              network_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    s_wifi_registered = true;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              network_event, NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail;
    s_ip_registered = true;

    wifi_config_t station = { 0 };
    memcpy(station.sta.ssid, ssid, strlen(ssid));
    memcpy(station.sta.password, password, strlen(password));
    station.sta.pmf_cfg.capable = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &station);
    memset(&station, 0, sizeof(station));
    if (err != ESP_OK) goto fail;

    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    time_config.start = false;
    time_config.wait_for_sync = true;
    err = esp_netif_sntp_init(&time_config);
    if (err != ESP_OK) goto fail;
    s_sntp_initialized = true;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;

    snprintf(s_poll_url, sizeof(s_poll_url), "%s/device/poll", gateway_url);
    snprintf(s_authorization, sizeof(s_authorization), "Bearer %s", device_token);
    s_started = true;
    s_status = "WIFI_CONNECTING";
    return ESP_OK;

fail:
    cleanup_startup();
    s_status = "NETWORK_START_FAILED";
    return err;
}

static void schedule_reconnect(int64_t now_us)
{
    s_connecting = false;
    s_next_connect_us = now_us + s_retry_us;
    s_retry_us *= 2;
    if (s_retry_us > GROK_RETRY_MAX_US) s_retry_us = GROK_RETRY_MAX_US;
    s_status = "WIFI_RETRY";
}

void grok_network_service(int64_t now_us)
{
    if (!s_started) return;
    EventBits_t changes = xEventGroupWaitBits(s_events, NET_DISCONNECTED | NET_LOST_IP,
                                              pdTRUE, pdFALSE, 0);
    bool online = (xEventGroupGetBits(s_events) & NET_HAS_IP) != 0;
    if (!online) {
        s_was_online = false;
        if (changes & NET_LOST_IP) {
            /* A lost DHCP address can leave the station associated. */
            (void)esp_wifi_disconnect();
        }
        if (changes & (NET_DISCONNECTED | NET_LOST_IP)) {
            schedule_reconnect(now_us);
        }
        if (s_connecting && now_us >= s_connect_deadline_us) {
            (void)esp_wifi_disconnect();
            schedule_reconnect(now_us);
        }
        if (!s_connecting && now_us >= s_next_connect_us) {
            esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK) {
                s_connecting = true;
                s_connect_deadline_us = now_us + GROK_CONNECT_TIMEOUT_US;
                s_status = "WIFI_CONNECTING";
            } else {
                schedule_reconnect(now_us);
            }
        }
        return;
    }

    s_connecting = false;
    s_retry_us = GROK_RETRY_MIN_US;
    if (!s_sntp_started) {
        if (esp_netif_sntp_start() != ESP_OK) {
            s_status = "TIME_SYNC_FAILED";
            return;
        }
        s_sntp_started = true;
    }
    if (!s_clock_synchronized && esp_netif_sntp_sync_wait(0) == ESP_OK) {
        s_clock_synchronized = true;
    }
    if (!s_clock_synchronized || time(NULL) < GROK_TIME_MIN) {
        s_status = "TIME_SYNCING";
        return;
    }
    if (!s_was_online) s_status = "GATEWAY_READY";
    s_was_online = true;
}

bool grok_network_ready(void)
{
    return s_started && s_clock_synchronized && time(NULL) >= GROK_TIME_MIN &&
           (xEventGroupGetBits(s_events) & NET_HAS_IP) != 0;
}

static esp_err_t set_remaining_timeout(esp_http_client_handle_t client, int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) return ESP_ERR_TIMEOUT;
    int timeout_ms = (int)((remaining_us + 999) / 1000);
    if (timeout_ms > GROK_HTTP_TIMEOUT_MS) timeout_ms = GROK_HTTP_TIMEOUT_MS;
    return esp_http_client_set_timeout_ms(client, timeout_ms);
}

esp_err_t grok_network_exchange(const char *request_json, char *response, size_t capacity)
{
    if (!response || capacity < 2) return ESP_ERR_INVALID_ARG;
    response[0] = '\0';
    if (!request_json) return ESP_ERR_INVALID_ARG;
    size_t request_length = strnlen(request_json, GROK_BODY_MAX + 1);
    if (!request_length || request_length > GROK_BODY_MAX) return ESP_ERR_INVALID_SIZE;
    if (!grok_network_ready()) return ESP_ERR_INVALID_STATE;
    size_t limit = capacity - 1;
    if (limit > GROK_BODY_MAX) limit = GROK_BODY_MAX;

    esp_http_client_config_t config = {
        .url = s_poll_url,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = GROK_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        s_status = "HTTP_NO_MEMORY";
        return ESP_ERR_NO_MEM;
    }
    s_status = "GATEWAY_CONNECTING";
    int64_t deadline_us = esp_timer_get_time() + GROK_HTTP_TIMEOUT_MS * INT64_C(1000);
    esp_err_t err = esp_http_client_set_header(client, "Authorization", s_authorization);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept", "application/json");
    if (err == ESP_OK) err = set_remaining_timeout(client, deadline_us);
    if (err == ESP_OK) err = esp_http_client_open(client, (int)request_length);
    if (err != ESP_OK) goto done;

    size_t sent = 0;
    while (sent < request_length) {
        err = set_remaining_timeout(client, deadline_us);
        if (err != ESP_OK) goto done;
        int count = esp_http_client_write(client, request_json + sent, (int)(request_length - sent));
        if (count <= 0) {
            err = ESP_FAIL;
            goto done;
        }
        sent += (size_t)count;
    }
    err = set_remaining_timeout(client, deadline_us);
    if (err != ESP_OK) goto done;
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        err = ESP_FAIL;
        goto done;
    }
    int http_status = esp_http_client_get_status_code(client);
    if (http_status != 200) {
        s_status = (http_status == 401 || http_status == 403) ? "GATEWAY_AUTH_FAILED" :
                   (http_status >= 300 && http_status < 400) ? "GATEWAY_REDIRECT_REJECTED" :
                   "GATEWAY_HTTP_ERROR";
        err = ESP_FAIL;
        goto done;
    }
    if ((uint64_t)content_length > limit) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    size_t received = 0;
    /* fetch_headers may have cached the entire body already. Always drain that
     * cache with read(), even when is_complete_data_received() is already true.
     */
    while (true) {
        err = set_remaining_timeout(client, deadline_us);
        if (err != ESP_OK) goto done;
        char overflow_byte;
        char *target = received < limit ? response + received : &overflow_byte;
        size_t available = received < limit ? limit - received : 1;
        int count = esp_http_client_read(client, target, (int)available);
        if (count < 0 || (count == 0 && !esp_http_client_is_complete_data_received(client))) {
            err = ESP_FAIL;
            goto done;
        }
        if (count == 0) break;
        if (count > 0) {
            if ((size_t)count > limit - received || memchr(target, '\0', (size_t)count)) {
                err = ESP_ERR_INVALID_SIZE;
                goto done;
            }
            received += (size_t)count;
        }
    }
    if (!received) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    response[received] = '\0';
    s_status = "GATEWAY_ONLINE";

done:
    if (err != ESP_OK) {
        response[0] = '\0';
        if (strcmp(s_status, "GATEWAY_CONNECTING") == 0) {
            s_status = (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_RESPONSE) ?
                       "GATEWAY_RESPONSE_INVALID" : "GATEWAY_UNREACHABLE";
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

const char *grok_network_status(void)
{
    return s_status;
}
