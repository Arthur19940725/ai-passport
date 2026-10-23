#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "grok_network.h"
#include "grok_protocol.h"
#include "key_guard.h"

#if __has_include("grok_local_config.h")
#include "grok_local_config.h"
#else
#include "grok_local_config.example.h"
#endif

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define POLL_INTERVAL_US (2LL * 1000 * 1000)
#define IDLE_DIM_US (60LL * 1000 * 1000)
#define KEY_GUARD_US (10LL * 1000 * 1000)

static const char *TAG = "grok_device";

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t event;
} key_event_t;

typedef struct {
    char json[640];
    bool reported_pause;
} network_request_t;

typedef struct {
    char json[1024];
    esp_err_t error;
    const char *status; /* Only pointers to static status strings cross queues. */
    bool reported_pause;
} network_result_t;

typedef struct {
    bool configured;
    bool ready;
    const char *status;
    char boot_id[17];
} network_status_t;

/* The control worker owns application state and BSP/UI work. A separate
 * network worker owns all network APIs: slow TLS/DNS/HTTP cannot stall local
 * pause or brightness controls. Queues copy snapshots, never LVGL pointers.
 * Screens and queues live for the whole boot and are never deleted in use. */
static QueueHandle_t s_keys;
static QueueHandle_t s_requests;
static QueueHandle_t s_results;
static QueueHandle_t s_link_status;
static key_guard_t s_key_guard;
static lv_obj_t *s_status_label;
static lv_obj_t *s_message_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_control_label;
static bool s_remote_enabled;
static bool s_pause_report_pending;
static bool s_configured;
static bool s_request_pending;
static bool s_dimmer_active;
static int s_brightness = 60;
static int s_requested_brightness = 60;
static int s_battery = -1;
static int s_battery_mv = -1;
static int64_t s_last_activity;
static char s_boot_id[17];
static char s_message[GROK_MESSAGE_MAX + 1] = "Waiting for your Bot.";
static char s_last_id[GROK_COMMAND_ID_LEN + 1];
static bool s_last_ok = true;
static const char *s_last_error = "";
static const char *s_status = "Starting";

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int width,
                            const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* Caller holds the LVGL lock. This screen is independent from ui_pixel/demo UI. */
static void create_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101B27), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = make_label(screen, 20, 20, 155, &lv_font_montserrat_14, 0x90A7B8);
    lv_label_set_text(title, "AI PASSPORT");
    s_battery_label = make_label(screen, 177, 20, 45, &lv_font_montserrat_14, 0x90A7B8);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_battery_label, "--");

    title = make_label(screen, 20, 53, 210, &lv_font_montserrat_20, 0xF2F8FF);
    lv_label_set_text(title, "Grok Bot Link");
    s_status_label = make_label(screen, 20, 88, 200, &lv_font_montserrat_14, 0x6EE7BD);
    lv_obj_set_height(s_status_label, 38);
    lv_label_set_text(s_status_label, "Starting");

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_pos(panel, 16, 134);
    lv_obj_set_size(panel, 208, 120);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1B2D3D), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    s_message_label = make_label(panel, 0, 0, 186, &lv_font_montserrat_14, 0xF2F8FF);
    lv_label_set_long_mode(s_message_label, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_size(s_message_label, 186, 98);
    lv_label_set_text(s_message_label, s_message);

    s_control_label = make_label(screen, 20, 267, 200, &lv_font_montserrat_14, 0x90A7B8);
    lv_label_set_text(s_control_label, "OK: pause remote\nUP/DOWN: brightness");
    lv_screen_load(screen);
}

static void render_status(void) {
    if (!bsp_lvgl_lock(50)) return;
    lv_label_set_text_fmt(s_status_label, "%s\nRemote %s | Light %d%%",
                          s_status, s_remote_enabled ? "ON" : "OFF", s_brightness);
    if (s_battery < 0) lv_label_set_text(s_battery_label, "--");
    else lv_label_set_text_fmt(s_battery_label, "%d%%", s_battery);
    if (s_pause_report_pending) {
        lv_label_set_text(s_control_label, "Paused; syncing stop\nUP/DOWN: brightness");
    } else {
        lv_label_set_text_fmt(s_control_label, "OK: %s remote\nUP/DOWN: brightness",
                              s_remote_enabled ? "pause" : "resume");
    }
    bsp_lvgl_unlock();
}

