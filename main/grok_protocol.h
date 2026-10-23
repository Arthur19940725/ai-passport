#pragma once

#include <stdbool.h>
#include <stddef.h>

#define GROK_MESSAGE_MAX 160
#define GROK_COMMAND_ID_LEN 36
#define GROK_DEVICE_ID_MAX 32
#define GROK_BRIGHTNESS_MIN 10
#define GROK_BRIGHTNESS_MAX 100

typedef enum {
    GROK_COMMAND_SHOW_MESSAGE,
    GROK_COMMAND_CLEAR_MESSAGE,
    GROK_COMMAND_SET_BACKLIGHT,
} grok_command_kind_t;

typedef struct {
    char id[GROK_COMMAND_ID_LEN + 1];
    grok_command_kind_t kind;
    char text[GROK_MESSAGE_MAX + 1];
    int brightness;
} grok_command_t;

/* Byte-bounded validation shared with host tests; no ESP-IDF or LVGL needed. */
bool grok_device_id_valid(const char *value);
bool grok_command_id_valid(const char *value);
bool grok_message_valid(const char *value);
bool grok_brightness_valid(int value);
bool grok_command_valid(const grok_command_t *command);
/* Bound nesting before calling a recursive JSON parser on the small MCU stack.
 * This is a resource guard; the JSON parser must still validate the grammar. */
bool grok_json_depth_valid(const char *json);

/* The server sends one in-flight command until ACK. Only replay within this
 * boot is suppressed. A fresh boot_id tells the server to cancel old work. */
bool grok_command_is_duplicate(const grok_command_t *command, const char *last_id);