static bool set_message(const char *message) {
    if (!bsp_lvgl_lock(100)) return false;
    snprintf(s_message, sizeof(s_message), "%s", message);
    lv_label_set_text(s_message_label, s_message);
    bsp_lvgl_unlock();
    return true;
}

static void wake_backlight(int64_t now) {
    s_last_activity = now;
    if (s_dimmer_active) {
        s_brightness = s_requested_brightness;
        bsp_display_backlight((uint8_t)s_brightness);
        s_dimmer_active = false;
    }
}

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *user) {
    (void)user;
    if (key_guard_consume(&s_key_guard, event == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (event != BSP_BTN_CLICK && event != BSP_BTN_LONG) return;
    const key_event_t input = { .button = button, .event = event };
    (void)xQueueSend(s_keys, &input, 0);
}

static void process_keys(void) {
    key_event_t input;
    while (xQueueReceive(s_keys, &input, 0) == pdTRUE) {
        wake_backlight(esp_timer_get_time());
        if (input.button == BSP_BTN_OK) {
            if (input.event == BSP_BTN_CLICK) {
                if (s_remote_enabled) {
                    s_remote_enabled = false;
                    s_pause_report_pending = true;
                } else if (s_configured && !s_pause_report_pending) {
                    s_remote_enabled = true;
                } else {
                    s_status = "Pause sync pending";
                }
            } else if (input.event == BSP_BTN_LONG && !set_message("")) s_status = "Display busy";
        } else if (input.event == BSP_BTN_CLICK) {
            int value = s_requested_brightness + (input.button == BSP_BTN_UP ? 10 : -10);
            if (value < GROK_BRIGHTNESS_MIN) value = GROK_BRIGHTNESS_MIN;
            if (value > GROK_BRIGHTNESS_MAX) value = GROK_BRIGHTNESS_MAX;
            s_brightness = s_requested_brightness = value;
            bsp_display_backlight((uint8_t)value);
        }
    }
}

static bool json_integer(const cJSON *value, int minimum, int maximum) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= minimum && value->valuedouble <= maximum &&
           value->valuedouble == (int)value->valuedouble;
}

static void acknowledge(const char *id, bool ok, const char *error) {
    snprintf(s_last_id, sizeof(s_last_id), "%s", id);
    s_last_ok = ok;
    s_last_error = error;
}

/* No automatic retries of an already applied command: every poll carries its
 * previous ACK, and an in-flight resend only reuses that ACK. Reboot discards
 * this ledger and changes boot_id, so the gateway invalidates older commands. */
static bool handle_response(const char *response, bool reported_pause) {
    if (!grok_json_depth_valid(response) || strstr(response, "\\u0000")) {
        s_status = "Invalid gateway response";
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(response, NULL, true);
    if (!cJSON_IsObject(root) ||
        !json_integer(cJSON_GetObjectItemCaseSensitive(root, "version"), 1, 1)) {
        s_status = "Invalid gateway response";
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (cJSON_IsNull(item)) {
        if (reported_pause && !s_remote_enabled) s_pause_report_pending = false;
        cJSON_Delete(root);
        return true;
    }
    const cJSON *id = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "id") : NULL;
    if (!cJSON_IsString(id) || !grok_command_id_valid(id->valuestring)) {
        s_status = "Invalid command ID";
        cJSON_Delete(root);
        return false;
    }
    grok_command_t command = {0};
    snprintf(command.id, sizeof(command.id), "%s", id->valuestring);
    if (grok_command_is_duplicate(&command, s_last_id)) {
        cJSON_Delete(root);
        return true;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    bool valid = cJSON_IsString(type);
    if (valid && strcmp(type->valuestring, "show_message") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        command.kind = GROK_COMMAND_SHOW_MESSAGE;
        valid = cJSON_IsString(text) && grok_message_valid(text->valuestring);
        if (valid) snprintf(command.text, sizeof(command.text), "%s", text->valuestring);
    } else if (valid && strcmp(type->valuestring, "clear_message") == 0) {
        command.kind = GROK_COMMAND_CLEAR_MESSAGE;
    } else if (valid && strcmp(type->valuestring, "set_backlight") == 0) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "brightness");
        command.kind = GROK_COMMAND_SET_BACKLIGHT;
        valid = json_integer(value, GROK_BRIGHTNESS_MIN, GROK_BRIGHTNESS_MAX);
        if (valid) command.brightness = value->valueint;
    } else {
        valid = false;
    }
    if (!valid || !grok_command_valid(&command)) {
        acknowledge(command.id, false, "invalid_command");
    } else if (!s_remote_enabled) {
        acknowledge(command.id, false, "remote_disabled");
    } else {
        bool applied = true;
        switch (command.kind) {
        case GROK_COMMAND_SHOW_MESSAGE:
            applied = set_message(command.text);
            break;
        case GROK_COMMAND_CLEAR_MESSAGE:
            applied = set_message("");
            break;
        case GROK_COMMAND_SET_BACKLIGHT:
            s_brightness = s_requested_brightness = command.brightness;
            s_dimmer_active = false;
            bsp_display_backlight((uint8_t)s_brightness);
            break;
        }
        if (applied) wake_backlight(esp_timer_get_time());
        acknowledge(command.id, applied, applied ? "" : "display_busy");
    }
    cJSON_Delete(root);
    return true;
}

static void queue_poll(void) {
    /* ID is a validated ASCII slug; boot/id/error are locally generated. No
     * message text or credentials enter this hand-built telemetry JSON. */
    network_request_t request = { .reported_pause = !s_remote_enabled };
    const int length = snprintf(request.json, sizeof(request.json),
        "{\"version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"remote_enabled\":%s,\"battery_percent\":%d,\"battery_mv\":%d,"
        "\"brightness\":%d,\"uptime_ms\":%llu,\"free_heap\":%lu,"
        "\"last_result\":{\"id\":\"%s\",\"ok\":%s,\"error\":\"%s\"}}",
        GROK_DEVICE_ID, s_boot_id, s_remote_enabled ? "true" : "false",
        s_battery, s_battery_mv, s_brightness,
        (unsigned long long)(esp_timer_get_time() / 1000),
        (unsigned long)esp_get_free_heap_size(), s_last_id,
        s_last_ok ? "true" : "false", s_last_error);
    if (length < 0 || (size_t)length >= sizeof(request.json)) {
        s_status = "Telemetry too large";
        return;
    }
    if (xQueueSend(s_requests, &request, 0) == pdTRUE) s_request_pending = true;
}

static void process_network_result(void) {
    network_result_t result;
    if (xQueueReceive(s_results, &result, 0) != pdTRUE) return;
    s_request_pending = false;
    if (result.error != ESP_OK) {
        s_status = result.status;
    } else {
        s_status = "Gateway connected";
        (void)handle_response(result.json, result.reported_pause);
    }
}

static void network_worker(void *arg) {
    (void)arg;
    const esp_err_t error = grok_device_id_valid(GROK_DEVICE_ID)
        ? grok_network_start(GROK_WIFI_SSID, GROK_WIFI_PASSWORD,
                             GROK_GATEWAY_URL, GROK_DEVICE_TOKEN)
        : ESP_ERR_INVALID_ARG;
    network_status_t status = {
        .configured = error == ESP_OK,
        .status = error == ESP_OK ? "Connecting Wi-Fi" : "Setup required",
    };
    /* Generate the boot nonce after starting Wi-Fi, when RF entropy is active. */
    snprintf(status.boot_id, sizeof(status.boot_id), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    (void)xQueueOverwrite(s_link_status, &status);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Network setup failed: %s", esp_err_to_name(error));
        vTaskDelete(NULL);
        return;
    }
    network_request_t request;
    network_result_t result;
    for (;;) {
        grok_network_service(esp_timer_get_time());
        status.ready = grok_network_ready();
        status.status = grok_network_status();
        (void)xQueueOverwrite(s_link_status, &status);
        if (xQueueReceive(s_requests, &request, 0) == pdTRUE) {
            result.reported_pause = request.reported_pause;
            result.error = grok_network_exchange(request.json, result.json, sizeof(result.json));
            result.status = grok_network_status();
            /* Exactly one request can be outstanding. The control worker keeps
             * draining this queue even when connectivity is lost. */
            (void)xQueueSend(s_results, &result, portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void worker(void *arg) {
    (void)arg;
    if (xTaskCreate(network_worker, "grok_network", 8192, NULL, 3, NULL) != pdPASS) {
        s_status = "Network allocation failed";
    }
    bool got_configuration = false;
    int64_t last_poll = -POLL_INTERVAL_US;
    int64_t last_battery = -10LL * 1000 * 1000;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        network_status_t link = {0};
        if (xQueuePeek(s_link_status, &link, 0) == pdTRUE) {
            if (!got_configuration) {
                got_configuration = true;
                s_configured = link.configured;
                s_remote_enabled = s_configured;
                memcpy(s_boot_id, link.boot_id, sizeof(s_boot_id));
                if (!s_configured) {
                    (void)set_message("Configure the private\ngrok_local_config.h\nthen rebuild firmware.");
                }
            }
            if (!link.ready) s_status = link.status;
        }
        /* Local input is applied before any command that arrived meanwhile. */
        process_keys();
        process_network_result();
        if (now - last_battery >= 10LL * 1000 * 1000) {
            s_battery = bsp_battery_soc();
            s_battery_mv = bsp_battery_mv();
            if (s_battery < 0 || s_battery > 100) s_battery = -1;
            if (s_battery_mv < 0 || s_battery_mv > 6000) s_battery_mv = -1;
            last_battery = now;
        }
        if (link.ready && !s_request_pending && now - last_poll >= POLL_INTERVAL_US) {
            queue_poll();
            last_poll = now;
        }
        if (s_request_pending && now - last_poll >= 6LL * 1000 * 1000) {
            s_status = "Gateway not responding";
        }
        if (!s_dimmer_active && now - s_last_activity >= IDLE_DIM_US) {
            s_brightness = GROK_BRIGHTNESS_MIN;
            bsp_display_backlight((uint8_t)s_brightness);
            s_dimmer_active = true;
        }
        render_status();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    /* This application never starts microphone capture or the audio codec. */
    esp_err_t error = bsp_i2c_init();
    if (error != ESP_OK || bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    bsp_display_backlight((uint8_t)s_brightness);
    (void)bsp_battery_init();
    s_last_activity = esp_timer_get_time();
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "Display lock failed");
        return;
    }
    create_screen();
    bsp_lvgl_unlock();
    s_keys = xQueueCreate(8, sizeof(key_event_t));
    s_requests = xQueueCreate(1, sizeof(network_request_t));
    s_results = xQueueCreate(1, sizeof(network_result_t));
    s_link_status = xQueueCreate(1, sizeof(network_status_t));
    if (!s_keys || !s_requests || !s_results || !s_link_status) {
        s_status = "Input allocation failed";
        render_status();
        /* No producer has started yet, so partial queues can be released. */
        if (s_keys) vQueueDelete(s_keys);
        if (s_requests) vQueueDelete(s_requests);
        if (s_results) vQueueDelete(s_results);
        if (s_link_status) vQueueDelete(s_link_status);
        return;
    }
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_US);
    }
    error = bsp_button_init(on_key, NULL);
    if (error != ESP_OK) {
        s_status = "Buttons unavailable";
        (void)set_message("Remote access is OFF.\nRestart after fixing\nthe button input.");
        render_status();
        return;
    }
    if (s_key_guard.armed) {
        key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }
    if (xTaskCreate(worker, "grok_control", 6144, NULL, 4, NULL) != pdPASS) {
        s_status = "Worker allocation failed";
        render_status();
    }
}
